"""STFT helpers shared by every model (analysis/synthesis must match the streaming path exactly).

Frames: n_fft samples, hop = n_fft / 2, no centring. Algorithmic latency = n_fft - hop + 0 (the overlap-add
of the last half frame): 10 ms with the defaults (960/480 at 48 kHz, 320/160 at 16 kHz).
Both windows are power complementary at 50 % overlap (w[n]^2 + w[n + hop]^2 = 1), so analysis + synthesis
with the same window reconstructs perfectly.
"""
import math

import torch


def make_window(kind: str, n: int) -> torch.Tensor:
    i = torch.arange(n, dtype=torch.float64)
    if kind == "vorbis":
        w = torch.sin(0.5 * math.pi * torch.sin(math.pi * (i + 0.5) / n) ** 2)
    elif kind == "hann-sqrt":
        w = torch.sqrt(0.5 - 0.5 * torch.cos(2 * math.pi * (i + 0.5) / n))
    else:
        raise ValueError(kind)
    return w.float()


class Stft(torch.nn.Module):
    def __init__(self, n_fft: int, hop: int, window: str = "vorbis"):
        super().__init__()
        if hop * 2 != n_fft:
            raise ValueError("hop must be n_fft / 2 (perfect reconstruction with the power-complementary window)")
        self.n_fft, self.hop = n_fft, hop
        self.register_buffer("window", make_window(window, n_fft), persistent=False)

    @property
    def bins(self) -> int:
        return self.n_fft // 2 + 1

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """[B, T] -> complex [B, F, frames]"""
        return torch.stft(x, self.n_fft, self.hop, window=self.window, center=False, return_complex=True)

    def inverse(self, X: torch.Tensor, length: int) -> torch.Tensor:
        return torch.istft(X, self.n_fft, self.hop, window=self.window, center=False, length=length)

    def latency(self) -> int:
        return self.n_fft - self.hop


def stft_for(sample_rate: int, n_fft: int = 0) -> tuple[int, int]:
    """Default 20 ms window / 10 ms hop for the sample rate."""
    n = n_fft or int(round(0.02 * sample_rate))
    return n, n // 2
