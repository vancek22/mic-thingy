"""Two-stage enhancer: ERB-band gains over the whole visible band + deep filtering of the low bins.

Stage 1 (envelope): log energy of `bands` ERB-spaced triangular bands -> conv over frequency -> shared GRU ->
one sigmoid gain per band, interpolated back to bins with the same triangles (they sum to 1 in every bin).
Stage 2 (fine structure): for the bins below `df_max_hz` the net predicts `df_order` complex taps per bin that
filter the stage-1 output over the current and the previous frames:
    Y[k, t] = X1[k, t] + sum_i C_i[k, t] * X1[k, t - i],   i = 0 .. df_order - 1
A real gain cannot remove noise between the pitch harmonics or correct the phase; a short complex filter along
time can. The last layer starts at zero, so training begins from "stage 1 only".
`df_order = 0` builds stage 1 alone (cheap baseline to compare against nsnet).

Causal: convolutions run over frequency only, all time context lives in the GRUs and in the df_order - 1 past
frames of X1. Latency = the STFT's. No normalisation layers, so the C port needs conv1d, GRU, dense and a
complex multiply-add (the streaming state is two GRU vectors and the X1 history).
~345 k parameters at hidden 128, 32 bands, 96 filtered bins, 5 taps.

Band limit: with band-limited training speech (speech_max_hz > 0) the bands only span the visible bins and the
bins above take the mean gain of hf_lo..hf_hi, as in the other models. Full-band control needs full-band speech.
"""
from __future__ import annotations

import math

import torch
import torch.nn as nn

from .base import EnhancerBase


def erb_centres(n_bins: int, hz_per_bin: float, bands: int) -> list[int]:
    """Band centres (bin indices), equally spaced on the ERB-rate scale, first = 0, last = n_bins - 1,
    strictly increasing (below ~1 kHz an ERB is narrower than one bin, so those centres are 1 bin apart)."""
    bands = max(2, min(bands, n_bins // 2))
    top = 21.4 * math.log10(1.0 + 0.00437 * (n_bins - 1) * hz_per_bin)
    c = [int(round((10.0 ** (top * i / (bands - 1) / 21.4) - 1.0) / 0.00437 / hz_per_bin)) for i in range(bands)]
    for i in range(1, bands):
        c[i] = max(c[i], c[i - 1] + 1)
    c[-1] = n_bins - 1
    for i in range(bands - 2, -1, -1):
        c[i] = min(c[i], c[i + 1] - 1)
    return c


def erb_matrix(n_bins: int, centres: list[int]) -> torch.Tensor:
    """[n_bins, bands] triangular weights: every bin is a linear mix of its two neighbouring band centres."""
    w = torch.zeros(n_bins, len(centres))
    for j in range(len(centres) - 1):
        lo, hi = centres[j], centres[j + 1]
        for k in range(lo, hi + 1):
            a = (k - lo) / (hi - lo)
            w[k, j], w[k, j + 1] = 1.0 - a, a
    return w


class ErbDf(EnhancerBase):
    def __init__(self, n_fft: int, sample_rate: int, speech_max_hz: float = 0.0, hidden: int = 128, bands: int = 32,
                 df_max_hz: float = 4800.0, df_order: int = 5, conv_ch: int = 16, compress: float = 0.3,
                 hf_lo_hz: float = 5000.0, hf_hi_hz: float = 7000.0):
        super().__init__(n_fft, sample_rate, speech_max_hz, hf_lo_hz, hf_hi_hz)
        if df_order == 1:
            raise ValueError("df_order must be 0 (stage 1 only) or >= 2")
        hz = sample_rate / n_fft
        V, h, C = self.speech_bins, hidden, conv_ch
        self.h, self.C, self.compress, self.order = h, C, compress, df_order
        self.feat_scale = n_fft / 2.0

        centres = erb_centres(V, hz, bands)
        self.nb = len(centres)
        w = erb_matrix(V, centres)
        self.register_buffer("erb_inv", w.t().contiguous())                   # [bands, V]: band gains -> bin gains
        self.register_buffer("erb_fwd", (w / w.sum(0, keepdim=True)).contiguous())   # [V, bands]: bin power -> band mean
        self.register_buffer("mean", torch.zeros(self.nb))
        self.register_buffer("std", torch.ones(self.nb))

        na = (self.nb - 1) // 2 + 1
        self.enc_a = nn.Sequential(nn.Conv1d(1, C, 3, 1, 1), nn.ReLU(), nn.Conv1d(C, C, 3, 2, 1), nn.ReLU())
        self.lin_a = nn.Linear(C * na, h)
        self.gru = nn.GRU(h, h, batch_first=True)
        self.out_erb = nn.Linear(h + C * na, self.nb)
        self.out_vad = nn.Linear(h, 1)

        self.D = (min(int(df_max_hz / hz), V) // 4) * 4 if df_order else 0   # filtered bins (two stride-2 convs)
        if self.D:
            D4 = self.D // 4
            self.enc_b1 = nn.Sequential(nn.Conv1d(2, C, 3, 2, 1), nn.ReLU())
            self.enc_b2 = nn.Sequential(nn.Conv1d(C, C, 3, 2, 1), nn.ReLU())
            self.lin_b = nn.Linear(C * D4, h)
            self.gru_df = nn.GRU(h, h, batch_first=True)
            self.lin_df = nn.Linear(h, C * D4)
            self.up1 = nn.ConvTranspose1d(C, C, 3, 2, 1, output_padding=1)
            self.up2 = nn.ConvTranspose1d(C, 2 * df_order, 3, 2, 1, output_padding=1)
            nn.init.zeros_(self.up2.weight)
            nn.init.zeros_(self.up2.bias)

    # ---- features (train.py standardises them from real batches) ----
    def _band_log(self, p: torch.Tensor) -> torch.Tensor:
        return torch.log10(p @ self.erb_fwd / self.feat_scale + 1e-10)

    def features(self, X: torch.Tensor) -> torch.Tensor:
        """complex [B, F, T] -> log10 band energy [B, T, bands]"""
        Xs = X[:, : self.speech_bins].transpose(1, 2)
        return self._band_log(Xs.real ** 2 + Xs.imag ** 2)

    @torch.no_grad()
    def set_normalisation(self, feats: torch.Tensor):
        f = feats.reshape(-1, feats.shape[-1])
        self.mean.copy_(f.mean(0))
        self.std.copy_(f.std(0) + 1e-3)

    # ---- the whole network in real arithmetic; forward, step and step_real all go through here ----
    def _core(self, re: torch.Tensor, im: torch.Tensor, state):
        """re, im [B, T, F]; state = (h1,) or (h1, h2, hist_re, hist_im).
        Returns re, im [B, T, F], bin gains [B, T, F], vad logits [B, T], new state."""
        B, T, _ = re.shape
        V, D = self.speech_bins, self.D
        rs, is_ = re[..., :V], im[..., :V]
        fa = (self._band_log(rs ** 2 + is_ ** 2) - self.mean) / self.std
        a = self.enc_a(fa.reshape(B * T, 1, self.nb)).reshape(B, T, -1)
        e = self.lin_a(a)
        if D:
            rd, id_ = rs[..., :D], is_[..., :D]
            k = (rd ** 2 + id_ ** 2 + 1e-12) ** (0.5 * (self.compress - 1.0))      # |X|^c / |X|
            b1 = self.enc_b1(torch.stack([rd * k, id_ * k], 2).reshape(B * T, 2, D))
            b2 = self.enc_b2(b1)
            e = e + self.lin_b(b2.reshape(B, T, -1))
        o1, h1 = self.gru(torch.relu(e), state[0])
        vad = self.out_vad(o1)[..., 0]
        gb = torch.sigmoid(self.out_erb(torch.cat([o1, a], -1)))
        G = self.fill_high(gb @ self.erb_inv)
        yr, yi = re * G, im * G
        if not D:
            return yr, yi, G, vad, (h1,)

        o2, h2 = self.gru_df(o1, state[1])
        d = torch.relu(self.lin_df(o2)).reshape(B * T, self.C, D // 4) + b2
        d = torch.relu(self.up1(d)) + b1
        c = torch.tanh(self.up2(d)).reshape(B, T, 2, self.order, D)
        xr = torch.cat([state[2], yr[..., :D]], 1)                               # [B, order - 1 + T, D]
        xi = torch.cat([state[3], yi[..., :D]], 1)
        n = self.order - 1
        ar, ai = yr[..., :D], yi[..., :D]
        for i in range(self.order):
            sr, si = xr[:, n - i: n - i + T], xi[:, n - i: n - i + T]
            cr, ci = c[:, :, 0, i], c[:, :, 1, i]
            ar = ar + cr * sr - ci * si
            ai = ai + cr * si + ci * sr
        yr = torch.cat([ar, yr[..., D:]], -1)
        yi = torch.cat([ai, yi[..., D:]], -1)
        return yr, yi, G, vad, (h1, h2, xr[:, T:], xi[:, T:])

    def init_state(self, batch: int, device=None):
        z = torch.zeros(1, batch, self.h, device=device)
        if not self.D:
            return (z,)
        hist = torch.zeros(batch, self.order - 1, self.D, device=device)
        return (z, z.clone(), hist, hist.clone())

    def forward(self, X: torch.Tensor):
        Xt = X.transpose(1, 2)
        yr, yi, G, vad, _ = self._core(Xt.real, Xt.imag, self.init_state(X.shape[0], X.device))
        return torch.complex(yr.float(), yi.float()).transpose(1, 2), {"vad": vad, "gain": G.transpose(1, 2)}

    def step(self, x: torch.Tensor, state):
        """x: complex [B, F] (one frame)."""
        yr, yi, _, vad, state = self._core(x.real[:, None], x.imag[:, None], state)
        return torch.complex(yr[:, 0], yi[:, 0]), state, torch.sigmoid(vad[:, 0])

    def step_real(self, re: torch.Tensor, im: torch.Tensor, *flat):
        """ONNX-friendly step (no complex tensors). re/im [B, F] -> (re, im, *state, vad prob)."""
        yr, yi, _, vad, state = self._core(re[:, None], im[:, None], tuple(flat))
        return (yr[:, 0], yi[:, 0], *state, torch.sigmoid(vad[:, 0]))

    def flat_state(self, state):
        return list(state)

    def unflat_state(self, flat):
        return tuple(flat)
