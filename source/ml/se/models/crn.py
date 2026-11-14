"""Causal convolutional recurrent network (CRN) with a bounded complex ratio mask.

Input per frame: power-law compressed magnitude plus the phase-carrying real/imaginary parts scaled to it
(3 channels over the visible bins). Encoder: Conv2d with kernel (2 frames x 3 bins), stride 2 over frequency,
causal in time (the extra frame is the PREVIOUS one, never the next). Bottleneck: GRU over the flattened
encoder output. Decoder: causal Conv2d + frequency-only transposed conv, with skip connections.
Mask: complex, magnitude bounded by tanh (DCCRN "E" style), applied to the noisy spectrum.
Latency = the STFT's (10 ms at the default 20 ms / 10 ms framing): no look-ahead anywhere.
"""
from __future__ import annotations

from typing import List

import torch
import torch.nn as nn

from .base import EnhancerBase


class CausalConv(nn.Module):
    def __init__(self, cin: int, cout: int, fstride: int, last: bool = False):
        super().__init__()
        self.conv = nn.Conv2d(cin, cout, (2, 3), (1, fstride), (0, 1))
        self.post = nn.Identity() if last else nn.Sequential(nn.BatchNorm2d(cout), nn.PReLU(cout))

    def forward(self, x, cache=None):
        """x [B, C, T, F]; cache = the previous frame [B, C, 1, F] (None = silence before the start)."""
        prev = torch.zeros_like(x[:, :, :1]) if cache is None else cache
        return self.post(self.conv(torch.cat([prev, x], 2))), x[:, :, -1:]


class UpFreq(nn.Module):
    def __init__(self, ch: int, fstride: int):
        super().__init__()
        self.up = nn.ConvTranspose2d(ch, ch, (1, 3), (1, fstride), (0, 1), output_padding=(0, fstride - 1)) \
            if fstride > 1 else nn.Identity()

    def forward(self, x, size: int):
        y = self.up(x)
        if y.shape[-1] < size:
            y = nn.functional.pad(y, (0, size - y.shape[-1]))
        return y[..., :size]


class CRN(EnhancerBase):
    def __init__(self, n_fft: int, sample_rate: int, speech_max_hz: float = 0.0, channels: List[int] = (16, 32, 48, 64),
                 freq_stride: int = 2, rnn_hidden: int = 256, rnn_layers: int = 2, mask: str = "complex",
                 compress: float = 0.3, hf_lo_hz: float = 5000.0, hf_hi_hz: float = 7000.0):
        super().__init__(n_fft, sample_rate, speech_max_hz, hf_lo_hz, hf_hi_hz)
        self.compress, self.mask = compress, mask
        ch = [3] + list(channels)
        self.sizes = [self.speech_bins]
        for _ in channels:
            self.sizes.append((self.sizes[-1] + 2 - 3) // freq_stride + 1)
        self.enc = nn.ModuleList(CausalConv(ch[i], ch[i + 1], freq_stride) for i in range(len(channels)))
        flat = ch[-1] * self.sizes[-1]
        self.rnn = nn.GRU(flat, rnn_hidden, rnn_layers, batch_first=True)
        self.proj = nn.Linear(rnn_hidden, flat)
        self.dec, self.ups = nn.ModuleList(), nn.ModuleList()
        for i in reversed(range(len(channels))):
            cout = ch[i] if i > 0 else 2
            self.dec.append(CausalConv(2 * ch[i + 1], cout if i > 0 else 8, 1))
            self.ups.append(UpFreq(cout if i > 0 else 8, freq_stride))
        self.head = nn.Conv2d(8, 2, 1)
        with torch.no_grad():                       # start close to pass-through: mask ~ tanh(1.5) = 0.9, phase 0
            self.head.weight.mul_(0.1)
            self.head.bias.copy_(torch.tensor([1.5, 0.0]))
        self.rnn_layers, self.rnn_hidden = rnn_layers, rnn_hidden

    def _features(self, X):
        """complex [B, F, T] -> [B, 3, T, bins]"""
        Xs = X[:, : self.speech_bins].transpose(1, 2)
        mag = Xs.abs()
        cm = (mag + 1e-8) ** self.compress
        unit = Xs / (mag + 1e-8)
        return torch.stack([cm, unit.real * cm, unit.imag * cm], 1)

    def _mask_apply(self, X, out):
        """out [B, 2, T, bins] -> enhanced complex [B, F, T]"""
        m = torch.complex(out[:, 0], out[:, 1]).transpose(1, 2)            # [B, bins, T]
        mag = m.abs()
        if self.mask == "complex":
            m = torch.tanh(mag) * m / (mag + 1e-8)
            g = torch.tanh(mag)
        else:
            m = torch.sigmoid(out[:, 0]).transpose(1, 2).to(X.dtype)
            g = m.real
        Y_lo = X[:, : self.speech_bins] * m
        if self.speech_bins == self.F:
            return Y_lo, g
        hf = g[:, self.hf_lo:self.hf_hi].mean(1, keepdim=True)
        Y_hi = X[:, self.speech_bins:] * hf
        return torch.cat([Y_lo, Y_hi], 1), g

    def _run(self, feat, state):
        caches = state["caches"] if state else [None] * (2 * len(self.enc))
        new = []
        skips = []
        x = feat
        for i, e in enumerate(self.enc):
            x, c = e(x, caches[i])
            new.append(c)
            skips.append(x)
        B, C, T, Fq = x.shape
        r, h = self.rnn(x.permute(0, 2, 1, 3).reshape(B, T, C * Fq), state["h"] if state else None)
        x = self.proj(r).reshape(B, T, C, Fq).permute(0, 2, 1, 3)
        for j, (d, u) in enumerate(zip(self.dec, self.ups)):
            i = len(self.enc) - 1 - j
            x, c = d(torch.cat([x, skips[i]], 1), caches[len(self.enc) + j])
            new.append(c)
            x = u(x, self.sizes[i])
        return self.head(x), {"caches": new, "h": h}

    def forward(self, X):
        out, _ = self._run(self._features(X), None)
        Y, g = self._mask_apply(X, out)
        return Y, {"gain": g}

    def init_state(self, batch: int, device=None):
        return None

    def step(self, x, state):
        X = x[:, :, None]
        out, state = self._run(self._features(X), state)
        Y, _ = self._mask_apply(X, out)
        return Y[:, :, 0], state, None

    def zero_state(self, batch: int = 1):
        """Explicit all-zero streaming state (what None means), e.g. for ONNX."""
        X = torch.zeros(batch, self.F, 1, dtype=torch.complex64)
        _, st = self._run(self._features(X), None)
        return {"caches": [torch.zeros_like(c) for c in st["caches"]], "h": torch.zeros_like(st["h"])}

    def flat_state(self, state):
        return list(state["caches"]) + [state["h"]]

    def unflat_state(self, flat):
        return {"caches": list(flat[:-1]), "h": flat[-1]}

    def step_real(self, re: torch.Tensor, im: torch.Tensor, *flat):
        """ONNX-friendly step (no complex tensors). re/im [B, F] -> (re, im, *state)."""
        state = self.unflat_state(list(flat))
        rs, is_ = re[:, : self.speech_bins], im[:, : self.speech_bins]
        mag = torch.sqrt(rs ** 2 + is_ ** 2)
        cm = (mag + 1e-8) ** self.compress
        feat = torch.stack([cm, rs / (mag + 1e-8) * cm, is_ / (mag + 1e-8) * cm], 1)[:, :, None]   # [B, 3, 1, bins]
        out, state = self._run(feat, state)
        mr, mi = out[:, 0, 0], out[:, 1, 0]
        if self.mask == "complex":
            mm = torch.sqrt(mr ** 2 + mi ** 2)
            g = torch.tanh(mm)
            k = g / (mm + 1e-8)
            mr, mi = mr * k, mi * k
        else:
            mr, mi = torch.sigmoid(mr), torch.zeros_like(mr)
            g = mr
        yr, yi = rs * mr - is_ * mi, rs * mi + is_ * mr
        if self.speech_bins < self.F:
            hf = g[:, self.hf_lo:self.hf_hi].mean(1, keepdim=True)
            yr = torch.cat([yr, re[:, self.speech_bins:] * hf], 1)
            yi = torch.cat([yi, im[:, self.speech_bins:] * hf], 1)
        return (yr, yi, *self.flat_state(state))
