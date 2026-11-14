"""Common model interface.

A model maps a complex spectrogram X [B, F, T] to an enhanced spectrogram, frame by frame and causally:
    Y, extras = model(X)                    # whole sequence (training)
    y_frame, state = model.step(x_frame, state)   # one frame [B, F] (streaming; identical results)
extras may hold "vad" logits [B, T].

Band limit: when the training speech is band-limited (LibriSpeech is 16 kHz -> nothing above 8 kHz),
`speech_bins` < F. Then the network only SEES and only PREDICTS bins below it, and every bin above takes the
mean gain of the hf_lo..hf_hi band. Without this a model learns that energy above 8 kHz is always noise and
muffles real (full-band) voices - see docs/NN_LOG.md in mic thingy.
"""
from __future__ import annotations

import torch
import torch.nn as nn


class EnhancerBase(nn.Module):
    def __init__(self, n_fft: int, sample_rate: int, speech_max_hz: float, hf_lo_hz: float, hf_hi_hz: float):
        super().__init__()
        self.n_fft, self.sr = n_fft, sample_rate
        self.F = n_fft // 2 + 1
        hz = sample_rate / n_fft
        lim = speech_max_hz * 0.9 if speech_max_hz > 0 else 0.0     # stay clear of the corpus' anti-alias roll-off
        self.speech_bins = min(self.F, int(lim / hz)) if lim > 0 else self.F
        self.hf_lo = min(int(hf_lo_hz / hz), self.speech_bins - 2)
        self.hf_hi = max(self.hf_lo + 1, min(int(hf_hi_hz / hz), self.speech_bins))

    def fill_high(self, g: torch.Tensor) -> torch.Tensor:
        """g [..., speech_bins] real gains -> [..., F]: bins above take the mean of hf_lo..hf_hi."""
        if self.speech_bins == self.F:
            return g
        hf = g[..., self.hf_lo:self.hf_hi].mean(-1, keepdim=True)
        return torch.cat([g, hf.expand(*g.shape[:-1], self.F - self.speech_bins)], -1)

    def init_state(self, batch: int, device=None):
        raise NotImplementedError

    def step(self, x: torch.Tensor, state):
        raise NotImplementedError
