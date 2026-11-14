"""Synthetic noise families: a fallback when no noise corpus is available, and extra variety (clicks, typing,
fans, hum, music-like tones) that public noise sets cover thinly. Every generator returns n float32 samples at
`sr`, roughly unit RMS after normalisation by the caller."""
from __future__ import annotations

import numpy as np
import scipy.signal as sig


def _shape(rng, n, sr, alpha, lo, hi):
    X = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1 / sr)
    f[0] = f[1]
    X *= f ** (-alpha / 2.0)
    X[(f < lo) | (f > hi)] = 0.0
    return np.fft.irfft(X, n).astype(np.float32)


def _env(rng, n, sr, rate, depth):
    k = max(2, int(n / sr * rate * 2) + 2)
    return np.interp(np.arange(n), np.linspace(0, n, k), rng.uniform(1 - depth, 1, k)).astype(np.float32)


def _decay(n, sr, tau):
    return np.exp(-np.arange(n) / sr / tau).astype(np.float32)


def _bp(x, sr, lo, hi):
    hi = min(hi, sr * 0.45)
    lo = max(20.0, min(lo, hi / 1.3))
    return sig.sosfilt(sig.butter(2, [lo, hi], btype="band", fs=sr, output="sos"), x).astype(np.float32)


def _events(rng, n, sr, rate, burst=0.0):
    pos, t = [], rng.uniform(0, 0.3)
    while t < n / sr:
        pos.append(int(t * sr))
        t += rng.uniform(0.05, 0.22) if rng.random() < burst else rng.exponential(1 / rate) + 0.02
    return pos


def colored(rng, n, sr):
    return _shape(rng, n, sr, rng.uniform(0, 2.2), rng.choice([0, 0, 60, 150, 300]), rng.uniform(2500, sr / 2))


def fan(rng, n, sr):
    x = _shape(rng, n, sr, rng.uniform(0.5, 2.5), 40, rng.uniform(300, 1800))
    f0 = rng.uniform(60, 320)
    t = np.arange(n) / sr
    ph = 2 * np.pi * np.cumsum(f0 * (1 + 0.01 * np.sin(2 * np.pi * rng.uniform(0.1, 1) * t))) / sr
    tone = sum(rng.uniform(0.1, 1) / h * np.sin(h * ph + rng.uniform(0, 6.28)) for h in range(1, int(rng.integers(3, 9))))
    return (x / (x.std() + 1e-9) + rng.uniform(0.2, 1.5) * tone / (np.std(tone) + 1e-9)).astype(np.float32) * _env(rng, n, sr, 0.3, 0.3)


def hum(rng, n, sr):
    f0 = rng.choice([50.0, 60.0]) * (1 + rng.uniform(-0.004, 0.004))
    t = np.arange(n) / sr
    x = sum(rng.uniform(0.05, 1) / h ** rng.uniform(0, 1.2) * np.sin(2 * np.pi * f0 * h * t + rng.uniform(0, 6.28))
            for h in range(1, int(rng.integers(3, 10))))
    return (x + 0.02 * rng.standard_normal(n)).astype(np.float32)


def keyboard(rng, n, sr):
    x = np.zeros(n, np.float32)
    lo, hi = rng.uniform(800, 2500), rng.uniform(3500, 9000)
    for p in _events(rng, n, sr, rng.uniform(0.3, 2.0), rng.uniform(0, 0.9)):
        L = int(rng.uniform(0.012, 0.045) * sr)
        if p + L >= n:
            break
        c = _bp(rng.standard_normal(L).astype(np.float32), sr, lo * rng.uniform(0.8, 1.3), hi * rng.uniform(0.8, 1.2)) * _decay(L, sr, rng.uniform(0.003, 0.012))
        thunk = np.sin(2 * np.pi * rng.uniform(120, 420) * np.arange(L) / sr).astype(np.float32) * _decay(L, sr, 0.02) * rng.uniform(0, 0.7)
        x[p:p + L] += (c + thunk) * rng.uniform(0.4, 1.6)
    return x


def mouse(rng, n, sr):
    x = np.zeros(n, np.float32)
    for p in _events(rng, n, sr, rng.uniform(0.1, 0.8)):
        for rep in range(int(rng.integers(1, 3))):
            q = p + rep * int(rng.uniform(0.06, 0.14) * sr)
            L = int(0.012 * sr)
            if q + L >= n:
                break
            x[q:q + L] += _bp(rng.standard_normal(L).astype(np.float32), sr, rng.uniform(1500, 3000), rng.uniform(5000, 12000)) * _decay(L, sr, rng.uniform(0.0015, 0.004)) * rng.uniform(0.5, 1.5)
    return x


def clatter(rng, n, sr):
    x = np.zeros(n, np.float32)
    for p in _events(rng, n, sr, rng.uniform(0.5, 4), 0.5):
        L = int(rng.uniform(0.05, 0.3) * sr)
        if p + L >= n:
            break
        t = np.arange(L) / sr
        x[p:p + L] += sum(np.sin(2 * np.pi * rng.uniform(900, min(9000, sr * 0.45)) * t + rng.uniform(0, 6.28)) * np.exp(-t / rng.uniform(0.01, 0.1)) * rng.uniform(0.2, 1)
                          for _ in range(int(rng.integers(2, 6)))).astype(np.float32)
    return x


def thump(rng, n, sr):
    x = np.zeros(n, np.float32)
    for p in _events(rng, n, sr, rng.uniform(0.1, 0.7)):
        L = int(rng.uniform(0.15, 0.5) * sr)
        if p + L >= n:
            break
        y = _shape(rng, L, sr, rng.uniform(0.5, 2), 30, rng.uniform(150, 700)) * _decay(L, sr, rng.uniform(0.05, 0.15))
        x[p:p + L] += y / (y.std() + 1e-9) * rng.uniform(0.5, 2)
    return x


def rustle(rng, n, sr):
    return _shape(rng, n, sr, rng.uniform(-0.5, 0.8), rng.uniform(800, 3000), sr / 2) * _env(rng, n, sr, rng.uniform(1, 9), 0.95) ** 2


def music(rng, n, sr):
    x = np.zeros(n, np.float32)
    beat = 60.0 / rng.uniform(70, 160)
    scale = np.array([0, 2, 4, 5, 7, 9, 11])
    root = rng.uniform(40, 70)
    k = 0.0
    while k < n / sr:
        a = int(k * sr)
        L = min(int(rng.choice([0.5, 1, 2]) * beat * sr), n - a)
        tt = np.arange(L) / sr
        for _ in range(int(rng.integers(1, 4))):
            f = 440.0 * 2 ** ((root + 12 * rng.integers(0, 4) + rng.choice(scale) - 69) / 12)
            y = sum(np.sin(2 * np.pi * f * h * tt + rng.uniform(0, 6.28)) / h ** rng.uniform(0.5, 1.5)
                    for h in range(1, int(rng.integers(3, 9))) if f * h < sr * 0.4)
            x[a:a + L] += (np.asarray(y) * np.exp(-tt / rng.uniform(0.2, 1.5)) * rng.uniform(0.3, 1)).astype(np.float32)
        k += beat * rng.choice([0.5, 1, 1, 2])
    return x


def beeps(rng, n, sr):
    x = np.zeros(n, np.float32)
    f, on, off = rng.uniform(500, 3500), rng.uniform(0.08, 0.5), rng.uniform(0.1, 1.5)
    t, tt = 0.0, np.arange(n) / sr
    while t < n / sr:
        a, b = int(t * sr), min(n, int((t + on) * sr))
        x[a:b] = np.sin(2 * np.pi * f * tt[a:b]) * rng.uniform(0.5, 1)
        t += on + off
    return x


def traffic(rng, n, sr):
    return _shape(rng, n, sr, rng.uniform(1.5, 3), 20, rng.uniform(300, 1200)) * _env(rng, n, sr, rng.uniform(0.1, 0.6), 0.6)


FAMILIES = {"colored": colored, "fan": fan, "hum": hum, "keyboard": keyboard, "mouse": mouse, "clatter": clatter,
            "thump": thump, "rustle": rustle, "music": music, "beeps": beeps, "traffic": traffic}
WEIGHTS = np.array([1.5, 1.2, 0.8, 1.6, 1.0, 0.8, 0.7, 0.8, 0.9, 0.5, 0.6])
WEIGHTS = WEIGHTS / WEIGHTS.sum()


def random_noise(rng: np.random.Generator, n: int, sr: int, family: str | None = None) -> np.ndarray:
    name = family or rng.choice(list(FAMILIES), p=WEIGHTS)
    y = FAMILIES[name](rng, n, sr)
    return (y / (np.sqrt((y ** 2).mean()) + 1e-9)).astype(np.float32)
