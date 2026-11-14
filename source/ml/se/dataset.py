"""On-the-fly (noisy, clean) pair synthesis.

Every training example is new: random speech utterances with pauses, optional room reverberation (real RIR
files or a synthetic tail), a random microphone colouration applied to speech and noise alike, random speech
level, one or two noises (files and/or synthetic families) at a random SNR, mic hiss and occasional clipping.

The target is the clean speech after the same room and microphone - the model removes noise, not the room -
unless data.dereverb is on, then the target keeps only the direct path + 50 ms of early reflections.
"""
from __future__ import annotations

import math
import os
from typing import List, Tuple

import numpy as np
import torch
from torch.utils.data import IterableDataset

from . import audio as A
from . import synth_noise
from .config import Config


class Corpus:
    """File lists for one split; random sections are read without decoding whole files."""

    def __init__(self, speech_dirs: List[str], noise_dirs: List[str], rir_dirs: List[str]):
        self.speech = A.list_audio(speech_dirs)
        self.noise = A.list_audio(noise_dirs)
        self.rir = A.list_audio(rir_dirs)
        if not self.speech:
            raise SystemExit(f"no speech files found in {speech_dirs} (run download_data.py, see README)")


def _speech_track(rng, corpus: Corpus, n: int, sr: int, max_utt_s: float = 4.0) -> np.ndarray:
    out = np.zeros(n, np.float32)
    pos = int(rng.uniform(0, 0.6) * sr)
    tries = 0
    while pos < n and tries < 50:
        tries += 1
        path = corpus.speech[int(rng.integers(len(corpus.speech)))]
        dur = A.duration(path)
        if dur < 0.8:
            continue
        want = min(dur, rng.uniform(1.0, max_utt_s))
        start = rng.uniform(0, max(0.0, dur - want))
        try:
            u = A.load(path, sr, start, want)
        except Exception:
            continue
        m = min(len(u), n - pos)
        out[pos:pos + m] = u[:m]
        pos += m + int(rng.choice([0.05, 0.15, 0.4, 0.8, 1.2]) * rng.uniform(0.5, 1.5) * sr)
    return out


def _noise_from_files(rng, corpus: Corpus, n: int, sr: int) -> np.ndarray | None:
    for _ in range(10):
        path = corpus.noise[int(rng.integers(len(corpus.noise)))]
        dur = A.duration(path)
        if dur < 0.2:
            continue
        want = n / sr
        start = rng.uniform(0, max(0.0, dur - want))
        try:
            x = A.load(path, sr, start, min(want, dur))
        except Exception:
            continue
        if len(x) < n:                                    # short clip: repeat with random gaps
            reps = [x]
            total = len(x)
            while total < n:
                g = np.zeros(int(rng.uniform(0, 0.5) * sr), np.float32)
                reps += [g, x]
                total += len(g) + len(x)
            x = np.concatenate(reps)
        x = x[:n]
        if A.rms(x) > 1e-6:
            return x / A.rms(x)
    return None


def _rir(rng, corpus: Corpus, sr: int) -> np.ndarray:
    if corpus.rir and rng.random() < 0.8:
        for _ in range(5):
            try:
                h = A.load(corpus.rir[int(rng.integers(len(corpus.rir)))], sr)
                if len(h) > 16 and np.abs(h).max() > 0:
                    return h[: int(1.2 * sr)]
            except Exception:
                pass
    return A.synth_rir(rng, sr)


def make_pair(rng: np.random.Generator, corpus: Corpus, cfg: Config) -> Tuple[np.ndarray, np.ndarray]:
    d, sr = cfg.data, cfg.data.sample_rate
    n = int(d.segment_s * sr)
    dry = _speech_track(rng, corpus, n, sr) if rng.random() > d.p_noise_only else np.zeros(n, np.float32)
    has_speech = bool(np.any(dry))
    speech_in, target = dry, dry
    if has_speech and rng.random() < d.p_rir:
        early, full = A.split_rir(_rir(rng, corpus, sr), sr)
        speech_in = A.convolve(dry, full)
        target = A.convolve(dry, early) if d.dereverb else speech_in
    eq_seed = int(rng.integers(1 << 31)) if rng.random() < d.p_mic_eq else None

    def mic(x):
        return A.random_eq(np.random.default_rng(eq_seed), x, sr) if eq_seed is not None else x

    speech_in, target = mic(speech_in), mic(target)
    # noise: files and/or synthetic families, optionally two of them, optionally reverberant
    parts = []
    for k in range(2 if rng.random() < d.p_second_noise else 1):
        y = None
        if corpus.noise and rng.random() >= d.p_synth_noise:
            y = _noise_from_files(rng, corpus, n, sr)
        if y is None:
            y = synth_noise.random_noise(rng, n, sr)
        if rng.random() < 0.25:
            y = A.convolve(y, A.split_rir(_rir(rng, corpus, sr), sr)[1])
        y = mic(y) if rng.random() < 0.5 else y
        parts.append(y / A.rms(y) * (10 ** (rng.uniform(-10, 0) / 20) if k else 1.0))
    noise = np.sum(parts, axis=0)
    noise /= A.rms(noise)
    if has_speech:
        g = 10 ** (rng.uniform(d.level_min_db, d.level_max_db) / 20) / A.active_rms(speech_in, sr)
        speech_in, target = speech_in * g, target * g
        snr = rng.uniform(d.snr_min, d.snr_max)
        noise *= A.active_rms(speech_in, sr) / 10 ** (snr / 20)
    else:
        noise *= 10 ** (rng.uniform(-75, -20) / 20)
    noisy = speech_in + noise + rng.standard_normal(n).astype(np.float32) * 10 ** (rng.uniform(-95, -65) / 20)
    peak = float(np.abs(noisy).max())
    if rng.random() < d.p_clip and peak > 0:
        lim = peak * rng.uniform(0.3, 0.8)                # clipping microphone preamp
        noisy = np.clip(noisy, -lim, lim)
        peak = lim
    if peak > 0.99:
        noisy, target = noisy * 0.99 / peak, target * 0.99 / peak
    return noisy.astype(np.float32), target.astype(np.float32)


def vad_labels(clean: np.ndarray, n_fft: int, hop: int) -> np.ndarray:
    """Per STFT frame: 1 where the clean speech is within 35 dB of its loudest frame (and above -75 dBFS)."""
    nfr = (len(clean) - n_fft) // hop + 1
    if nfr <= 0:
        return np.zeros(0, np.float32)
    idx = np.arange(nfr)[:, None] * hop + np.arange(n_fft)[None, :]
    db = 10 * np.log10((clean[idx] ** 2).mean(1) + 1e-12)
    return ((db > db.max() - 35) & (db > -75)).astype(np.float32)


class MixDataset(IterableDataset):
    """Endless stream of fresh mixtures; each DataLoader worker gets its own random stream."""

    def __init__(self, cfg: Config, corpus: Corpus, seed: int):
        self.cfg, self.corpus, self.seed = cfg, corpus, seed

    def __iter__(self):
        info = torch.utils.data.get_worker_info()
        rng = np.random.default_rng([self.seed, info.id if info else 0, os.getpid()])
        while True:
            noisy, clean = make_pair(rng, self.corpus, self.cfg)
            yield torch.from_numpy(noisy), torch.from_numpy(clean), torch.from_numpy(
                vad_labels(clean, self.cfg.stft.n_fft, self.cfg.stft.hop))


def build_val_set(cfg: Config, out_dir: str) -> List[Tuple[str, str]]:
    """Fixed validation mixtures (held-out speakers/noise), written once as WAV pairs and reused."""
    os.makedirs(out_dir, exist_ok=True)
    listing = os.path.join(out_dir, "pairs.txt")
    if os.path.exists(listing):
        return [tuple(l.split("\t")) for l in open(listing, encoding="utf-8").read().splitlines() if l]
    d = cfg.data
    corpus = Corpus(d.val_speech_dirs or d.speech_dirs, d.val_noise_dirs, d.rir_dirs)
    rng = np.random.default_rng(12345)
    pairs = []
    for i in range(d.val_items):
        noisy, clean = make_pair(rng, corpus, cfg)
        a, b = os.path.join(out_dir, f"{i:04d}_noisy.wav"), os.path.join(out_dir, f"{i:04d}_clean.wav")
        A.write(a, noisy, d.sample_rate)
        A.write(b, clean, d.sample_rate)
        pairs.append((a, b))
    with open(listing, "w", encoding="utf-8") as f:
        f.write("\n".join("\t".join(p) for p in pairs))
    return pairs
