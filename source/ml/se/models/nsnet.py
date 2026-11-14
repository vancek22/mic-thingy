"""NSNet-style recurrent mask estimator (the model mic thingy runs in C, src/dsp/nn.c).

log power of the visible bins -> standardise -> dense(B->H) tanh -> GRU(H) -> GRU(2H->H)
-> dense(3H -> B + 1): B sigmoid gains + one speech-presence logit.
Parameter names match tools/nn (inp, g1, g2, out, mean, std) so export_c.py can write src/dsp/nn_weights.c.
~330 k parameters at H = 128, B = 145.
"""
from __future__ import annotations

import torch
import torch.nn as nn

from .base import EnhancerBase


class NSNet(EnhancerBase):
    def __init__(self, n_fft: int, sample_rate: int, speech_max_hz: float = 0.0, hidden: int = 128, bins: int = 0,
                 hf_lo_hz: float = 5000.0, hf_hi_hz: float = 7000.0):
        super().__init__(n_fft, sample_rate, speech_max_hz, hf_lo_hz, hf_hi_hz)
        if bins:
            self.speech_bins = min(bins, self.speech_bins)
            self.hf_hi = min(self.hf_hi, self.speech_bins)
        B, h = self.speech_bins, hidden
        self.h = h
        self.inp = nn.Linear(B, h)
        self.g1 = nn.GRU(h, h, batch_first=True)
        self.g2 = nn.GRU(2 * h, h, batch_first=True)
        self.out = nn.Linear(3 * h, B + 1)
        self.register_buffer("mean", torch.zeros(B))
        self.register_buffer("std", torch.ones(B))
        self.feat_scale = n_fft / 2.0

    def features(self, X: torch.Tensor) -> torch.Tensor:
        """complex [B, F, T] -> log10 power [B, T, speech_bins] (power normalised by the window energy)."""
        p = (X.real ** 2 + X.imag ** 2)[:, : self.speech_bins] / self.feat_scale + 1e-10
        return torch.log10(p).transpose(1, 2)

    @torch.no_grad()
    def set_normalisation(self, feats: torch.Tensor):
        f = feats.reshape(-1, feats.shape[-1])
        self.mean.copy_(f.mean(0))
        self.std.copy_(f.std(0) + 1e-3)

    def _head(self, d, o1, o2):
        y = self.out(torch.cat([d, o1, o2], -1))
        return torch.sigmoid(y[..., :-1]), y[..., -1]

    def forward(self, X: torch.Tensor):
        x = (self.features(X) - self.mean) / self.std
        d = torch.tanh(self.inp(x))
        o1, _ = self.g1(d)
        o2, _ = self.g2(torch.cat([d, o1], -1))
        g, vad = self._head(d, o1, o2)
        G = self.fill_high(g).transpose(1, 2)               # [B, F, T]
        return X * G, {"vad": vad, "gain": G}

    def init_state(self, batch: int, device=None):
        z = torch.zeros(1, batch, self.h, device=device)
        return (z, z.clone())

    def step(self, x: torch.Tensor, state):
        """x: complex [B, F] (one frame)."""
        X = x[:, :, None]
        f = (self.features(X) - self.mean) / self.std        # [B, 1, bins]
        d = torch.tanh(self.inp(f))
        o1, h1 = self.g1(d, state[0])
        o2, h2 = self.g2(torch.cat([d, o1], -1), state[1])
        g, vad = self._head(d, o1, o2)
        G = self.fill_high(g)[:, 0]                          # [B, F]
        return x * G, (h1, h2), torch.sigmoid(vad[:, 0])

    def step_real(self, re: torch.Tensor, im: torch.Tensor, h1: torch.Tensor, h2: torch.Tensor):
        """ONNX-friendly step (no complex tensors). re/im [B, F] -> (re, im, h1, h2, vad prob)."""
        p = (re ** 2 + im ** 2)[:, : self.speech_bins] / self.feat_scale + 1e-10
        f = ((torch.log10(p) - self.mean) / self.std)[:, None]
        d = torch.tanh(self.inp(f))
        o1, h1 = self.g1(d, h1)
        o2, h2 = self.g2(torch.cat([d, o1], -1), h2)
        g, vad = self._head(d, o1, o2)
        G = self.fill_high(g)[:, 0]
        return re * G, im * G, h1, h2, torch.sigmoid(vad[:, 0])

    def flat_state(self, state):
        return list(state)

    def unflat_state(self, flat):
        return (flat[0], flat[1])
