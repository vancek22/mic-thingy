"""Data synthesis, features and model for the mic thingy neural denoiser ("nn" effect).

Signal path (must match src/dsp/fx_nn.c exactly):
  48 kHz mono float -> frames of 960 samples, hop 480, Vorbis window -> 960-point DFT (481 bins)
  features: log10(|X|^2 / 480 + 1e-10) for the first NB = 161 bins (0..8 kHz, 50 Hz apart), standardised
  net: dense(NB->H) tanh -> GRU(H) -> GRU(H) -> dense(3H -> NB + 1): NB sigmoid gains + 1 sigmoid VAD
  synthesis: gains on bins 0..160, bins above 8 kHz get the mean gain of bins 140..160, IDFT, Vorbis
  window, overlap-add. Delay: 480 samples (10 ms), like RNNoise.
LibriSpeech is 16 kHz, so speech has nothing above 8 kHz; that is why the net only looks below 8 kHz.
"""
import math

import numpy as np
import scipy.signal as sig
import torch
import torch.nn as nn

SR = 48000
N = 960
HOP = 480
NBINS = 481
NB = 161          # bins the net sees / predicts (0..8 kHz)
HF_LO = 140       # bins HF_LO..NB-1 supply the gain for everything above 8 kHz
SEG_S = 4
SEG = SR * SEG_S  # samples per training segment
EPS = 1e-10

_n = np.arange(N)
VORBIS = np.sin(0.5 * np.pi * np.sin(np.pi * (_n + 0.5) / N) ** 2).astype(np.float32)


def vorbis_t():
    return torch.from_numpy(VORBIS)


def stft(x):
    """x: [B, T] float tensor -> complex [B, 481, frames] (no centring)."""
    return torch.stft(x, n_fft=N, hop_length=HOP, win_length=N, window=vorbis_t().to(x.device),
                      center=False, return_complex=True)


def features(X):
    """complex [B, 481, F] -> raw log features [B, F, NB] (not yet standardised)."""
    p = (X.real ** 2 + X.imag ** 2)[:, :NB, :] / (N / 2) + EPS
    return torch.log10(p).transpose(1, 2)


# --------------------------------------------------------------------------- speech bank

class SpeechBank:
    def __init__(self, prefix):
        self.audio = np.load(prefix + ".npy", mmap_mode="r")
        self.idx = np.load(prefix + "_idx.npy")
        self.n_spk = int(self.idx[:, 2].max()) + 1

    def utterance(self, rng, max_s=3.5):
        i = rng.integers(len(self.idx))
        s, n, spk = self.idx[i]
        want = int(rng.uniform(1.0, max_s) * 16000)
        if n > want:
            s += int(rng.integers(0, n - want))
            n = want
        x = np.asarray(self.audio[s:s + n], dtype=np.float32) / 32768.0
        return x, int(spk)


def up3(x):
    return sig.resample_poly(x, 3, 1).astype(np.float32)


# --------------------------------------------------------------------------- noise zoo

def _shape_noise(rng, n, alpha, lo_hz, hi_hz):
    """Gaussian noise with a 1/f^alpha spectrum between lo_hz and hi_hz."""
    X = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1 / SR)
    f[0] = f[1]
    X *= f ** (-alpha / 2.0)
    X[f < lo_hz] *= 0.0
    X[f > hi_hz] *= 0.0
    return np.fft.irfft(X, n).astype(np.float32)


def _env(rng, n, rate_hz, depth):
    """Smooth random envelope in [1-depth, 1]."""
    k = max(2, int(n / SR * rate_hz * 2) + 2)
    pts = rng.uniform(1 - depth, 1, k)
    return np.interp(np.arange(n), np.linspace(0, n, k), pts).astype(np.float32)


def _decay(n, tau_s):
    t = np.arange(n) / SR
    return np.exp(-t / tau_s).astype(np.float32)


def _bandpass(x, lo, hi):
    lo = max(lo, 20.0)
    hi = min(hi, 20000.0)
    if hi <= lo * 1.2:
        hi = lo * 1.5
    sos = sig.butter(2, [lo, hi], btype="band", fs=SR, output="sos")
    return sig.sosfilt(sos, x).astype(np.float32)


def n_colored(rng, n):
    return _shape_noise(rng, n, rng.uniform(0, 2.2), rng.choice([0, 0, 60, 150, 300]), rng.uniform(2500, 24000))


def n_fan(rng, n):
    x = _shape_noise(rng, n, rng.uniform(0.5, 2.5), 40, rng.uniform(300, 1800))
    f0 = rng.uniform(60, 320)
    t = np.arange(n) / SR
    fm = 1 + 0.01 * np.sin(2 * np.pi * rng.uniform(0.1, 1.0) * t)
    ph = 2 * np.pi * np.cumsum(f0 * fm) / SR
    tone = sum(rng.uniform(0.1, 1.0) / h * np.sin(h * ph + rng.uniform(0, 6.28)) for h in range(1, rng.integers(3, 9)))
    return (x / (x.std() + 1e-9) + rng.uniform(0.2, 1.5) * tone / (np.std(tone) + 1e-9)).astype(np.float32) * _env(rng, n, 0.3, 0.3)


def n_hum(rng, n):
    f0 = rng.choice([50.0, 60.0]) * (1 + rng.uniform(-0.004, 0.004))
    t = np.arange(n) / SR
    x = np.zeros(n, np.float32)
    for h in range(1, rng.integers(3, 10)):
        x += (rng.uniform(0.05, 1.0) / h ** rng.uniform(0, 1.2) * np.sin(2 * np.pi * f0 * h * t + rng.uniform(0, 6.28))).astype(np.float32)
    return x + 0.02 * rng.standard_normal(n).astype(np.float32)


def _events(rng, n, rate_hz, burst_prob=0.0):
    """Sorted event sample positions: Poisson-ish, optionally in bursts (typing)."""
    pos, t = [], rng.uniform(0, 0.3)
    dur = n / SR
    while t < dur:
        pos.append(int(t * SR))
        if rng.random() < burst_prob:
            t += rng.uniform(0.05, 0.22)                 # next key in a burst
        else:
            t += rng.exponential(1.0 / rate_hz) + 0.02
    return pos


def n_keyboard(rng, n):
    x = np.zeros(n, np.float32)
    burst = rng.uniform(0.0, 0.9)
    lo, hi = rng.uniform(800, 2500), rng.uniform(3500, 9000)
    for p in _events(rng, n, rng.uniform(0.3, 2.0), burst):
        L = int(rng.uniform(0.012, 0.045) * SR)
        if p + L >= n:
            break
        c = _bandpass(rng.standard_normal(L).astype(np.float32), lo * rng.uniform(0.8, 1.3), hi * rng.uniform(0.8, 1.2)) * _decay(L, rng.uniform(0.003, 0.012))
        thunk = np.sin(2 * np.pi * rng.uniform(120, 420) * np.arange(L) / SR).astype(np.float32) * _decay(L, 0.02) * rng.uniform(0.0, 0.7)
        x[p:p + L] += (c + thunk) * rng.uniform(0.4, 1.6)
    return x


def n_mouse(rng, n):
    x = np.zeros(n, np.float32)
    for p in _events(rng, n, rng.uniform(0.1, 0.8)):
        for rep in range(int(rng.integers(1, 3))):
            q = p + rep * int(rng.uniform(0.06, 0.14) * SR)
            L = int(0.012 * SR)
            if q + L >= n:
                break
            c = _bandpass(rng.standard_normal(L).astype(np.float32), rng.uniform(1500, 3000), rng.uniform(5000, 12000)) * _decay(L, rng.uniform(0.0015, 0.004))
            x[q:q + L] += c * rng.uniform(0.5, 1.5)
    return x


def n_clatter(rng, n):
    x = np.zeros(n, np.float32)
    for p in _events(rng, n, rng.uniform(0.5, 4.0), 0.5):
        L = int(rng.uniform(0.05, 0.3) * SR)
        if p + L >= n:
            break
        t = np.arange(L) / SR
        y = np.zeros(L, np.float32)
        for _ in range(int(rng.integers(2, 6))):
            y += (np.sin(2 * np.pi * rng.uniform(900, 9000) * t + rng.uniform(0, 6.28)) * np.exp(-t / rng.uniform(0.01, 0.1))).astype(np.float32) * rng.uniform(0.2, 1.0)
        x[p:p + L] += y
    return x


def n_thump(rng, n):
    x = np.zeros(n, np.float32)
    for p in _events(rng, n, rng.uniform(0.1, 0.7)):
        L = int(rng.uniform(0.15, 0.5) * SR)
        if p + L >= n:
            break
        y = _shape_noise(rng, L, rng.uniform(0.5, 2), 30, rng.uniform(150, 700)) * _decay(L, rng.uniform(0.05, 0.15))
        x[p:p + L] += y / (y.std() + 1e-9) * rng.uniform(0.5, 2.0)
    return x


def n_rustle(rng, n):
    x = _shape_noise(rng, n, rng.uniform(-0.5, 0.8), rng.uniform(800, 3000), 22000)
    return x * (_env(rng, n, rng.uniform(1, 9), 0.95) ** 2)


def n_music(rng, n):
    x = np.zeros(n, np.float32)
    t = np.arange(n) / SR
    bpm = rng.uniform(70, 160)
    beat = 60.0 / bpm
    scale = np.array([0, 2, 4, 5, 7, 9, 11])
    root = rng.uniform(40, 70)
    k = 0.0
    while k < n / SR:
        L = int(rng.choice([0.5, 1, 2]) * beat * SR)
        a = int(k * SR)
        if a >= n:
            break
        L = min(L, n - a)
        tt = np.arange(L) / SR
        for _ in range(int(rng.integers(1, 4))):
            midi = root + 12 * rng.integers(0, 4) + rng.choice(scale)
            f = 440.0 * 2 ** ((midi - 69) / 12)
            y = np.zeros(L, np.float32)
            for h in range(1, int(rng.integers(3, 9))):
                if f * h < 15000:
                    y += (np.sin(2 * np.pi * f * h * tt + rng.uniform(0, 6.28)) / h ** rng.uniform(0.5, 1.5)).astype(np.float32)
            x[a:a + L] += y * np.exp(-tt / rng.uniform(0.2, 1.5)).astype(np.float32) * rng.uniform(0.3, 1.0)
        k += beat * rng.choice([0.5, 1, 1, 2])
    return x


def n_beeps(rng, n):
    x = np.zeros(n, np.float32)
    f = rng.uniform(500, 3500)
    on, off = rng.uniform(0.08, 0.5), rng.uniform(0.1, 1.5)
    t, tt = 0.0, np.arange(n) / SR
    while t < n / SR:
        a, b = int(t * SR), min(n, int((t + on) * SR))
        x[a:b] = np.sin(2 * np.pi * f * tt[a:b]).astype(np.float32) * rng.uniform(0.5, 1.0)
        t += on + off
    return x


def n_sfx(rng, n):
    """Game-like effects: sweeps and explosions."""
    x = np.zeros(n, np.float32)
    for p in _events(rng, n, rng.uniform(0.2, 1.5)):
        L = int(rng.uniform(0.1, 0.7) * SR)
        if p + L >= n:
            break
        tt = np.arange(L) / SR
        if rng.random() < 0.5:
            f = np.linspace(rng.uniform(300, 4000), rng.uniform(100, 6000), L)
            y = np.sin(2 * np.pi * np.cumsum(f) / SR).astype(np.float32) * np.exp(-tt / rng.uniform(0.1, 0.4)).astype(np.float32)
        else:
            y = _shape_noise(rng, L, rng.uniform(0, 2), 30, rng.uniform(500, 12000)) * np.exp(-tt / rng.uniform(0.05, 0.3)).astype(np.float32)
            y /= (y.std() + 1e-9)
        x[p:p + L] += y * rng.uniform(0.4, 1.5)
    return x


def n_traffic(rng, n):
    x = _shape_noise(rng, n, rng.uniform(1.5, 3.0), 20, rng.uniform(300, 1200))
    return x * _env(rng, n, rng.uniform(0.1, 0.6), 0.6)


NOISES = [n_colored, n_fan, n_hum, n_keyboard, n_mouse, n_clatter, n_thump, n_rustle, n_music, n_beeps, n_sfx, n_traffic]
NOISE_W = np.array([1.5, 1.2, 0.8, 1.6, 1.0, 0.8, 0.7, 0.8, 0.9, 0.5, 0.9, 0.6])
NOISE_W = NOISE_W / NOISE_W.sum()


def n_babble(rng, n, bank):
    x = np.zeros(n, np.float32)
    for _ in range(int(rng.integers(3, 8))):
        pos = 0
        while pos < n:
            u, _ = bank.utterance(rng, 4.0)
            u = up3(u)
            m = min(len(u), n - pos)
            x[pos:pos + m] += u[:m] * rng.uniform(0.3, 1.0)
            pos += m + int(rng.uniform(0, 0.4) * SR)
    return x


# --------------------------------------------------------------------------- augmentation

def rir(rng):
    rt60 = rng.uniform(0.12, 0.8)
    n = int(min(rt60 * 1.1, 0.7) * SR)
    t = np.arange(n) / SR
    h = rng.standard_normal(n).astype(np.float32) * np.exp(-6.9 * t / rt60).astype(np.float32)
    h[0] = 0.0
    h *= rng.uniform(0.15, 0.6) / (np.sqrt((h ** 2).sum()) + 1e-9)       # reverberant / direct level
    h[0] = 1.0
    return h


def reverb(rng, x):
    h = rir(rng)
    return sig.fftconvolve(x, h)[:len(x)].astype(np.float32)


def rand_eq(rng, x, strength=1.0):
    """A random microphone / room colouration: up to 3 RBJ biquads."""
    for _ in range(int(rng.integers(1, 4))):
        kind = rng.choice(["ls", "hs", "pk"])
        f = {"ls": rng.uniform(80, 400), "hs": rng.uniform(3000, 9000), "pk": rng.uniform(300, 6000)}[kind]
        g = rng.uniform(-9, 6) * strength
        A = 10 ** (g / 40)
        w0 = 2 * math.pi * f / SR
        c, s = math.cos(w0), math.sin(w0)
        if kind == "pk":
            q = rng.uniform(0.5, 2.5)
            al = s / (2 * q)
            b = [1 + al * A, -2 * c, 1 - al * A]
            a = [1 + al / A, -2 * c, 1 - al / A]
        else:
            al = s / 2 * math.sqrt(2)
            sq = 2 * math.sqrt(A) * al
            if kind == "ls":
                b = [A * ((A + 1) - (A - 1) * c + sq), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sq)]
                a = [(A + 1) + (A - 1) * c + sq, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sq]
            else:
                b = [A * ((A + 1) + (A - 1) * c + sq), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sq)]
                a = [(A + 1) - (A - 1) * c + sq, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sq]
        x = sig.lfilter(np.array(b) / a[0], np.array(a) / a[0], x).astype(np.float32)
    return x


def active_rms(x, thr_db=-45.0):
    fr = x[: len(x) // 480 * 480].reshape(-1, 480)
    e = np.sqrt((fr ** 2).mean(1) + 1e-12)
    peak = e.max()
    sel = e > peak * 10 ** (thr_db / 20.0)
    return float(np.sqrt((e[sel] ** 2).mean())) if sel.any() else 1e-6


def make_noise(rng, n, bank):
    kinds = []
    if rng.random() < 0.12:
        kinds.append(("babble", None))
    for _ in range(int(rng.choice([1, 1, 2, 3]))):
        kinds.append(("zoo", rng.choice(len(NOISES), p=NOISE_W)))
    total = np.zeros(n, np.float32)
    for k, i in kinds:
        y = n_babble(rng, n, bank) if k == "babble" else NOISES[i](rng, n)
        if rng.random() < 0.3:
            y = reverb(rng, y)
        if rng.random() < 0.5:
            y = rand_eq(rng, y, 0.7)
        y = y / (np.sqrt((y ** 2).mean()) + 1e-9)
        total += y * (10 ** (rng.uniform(-8, 0) / 20) if len(kinds) > 1 else 1.0)
    return total / (np.sqrt((total ** 2).mean()) + 1e-9)


def make_example(rng, bank, seconds=SEG_S):
    """-> noisy, clean float32 arrays of seconds*SR samples."""
    n = seconds * SR
    # speech track: utterances separated by pauses
    clean = np.zeros(n, np.float32)
    if rng.random() > 0.08:
        pos = int(rng.uniform(0, 0.8) * SR)
        while pos < n:
            u, _ = bank.utterance(rng)
            u = up3(u)
            m = min(len(u), n - pos)
            clean[pos:pos + m] = u[:m]
            pos += m + int(rng.choice([0.05, 0.15, 0.4, 0.8, 1.2]) * rng.uniform(0.5, 1.5) * SR)
    speech_present = clean.any()
    if speech_present:
        if rng.random() < 0.35:
            clean = reverb(rng, clean)
        if rng.random() < 0.8:
            clean = rand_eq(rng, clean)
        clean *= 10 ** (rng.uniform(-45, -18) / 20) / active_rms(clean)
    noise = make_noise(rng, n, bank)
    if speech_present:
        snr = rng.choice([rng.uniform(-5, 10), rng.uniform(5, 25), rng.uniform(20, 40)], p=[0.3, 0.5, 0.2])
        noise *= active_rms(clean) / 10 ** (snr / 20)
    else:
        noise *= 10 ** (rng.uniform(-75, -18) / 20)
    hiss = rng.standard_normal(n).astype(np.float32) * 10 ** (rng.uniform(-90, -65) / 20)
    noisy = clean + noise + hiss
    pk = np.abs(noisy).max()
    if pk > 0.99:                                   # headroom: a real mic does not exceed full scale
        g = 0.99 / pk
        clean, noisy = clean * g, noisy * g
    return noisy.astype(np.float32), clean.astype(np.float32)


def vad_labels(clean):
    """Per-frame speech activity from the clean signal (frames as in stft(), centre not used)."""
    nfr = (len(clean) - N) // HOP + 1
    idx = np.arange(nfr)[:, None] * HOP + np.arange(N)[None, :]
    e = ((clean[idx] * VORBIS) ** 2).mean(1)
    db = 10 * np.log10(e + 1e-12)
    pk = db.max()
    return ((db > pk - 35.0) & (db > -75.0)).astype(np.float32)


# --------------------------------------------------------------------------- model

class Denoiser(nn.Module):
    def __init__(self, h=128):
        super().__init__()
        self.h = h
        self.inp = nn.Linear(NB, h)
        self.g1 = nn.GRU(h, h, batch_first=True)
        self.g2 = nn.GRU(2 * h, h, batch_first=True)
        self.out = nn.Linear(3 * h, NB + 1)
        self.register_buffer("mean", torch.zeros(NB))
        self.register_buffer("std", torch.ones(NB))

    def forward(self, feat, h0=None):
        """feat: raw log features [B, F, NB]. -> gains [B, F, NB], vad logits [B, F]"""
        x = (feat - self.mean) / self.std
        d = torch.tanh(self.inp(x))
        o1, _ = self.g1(d)
        o2, _ = self.g2(torch.cat([d, o1], -1))
        y = self.out(torch.cat([d, o1, o2], -1))
        return torch.sigmoid(y[..., :NB]), y[..., NB]


def apply_gains(X, gains):
    """X: complex [B, 481, F]; gains [B, F, NB] -> enhanced complex [B, 481, F]."""
    g = gains.transpose(1, 2)                                   # [B, NB, F]
    hf = g[:, HF_LO:NB, :].mean(1, keepdim=True)
    full = torch.cat([g, hf.expand(-1, NBINS - NB, -1)], 1)
    return X * full


def istft(X, length):
    """Overlap-add with the Vorbis window (matches the C effect)."""
    return torch.istft(X, n_fft=N, hop_length=HOP, win_length=N, window=vorbis_t().to(X.device),
                       center=False, length=length)


def enhance(model, noisy):
    """noisy: [B, T] tensor -> enhanced [B, T] (frames beyond the last full one are zero) and vad."""
    X = stft(noisy)
    g, v = model(features(X))
    return istft(apply_gains(X, g), noisy.shape[-1]), torch.sigmoid(v)
