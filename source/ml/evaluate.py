"""Evaluate checkpoints and any other systems (Krisp, RNNoise, ...) on the same noisy/clean pairs.

    python evaluate.py --set data/testset --ckpt runs/crn48k/best.pt [--ckpt runs/nsnet48k/best.pt] [--examples 4]
    python evaluate.py --make-set data/testset --speech data/LibriSpeech/test-clean --noise data/demand_val --n 60

Test set layout (--make-set writes it; or bring your own, e.g. the DNS synthetic test set):
    <set>/<name>_noisy.wav      the input
    <set>/<name>_clean.wav      the reference (optional: without it only DNSMOS is computed)
Other systems: drop their output next to the input as <name>_noisy.<system>.wav, e.g.
    <set>/0007_noisy.krisp.wav  (Krisp has no file mode: record "Krisp Microphone" while playing the file into
                                 the microphone Krisp uses, e.g. a virtual cable, or use tools/nn/krisp_sim.py)
Every system is scored with the same metrics; external outputs are aligned to the reference first (their
device round trip adds delay). Writes <set>/results.csv and prints a table; --examples N copies before/after
WAVs of the first N items into <set>/examples/.
"""
from __future__ import annotations

import argparse
import csv
import glob
import os
import shutil
from collections import defaultdict

import numpy as np

from se import audio as A
from se import checkpoint
from se.metrics import Dnsmos, score


def make_set(out: str, speech: str, noise: str, n: int, sr: int, seed: int):
    from se import synth_noise
    rng = np.random.default_rng(seed)
    sp = A.list_audio([speech])
    nz = A.list_audio([noise]) if noise else []
    os.makedirs(out, exist_ok=True)
    for i in range(n):
        clean = np.zeros(int(8 * sr), np.float32)
        pos = int(0.5 * sr)
        while pos < len(clean):
            u = A.load(sp[int(rng.integers(len(sp)))], sr)
            m = min(len(u), len(clean) - pos)
            clean[pos:pos + m] = u[:m]
            pos += m + int(rng.uniform(0.4, 1.2) * sr)
        clean *= 10 ** (rng.uniform(-35, -22) / 20) / A.active_rms(clean, sr)
        if nz and rng.random() < 0.7:
            f = nz[int(rng.integers(len(nz)))]
            d = A.duration(f)
            noise = A.load(f, sr, rng.uniform(0, max(0, d - 8.5)), 8.5)[: len(clean)]
            noise = np.pad(noise, (0, len(clean) - len(noise)), mode="wrap")
        else:
            noise = synth_noise.random_noise(rng, len(clean), sr)
        snr = [0, 5, 10, 15, 20][i % 5]
        noise = noise / A.rms(noise) * A.active_rms(clean, sr) / 10 ** (snr / 20)
        noisy = clean + noise
        pk = np.abs(noisy).max()
        if pk > 0.99:
            clean, noisy = clean * 0.99 / pk, noisy * 0.99 / pk
        A.write(os.path.join(out, f"{i:04d}_snr{snr:02d}_noisy.wav"), noisy, sr)
        A.write(os.path.join(out, f"{i:04d}_snr{snr:02d}_clean.wav"), clean, sr)
    print(f"wrote {n} pairs to {out}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", help="folder with *_noisy.wav [+ *_clean.wav, *_noisy.<system>.wav]")
    ap.add_argument("--ckpt", action="append", default=[])
    ap.add_argument("--dnsmos-dir", default="data/dnsmos")
    ap.add_argument("--examples", type=int, default=0)
    ap.add_argument("--sr", type=int, default=48000, help="scoring sample rate")
    ap.add_argument("--make-set")
    ap.add_argument("--speech")
    ap.add_argument("--noise", default="")
    ap.add_argument("--n", type=int, default=60)
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()
    if a.make_set:
        make_set(a.make_set, a.speech, a.noise, a.n, a.sr, a.seed)
        return
    if not a.set:
        ap.error("--set or --make-set")
    try:
        dnsmos = Dnsmos(a.dnsmos_dir)
    except Exception as e:
        print(f"(DNSMOS off: {e})")
        dnsmos = None
    sr = a.sr
    noisy_files = sorted(glob.glob(os.path.join(a.set, "*_noisy.wav")))
    models = []
    for c in a.ckpt:
        m, cfg, stft, _ = checkpoint.load(c)
        tag = os.path.basename(os.path.dirname(os.path.abspath(c))) or "model"
        models.append((tag, m, cfg, stft))
    from inference import enhance_offline
    rows, ex_dir = [], os.path.join(a.set, "examples")
    for k, nf in enumerate(noisy_files):
        base = nf[: -len("_noisy.wav")]
        noisy = A.load(nf, sr)
        clean = A.load(base + "_clean.wav", sr) if os.path.exists(base + "_clean.wav") else None
        n = len(noisy)
        systems = {"noisy": (noisy, 0)}
        for tag, m, cfg, stft in models:
            y = enhance_offline(m, stft, A.load(nf, cfg.data.sample_rate))
            systems[tag] = (A.resample(y, cfg.data.sample_rate, sr)[:n], 0)
        for ext in sorted(glob.glob(nf[:-4] + ".*.wav")):
            systems[ext[len(nf) - 3:-4]] = (A.load(ext, sr), int(0.6 * sr))   # external: align up to 600 ms
        for name, (y, lag) in systems.items():
            r = score(y, clean, noisy, sr, dnsmos, max_lag=lag)
            rows.append({"item": os.path.basename(base), "system": name, **r})
            if k < a.examples:
                A.write(os.path.join(ex_dir, f"{os.path.basename(base)}.{name}.wav"), y, sr)
        print(f"{k + 1}/{len(noisy_files)} {os.path.basename(base)}", end="\r")
    print()
    keys = sorted({k for r in rows for k in r if k not in ("item", "system")})
    with open(os.path.join(a.set, "results.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, ["item", "system"] + keys)
        w.writeheader()
        w.writerows(rows)
    by = defaultdict(list)
    for r in rows:
        by[r["system"]].append(r)
    cols = [c for c in ["si_sdr", "stoi", "pesq_wb", "pause_atten_db", "dnsmos_sig", "dnsmos_bak", "dnsmos_ovrl", "lag_ms"] if c in keys]
    print(f"{'system':16s}" + "".join(f"{c:>15s}" for c in cols) + f"{'items':>8s}")
    for sys_, rs in by.items():
        line = f"{sys_:16s}"
        for c in cols:
            v = [r[c] for r in rs if c in r and np.isfinite(r[c])]
            line += f"{np.mean(v):15.3f}" if v else f"{'-':>15s}"
        print(line + f"{len(rs):8d}")
    print(f"per-item results: {os.path.join(a.set, 'results.csv')}")


if __name__ == "__main__":
    main()
