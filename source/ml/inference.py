"""Enhance WAV/FLAC files with a trained checkpoint.

    python inference.py --ckpt runs/crn48k/best.pt noisy.wav [more.wav ...] [--out-dir enhanced/] [--stream]
    python inference.py --ckpt runs/crn48k/best.pt --in-dir noisy_folder --out-dir enhanced/

Offline mode runs the whole file at once; --stream feeds 10 ms blocks through the streaming path (what a
real-time host does) and compensates its latency so both outputs line up with the input. --check runs both
and prints the difference (should be ~1e-6). Output files: <name>.enh.wav at the model's sample rate.
"""
from __future__ import annotations

import argparse
import os
import time

import numpy as np
import torch

from se import audio as A
from se import checkpoint
from se.stream import Streamer


@torch.no_grad()
def enhance_offline(model, stft, x: np.ndarray, device="cpu") -> np.ndarray:
    t = torch.from_numpy(x)[None].to(device)
    Y, _ = model(stft(t))
    return stft.inverse(Y, t.shape[-1])[0].cpu().numpy()


def enhance_stream(model, stft, x: np.ndarray, device="cpu", block: int = 0) -> np.ndarray:
    s = Streamer(model, stft, device)
    block = block or stft.hop
    out = [s.process(x[i:i + block]) for i in range(0, len(x), block)]
    out.append(s.flush())
    y = np.concatenate(out)
    lat = stft.n_fft - stft.hop
    return y[lat:lat + len(x)]                       # undo the algorithmic delay for file output


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("files", nargs="*")
    ap.add_argument("--in-dir")
    ap.add_argument("--out-dir", default="enhanced")
    ap.add_argument("--stream", action="store_true")
    ap.add_argument("--check", action="store_true", help="compare offline and streaming outputs")
    ap.add_argument("--device", default="cpu")
    a = ap.parse_args()
    model, cfg, stft, _ = checkpoint.load(a.ckpt, a.device)
    sr = cfg.data.sample_rate
    files = list(a.files) + (A.list_audio([a.in_dir]) if a.in_dir else [])
    if not files:
        ap.error("no input files")
    os.makedirs(a.out_dir, exist_ok=True)
    for f in files:
        x = A.load(f, sr)
        t0 = time.time()
        y = enhance_stream(model, stft, x, a.device) if a.stream else enhance_offline(model, stft, x, a.device)
        dt = time.time() - t0
        name = os.path.splitext(os.path.basename(f))[0]
        A.write(os.path.join(a.out_dir, name + ".enh.wav"), y, sr)
        msg = f"{f}: {len(x) / sr:.1f} s audio in {dt:.2f} s (real-time factor {dt / (len(x) / sr):.3f})"
        if a.check:
            y2 = enhance_offline(model, stft, x, a.device) if a.stream else enhance_stream(model, stft, x, a.device)
            n = min(len(y), len(y2)) - stft.n_fft          # the very last frame differs (flush)
            msg += f", offline vs streaming max diff {np.abs(y[:n] - y2[:n]).max():.2e}"
        print(msg)


if __name__ == "__main__":
    main()
