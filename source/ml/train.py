"""Train a speech enhancement model on on-the-fly mixtures.

    python train.py --config configs/crn48k.yaml [train.batch=8 train.steps=20000 ...] [--resume runs/x/last.pt]

Writes <train.out_dir>/: config.yaml, log.csv (one row per log/validation step), last.pt, best.pt (best
validation SI-SDR), val/ (the fixed validation mixtures) and examples/ (before/after audio at each validation).
"""
from __future__ import annotations

import argparse
import csv
import math
import os
import time

import numpy as np
import torch
import yaml
from torch.utils.data import DataLoader

from se import audio as A
from se import checkpoint
from se.config import load_config, to_dict
from se.dataset import Corpus, MixDataset, build_val_set
from se.losses import EnhancementLoss, si_sdr
from se.metrics import Dnsmos
from se.models import build_model
from se.stft import Stft


def pick_device(name: str) -> torch.device:
    if name == "auto":
        return torch.device("cuda" if torch.cuda.is_available() else "cpu")
    return torch.device(name)


def lr_lambda(step: int, total: int, warmup: float) -> float:
    w = max(1, int(total * warmup))
    if step < w:
        return (step + 1) / w
    p = (step - w) / max(1, total - w)
    return 0.02 + 0.98 * 0.5 * (1 + math.cos(math.pi * min(1.0, p)))


@torch.no_grad()
def validate(model, stft, pairs, cfg, device, examples_dir=None, dnsmos=None):
    model.eval()
    gains, sdrs = [], []
    for i, (np_, cp) in enumerate(pairs):
        noisy = A.load(np_, cfg.data.sample_rate)
        clean = A.load(cp, cfg.data.sample_rate)
        x = torch.from_numpy(noisy)[None].to(device)
        Y, _ = model(stft(x))
        y = stft.inverse(Y, x.shape[-1])[0].cpu().numpy()
        if np.abs(clean).max() > 1e-5:
            a, b = si_sdr(torch.from_numpy(y)[None], torch.from_numpy(clean)[None]).item(), \
                si_sdr(torch.from_numpy(noisy)[None], torch.from_numpy(clean)[None]).item()
            sdrs.append(a)
            gains.append(a - b)
        if examples_dir and i < 4:
            A.write(os.path.join(examples_dir, f"{i:02d}_noisy.wav"), noisy, cfg.data.sample_rate)
            A.write(os.path.join(examples_dir, f"{i:02d}_enhanced.wav"), y, cfg.data.sample_rate)
    res = {"val_sisdr": float(np.mean(sdrs)), "val_sisdr_gain": float(np.mean(gains))}
    if dnsmos is not None and cfg.data.real_dirs:
        scores = []
        for f in A.list_audio(cfg.data.real_dirs)[:20]:
            x = torch.from_numpy(A.load(f, cfg.data.sample_rate))[None].to(device)
            Y, _ = model(stft(x))
            scores.append(dnsmos(stft.inverse(Y, x.shape[-1])[0].cpu().numpy(), cfg.data.sample_rate)["dnsmos_ovrl"])
        if scores:
            res["real_dnsmos_ovrl"] = float(np.mean(scores))
    model.train()
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="configs/crn48k.yaml")
    ap.add_argument("--resume")
    ap.add_argument("--dnsmos-dir", default="data/dnsmos", help="folder with sig_bak_ovr.onnx (optional)")
    ap.add_argument("overrides", nargs="*", help="key=value, e.g. train.batch=8")
    a = ap.parse_args()
    cfg = load_config(a.config, a.overrides)
    t = cfg.train
    os.makedirs(t.out_dir, exist_ok=True)
    with open(os.path.join(t.out_dir, "config.yaml"), "w", encoding="utf-8") as f:
        yaml.safe_dump(to_dict(cfg), f, sort_keys=False)
    torch.manual_seed(t.seed)
    if t.threads:
        torch.set_num_threads(t.threads)
    device = pick_device(t.device)
    use_amp = t.amp and device.type == "cuda"

    stft = Stft(cfg.stft.n_fft, cfg.stft.hop, cfg.stft.window).to(device)
    model = build_model(cfg).to(device)
    crit = EnhancementLoss(cfg).to(device)
    n_par = sum(p.numel() for p in model.parameters())
    print(f"model {cfg.model.name}: {n_par / 1e6:.2f} M parameters, {cfg.data.sample_rate} Hz, "
          f"latency {stft.latency() / cfg.data.sample_rate * 1000:.1f} ms, visible bins {model.speech_bins}/{model.F}, "
          f"device {device}{' + AMP' if use_amp else ''}", flush=True)

    corpus = Corpus(cfg.data.speech_dirs, cfg.data.noise_dirs, cfg.data.rir_dirs)
    print(f"train corpus: {len(corpus.speech)} speech, {len(corpus.noise)} noise, {len(corpus.rir)} RIR files", flush=True)
    dl = DataLoader(MixDataset(cfg, corpus, t.seed), batch_size=t.batch, num_workers=cfg.data.num_workers,
                    persistent_workers=cfg.data.num_workers > 0, prefetch_factor=4 if cfg.data.num_workers else None,
                    pin_memory=device.type == "cuda")
    it = iter(dl)
    val_pairs = build_val_set(cfg, os.path.join(t.out_dir, "val"))
    dnsmos = None
    try:
        dnsmos = Dnsmos(a.dnsmos_dir)
    except Exception:
        pass

    opt = torch.optim.AdamW(model.parameters(), lr=t.lr, weight_decay=t.weight_decay)
    sched = torch.optim.lr_scheduler.LambdaLR(opt, lambda s: lr_lambda(s, t.steps, t.warmup))
    scaler = torch.amp.GradScaler("cuda", enabled=use_amp)
    step, best = 0, -1e9
    if a.resume:
        ck = torch.load(a.resume, map_location=device, weights_only=False)
        model.load_state_dict(ck["model"])
        if ck.get("opt"):
            opt.load_state_dict(ck["opt"])
            sched.load_state_dict(ck["sched"])
        step, best = ck["step"], ck.get("best") or -1e9
        print(f"resumed at step {step}", flush=True)
    elif hasattr(model, "set_normalisation"):                # nsnet: feature standardisation from real batches
        feats = [model.features(stft(next(it)[0].to(device))) for _ in range(4)]
        model.set_normalisation(torch.cat(feats, 1))

    log_path = os.path.join(t.out_dir, "log.csv")
    new_log = not os.path.exists(log_path) or not a.resume
    logf = open(log_path, "w" if new_log else "a", newline="")
    log = csv.writer(logf)
    if new_log:
        log.writerow(["step", "time_s", "lr", "loss", "mrstft", "sisdr", "spec", "vad", "val_sisdr", "val_sisdr_gain", "real_dnsmos_ovrl"])
    t0, run = time.time(), None
    model.train()
    while step < t.steps:
        noisy, clean, vad = (z.to(device, non_blocking=True) for z in next(it))
        X, Xc = stft(noisy), stft(clean)
        with torch.autocast(device.type, dtype=torch.float16, enabled=use_amp):
            Y, extras = model(X)
        Y = Y.to(torch.complex64)
        est = stft.inverse(Y, noisy.shape[-1])
        loss, parts = crit(est, clean, noisy, Y, Xc, extras, vad, model.speech_bins)
        opt.zero_grad(set_to_none=True)
        scaler.scale(loss).backward()
        scaler.unscale_(opt)
        torch.nn.utils.clip_grad_norm_(model.parameters(), t.grad_clip)
        scaler.step(opt)
        scaler.update()
        sched.step()
        step += 1
        run = loss.item() if run is None else 0.98 * run + 0.02 * loss.item()
        if step % t.log_every == 0:
            print(f"step {step} loss {run:.4f} " + " ".join(f"{k} {v:.3f}" for k, v in parts.items())
                  + f" lr {sched.get_last_lr()[0]:.2e} {time.time() - t0:.0f}s", flush=True)
            log.writerow([step, round(time.time() - t0), sched.get_last_lr()[0], run, parts.get("mrstft"),
                          parts.get("sisdr"), parts.get("spec"), parts.get("vad"), "", "", ""])
            logf.flush()
        if step % t.val_every == 0 or step == t.steps:
            ex = os.path.join(t.out_dir, "examples", f"step{step:06d}")
            r = validate(model, stft, val_pairs, cfg, device, ex, dnsmos)
            print(f"VAL step {step}: " + ", ".join(f"{k} {v:.3f}" for k, v in r.items()), flush=True)
            log.writerow([step, round(time.time() - t0), "", "", "", "", "", "", r["val_sisdr"], r["val_sisdr_gain"],
                          r.get("real_dnsmos_ovrl", "")])
            logf.flush()
            if r["val_sisdr"] > best:
                best = r["val_sisdr"]
                checkpoint.save(os.path.join(t.out_dir, "best.pt"), model, cfg, step, best=best)
        if step % t.ckpt_every == 0 or step == t.steps:
            checkpoint.save(os.path.join(t.out_dir, "last.pt"), model, cfg, step, opt, sched, best)
    logf.close()


if __name__ == "__main__":
    main()
