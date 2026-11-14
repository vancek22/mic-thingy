"""Audio I/O and signal utilities (numpy, float32, mono)."""
from __future__ import annotations

import math
import os
from functools import lru_cache
from math import gcd
from typing import List

import numpy as np
import scipy.signal as sig
import soundfile as sf

AUDIO_EXT = (".wav", ".flac", ".mp3", ".ogg")


def list_audio(dirs: List[str]) -> List[str]:
    """All audio files below the given folders (sorted, cached in <dir>/.filelist.txt for big corpora)."""
    out = []
    for d in dirs:
        if os.path.isfile(d):
            out.append(d)
            continue
        cache = os.path.join(d, ".filelist.txt")
        if os.path.exists(cache) and os.path.getmtime(cache) >= os.path.getmtime(d):
            with open(cache, encoding="utf-8") as f:
                out += [os.path.join(d, l.rstrip("\n")) for l in f if l.strip()]
            continue
        found = []
        for root, _, files in os.walk(d):
            for fn in files:
                if fn.lower().endswith(AUDIO_EXT):
                    found.append(os.path.relpath(os.path.join(root, fn), d))
        found.sort()
        try:
            with open(cache, "w", encoding="utf-8") as f:
                f.write("\n".join(found))
        except OSError:
            pass
        out += [os.path.join(d, p) for p in found]
    return out


def resample(x: np.ndarray, sr_from: int, sr_to: int) -> np.ndarray:
    if sr_from == sr_to:
        return x.astype(np.float32, copy=False)
    g = gcd(sr_from, sr_to)
    return sig.resample_poly(x, sr_to // g, sr_from // g).astype(np.float32)


def load(path: str, sr: int, start_s: float | None = None, dur_s: float | None = None) -> np.ndarray:
    """Mono float32 at `sr`; optionally a section (random access without decoding the whole file for wav/flac)."""
    info = sf.info(path)
    start = int((start_s or 0.0) * info.samplerate)
    frames = -1 if dur_s is None else int(dur_s * info.samplerate) + 1
    x, fs = sf.read(path, start=start, frames=frames, dtype="float32", always_2d=True)
    return resample(x.mean(1), fs, sr)


@lru_cache(maxsize=200000)
def duration(path: str) -> float:
    try:
        i = sf.info(path)
        return i.frames / i.samplerate
    except Exception:
        return 0.0


def write(path: str, x: np.ndarray, sr: int) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    sf.write(path, np.asarray(x, np.float32), sr, subtype="FLOAT")


def rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(x.astype(np.float64) ** 2) + 1e-20))


def active_rms(x: np.ndarray, sr: int, thr_db: float = -45.0) -> float:
    """RMS over 10 ms frames within thr_db of the loudest frame (speech level without the pauses)."""
    hop = sr // 100
    n = len(x) // hop
    if n == 0:
        return rms(x)
    e = np.sqrt((x[: n * hop].reshape(n, hop).astype(np.float64) ** 2).mean(1) + 1e-20)
    sel = e > e.max() * 10 ** (thr_db / 20)
    return float(np.sqrt((e[sel] ** 2).mean())) if sel.any() else 1e-10


def band_limit(x: np.ndarray, sr: int, hz: float) -> np.ndarray:
    if hz <= 0 or hz >= sr / 2 * 0.98:
        return x
    sos = sig.butter(8, hz, btype="low", fs=sr, output="sos")
    return sig.sosfilt(sos, x).astype(np.float32)


def rbj_biquad(kind: str, f: float, gain_db: float, q: float, sr: int):
    A = 10 ** (gain_db / 40)
    w0 = 2 * math.pi * f / sr
    c, s = math.cos(w0), math.sin(w0)
    al = s / (2 * q)
    if kind == "peak":
        b = [1 + al * A, -2 * c, 1 - al * A]
        a = [1 + al / A, -2 * c, 1 - al / A]
    else:
        sq = 2 * math.sqrt(A) * al
        if kind == "lowshelf":
            b = [A * ((A + 1) - (A - 1) * c + sq), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sq)]
            a = [(A + 1) + (A - 1) * c + sq, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sq]
        else:
            b = [A * ((A + 1) + (A - 1) * c + sq), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sq)]
            a = [(A + 1) - (A - 1) * c + sq, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sq]
    return np.array(b) / a[0], np.array(a) / a[0]


def random_eq(rng: np.random.Generator, x: np.ndarray, sr: int, strength: float = 1.0) -> np.ndarray:
    """Random microphone / room colouration: 1-3 shelves or peaks."""
    for _ in range(int(rng.integers(1, 4))):
        kind = rng.choice(["lowshelf", "highshelf", "peak"])
        f = {"lowshelf": rng.uniform(80, 400), "highshelf": rng.uniform(2500, min(9000, sr * 0.4)),
             "peak": rng.uniform(300, min(6000, sr * 0.4))}[kind]
        b, a = rbj_biquad(kind, f, rng.uniform(-9, 6) * strength, rng.uniform(0.5, 2.5) if kind == "peak" else 0.707, sr)
        x = sig.lfilter(b, a, x).astype(np.float32)
    return x


def synth_rir(rng: np.random.Generator, sr: int) -> np.ndarray:
    """Exponentially decaying noise tail after a direct path (used when no RIR files are given)."""
    rt60 = rng.uniform(0.15, 0.9)
    n = int(min(rt60 * 1.2, 1.0) * sr)
    t = np.arange(n) / sr
    h = rng.standard_normal(n).astype(np.float32) * np.exp(-6.9 * t / rt60).astype(np.float32)
    pre = int(rng.uniform(0.001, 0.006) * sr)
    h[:pre] = 0.0
    h *= rng.uniform(0.2, 1.0) / (np.sqrt((h ** 2).sum()) + 1e-9)
    h[0] = 1.0
    return h


def split_rir(h: np.ndarray, sr: int, early_ms: float = 50.0) -> tuple[np.ndarray, np.ndarray]:
    """(direct + early part, full) of an RIR aligned so that its peak is the direct path."""
    p = int(np.argmax(np.abs(h)))
    h = h[p:] / (np.abs(h[p]) + 1e-9)
    e = h.copy()
    cut = int(early_ms * 1e-3 * sr)
    if len(e) > cut:
        fade = np.exp(-np.arange(len(e) - cut) / (0.01 * sr)).astype(np.float32)
        e[cut:] *= fade
    return e.astype(np.float32), h.astype(np.float32)


def convolve(x: np.ndarray, h: np.ndarray) -> np.ndarray:
    return sig.fftconvolve(x, h)[: len(x)].astype(np.float32)
