"""Train the mic thingy denoiser.

    python train.py --train <prefix> [--train2 <prefix2>] --val <prefix> --out <dir> [--steps N] [--hidden H]

Speech banks come from prep_speech.py (disjoint speakers for train / val). Noise is synthesised on the fly
(nnlib.py), so the model never sees the same mixture twice.
"""
import argparse
import os
import time

import numpy as np
import torch
import torch.nn.functional as F
from torch.utils.data import DataLoader, IterableDataset

import nnlib as L


class Stream(IterableDataset):
    def __init__(self, prefixes, seed, fixed=0, seconds=L.SEG_S):
        self.prefixes, self.seed, self.fixed, self.seconds = prefixes, seed, fixed, seconds

    def __iter__(self):
        info = torch.utils.data.get_worker_info()
        wid = info.id if info else 0
        banks = [L.SpeechBank(p) for p in self.prefixes]
        rng = np.random.default_rng([self.seed, wid])
        count = 0
        while True:
            if self.fixed and count >= self.fixed // (info.num_workers if info else 1):
                return
            bank = banks[rng.integers(len(banks))]
            noisy, clean = L.make_example(rng, bank, self.seconds)
            yield torch.from_numpy(noisy), torch.from_numpy(clean), torch.from_numpy(L.vad_labels(clean))
            count += 1


def si_sdr(est, ref, eps=1e-8):
    est = est - est.mean(-1, keepdim=True)
    ref = ref - ref.mean(-1, keepdim=True)
    a = (est * ref).sum(-1, keepdim=True) / ((ref ** 2).sum(-1, keepdim=True) + eps)
    t = a * ref
    return 10 * torch.log10((t ** 2).sum(-1) / (((est - t) ** 2).sum(-1) + eps) + eps)


def compute_loss(model, noisy, clean, vad):
    Xraw = L.stft(noisy)
    g, vl = model(L.features(Xraw))
    scale = noisy.pow(2).mean(-1, keepdim=True).sqrt() + 1e-4
    Xn = Xraw / scale.unsqueeze(-1)
    Xc = L.stft(clean) / scale.unsqueeze(-1)
    En = L.apply_gains(Xn, g)[:, :L.NB]               # scored only where the training speech has content
    Xc = Xc[:, :L.NB]
    c = 0.3
    e = (En.abs() + 1e-7) ** c
    s = (Xc.abs() + 1e-7) ** c
    d = e - s
    l_mag = (d ** 2).mean() + (F.relu(-d) ** 2).mean()          # under-estimating speech costs double
    nfr = min(vl.shape[1], vad.shape[1])
    l_vad = F.binary_cross_entropy_with_logits(vl[:, :nfr], vad[:, :nfr])
    return l_mag * 10 + 0.2 * l_vad, l_mag.item(), l_vad.item()


@torch.no_grad()
def validate(model, val):
    model.eval()
    tot, base, vacc, n = 0.0, 0.0, 0.0, 0
    lm = 0.0
    for noisy, clean, vad in val:
        enh, v = L.enhance(model, noisy)
        m = (noisy.shape[-1] // L.HOP - 1) * L.HOP
        sl = slice(L.HOP, m)                       # skip the edge frames
        tot += si_sdr(enh[:, sl], clean[:, sl]).sum().item()
        base += si_sdr(noisy[:, sl], clean[:, sl]).sum().item()
        nfr = min(v.shape[1], vad.shape[1])
        vacc += ((v[:, :nfr] > 0.5).float() == vad[:, :nfr]).float().mean().item() * noisy.shape[0]
        lm += compute_loss(model, noisy, clean, vad)[1] * noisy.shape[0]
        n += noisy.shape[0]
    model.train()
    return tot / n, base / n, vacc / n, lm / n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--train", required=True)
    ap.add_argument("--train2")
    ap.add_argument("--val", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--steps", type=int, default=20000)
    ap.add_argument("--batch", type=int, default=32)
    ap.add_argument("--hidden", type=int, default=128)
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--workers", type=int, default=22)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--resume")
    ap.add_argument("--seconds", type=int, default=2)
    a = ap.parse_args()
    torch.set_num_threads(a.threads)
    os.makedirs(a.out, exist_ok=True)
    model = L.Denoiser(a.hidden)
    print("params:", sum(p.numel() for p in model.parameters()), flush=True)
    if a.resume:
        model.load_state_dict(torch.load(a.resume)["model"])

    trains = [a.train] + ([a.train2] if a.train2 else [])
    dl = DataLoader(Stream(trains, seed=1, seconds=a.seconds), batch_size=a.batch, num_workers=a.workers, prefetch_factor=4,
                    persistent_workers=True)
    val = list(DataLoader(Stream([a.val], seed=999, fixed=64), batch_size=16, num_workers=4))
    it = iter(dl)

    if not a.resume:                                # standardisation from a few batches
        feats = []
        for _ in range(6):
            noisy, _, _ = next(it)
            feats.append(L.features(L.stft(noisy)).reshape(-1, L.NB))
        f = torch.cat(feats)
        model.mean.copy_(f.mean(0))
        model.std.copy_(f.std(0) + 1e-3)

    opt = torch.optim.AdamW(model.parameters(), lr=a.lr, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=a.lr, total_steps=a.steps, pct_start=0.05)
    t0, best, run = time.time(), -1e9, 0.0
    for step in range(1, a.steps + 1):
        noisy, clean, vad = next(it)
        loss, lm, lv = compute_loss(model, noisy, clean, vad)
        opt.zero_grad()
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step()
        sched.step()
        run = 0.98 * run + 0.02 * loss.item() if step > 1 else loss.item()
        if step % 50 == 0:
            print(f"step {step} loss {run:.4f} mag {lm:.4f} vad {lv:.3f} lr {sched.get_last_lr()[0]:.5f} {time.time() - t0:.0f}s", flush=True)
        if step % 500 == 0 or step == a.steps:
            s, b, va, vm = validate(model, val)
            print(f"VAL step {step}: SI-SDR {s:.2f} dB (noisy {b:.2f}, gain {s - b:+.2f}) vad-acc {va:.3f} mag-loss {vm:.4f}", flush=True)
            torch.save({"model": model.state_dict(), "hidden": a.hidden, "step": step}, os.path.join(a.out, "last.pt"))
            if s > best:
                best = s
                torch.save({"model": model.state_dict(), "hidden": a.hidden, "step": step}, os.path.join(a.out, "best.pt"))


if __name__ == "__main__":
    main()
