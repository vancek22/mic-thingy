"""Real-time style processing: feed any number of samples, get the same number back, delayed by the
algorithmic latency (n_fft - hop). Internally one STFT frame per hop, the model's step() with carried state,
and windowed overlap-add - the same arithmetic as the offline path, so outputs match to float precision."""
from __future__ import annotations

import numpy as np
import torch


class Streamer:
    def __init__(self, model, stft, device="cpu"):
        self.model, self.stft, self.device = model, stft, device
        self.n, self.hop = stft.n_fft, stft.hop
        self.win = stft.window.to(device)
        self.reset()

    def reset(self):
        self.inbuf = np.zeros(self.n - self.hop, np.float32)      # previous half frame
        self.pending = np.zeros(0, np.float32)
        self.ola = torch.zeros(self.n - self.hop, device=self.device)
        self.state = self.model.init_state(1, self.device)
        self.vad = []
        self.primed = False      # the first hop only fills the buffer: frame 0 is samples 0..n-1, as offline

    @torch.no_grad()
    def process(self, x: np.ndarray) -> np.ndarray:
        self.pending = np.concatenate([self.pending, x.astype(np.float32)])
        out = []
        while len(self.pending) >= self.hop:
            chunk, self.pending = self.pending[: self.hop], self.pending[self.hop:]
            if not self.primed:
                self.inbuf, self.primed = chunk.copy(), True
                out.append(np.zeros(self.hop, np.float32))
                continue
            frame = np.concatenate([self.inbuf, chunk])
            self.inbuf = frame[self.hop:]
            f = torch.from_numpy(frame).to(self.device) * self.win
            X = torch.fft.rfft(f)[None]
            Y, self.state, vad = self.model.step(X, self.state)
            if vad is not None:
                self.vad.append(float(vad[0]))
            y = torch.fft.irfft(Y[0], self.n) * self.win
            o = self.ola + y[: self.n - self.hop]
            self.ola = y[self.n - self.hop:]
            out.append(o.cpu().numpy())
        return np.concatenate(out) if out else np.zeros(0, np.float32)

    def flush(self) -> np.ndarray:
        return self.process(np.zeros(self.n - self.hop + (-len(self.pending)) % self.hop, np.float32))
