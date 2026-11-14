"""Training losses.

total = w_mrstft * multi-resolution STFT (spectral convergence + log magnitude)
      + w_sisdr  * (-SI-SDR)
      + w_spec   * compressed complex spectral loss on the model's own STFT (magnitude^c with phase)
      + w_vad    * speech-presence BCE (models that predict it)

Band limit: when the clean speech is band-limited (speech_max_hz > 0), the time-domain losses compare
low-passed signals and the spectral loss uses only the bins below the limit. The target has no energy above
the limit even while someone speaks, so scoring those bins would teach the model to delete the treble of
real voices.
"""
from __future__ import annotations

import torch
import torch.nn.functional as F

from .config import Config


def si_sdr(est: torch.Tensor, ref: torch.Tensor, eps: float = 1e-8) -> torch.Tensor:
    """[B, T] -> [B] in dB."""
    est = est - est.mean(-1, keepdim=True)
    ref = ref - ref.mean(-1, keepdim=True)
    a = (est * ref).sum(-1, keepdim=True) / (ref.pow(2).sum(-1, keepdim=True) + eps)
    t = a * ref
    return 10 * torch.log10(t.pow(2).sum(-1) / ((est - t).pow(2).sum(-1) + eps) + eps)


def _lowpass_fft(x: torch.Tensor, sr: int, hz: float) -> torch.Tensor:
    X = torch.fft.rfft(x)
    f = torch.fft.rfftfreq(x.shape[-1], 1 / sr).to(x.device)
    return torch.fft.irfft(X * (f <= hz), x.shape[-1])


class MultiResStft(torch.nn.Module):
    def __init__(self, ffts, sr: int, max_hz: float = 0.0):
        super().__init__()
        self.ffts, self.sr, self.max_hz = list(ffts), sr, max_hz
        for n in self.ffts:
            self.register_buffer(f"w{n}", torch.hann_window(n), persistent=False)

    def forward(self, est, ref, noisy):
        """Signals are level-normalised by the noisy input (RMS ~1). Spectral convergence is per item with the
        denominator floored by a tenth of the noisy spectrum's norm: in a noise-only or mostly-pause example the
        target's norm is ~0 and the plain ratio explodes. The log magnitudes are floored at -80 dB re the input."""
        loss = 0.0
        for n in self.ffts:
            w = getattr(self, f"w{n}")
            E = torch.stft(est, n, n // 4, window=w, return_complex=True).abs()
            R = torch.stft(ref, n, n // 4, window=w, return_complex=True).abs()
            N = torch.stft(noisy, n, n // 4, window=w, return_complex=True).abs()
            if self.max_hz > 0:
                k = int(self.max_hz / (self.sr / n)) + 1
                E, R, N = E[:, :k], R[:, :k], N[:, :k]
            num = torch.linalg.vector_norm(R - E, dim=(1, 2))
            den = torch.maximum(torch.linalg.vector_norm(R, dim=(1, 2)), 0.1 * torch.linalg.vector_norm(N, dim=(1, 2)))
            sc = (num / (den + 1e-7)).mean()
            floor = 1e-4 * n ** 0.5                        # |X| of a unit-RMS signal scales with sqrt(n)
            lm = F.l1_loss(torch.log(E + floor), torch.log(R + floor))
            loss = loss + sc + lm
        return loss / len(self.ffts)


class EnhancementLoss(torch.nn.Module):
    def __init__(self, cfg: Config):
        super().__init__()
        self.cfg = cfg
        self.max_hz = cfg.data.speech_max_hz * 0.9 if (cfg.loss.band_limit and cfg.data.speech_max_hz > 0) else 0.0
        self.mr = MultiResStft(cfg.loss.mrstft_ffts, cfg.data.sample_rate, self.max_hz)

    def forward(self, est_wav, ref_wav, noisy_wav, Y, Xc, extras, vad_target, spec_bins: int):
        L, parts = self.cfg.loss, {}
        e, r, nz = est_wav, ref_wav, noisy_wav
        if self.max_hz > 0:
            sr = self.cfg.data.sample_rate
            e, r, nz = (_lowpass_fft(z, sr, self.max_hz) for z in (e, r, nz))
        # level-normalise by the NOISY input (never silent; the clean target is zero in noise-only examples),
        # so loud and quiet examples weigh the same
        s = nz.pow(2).mean(-1, keepdim=True).sqrt() + 1e-5
        total = 0.0
        if L.w_mrstft:
            parts["mrstft"] = self.mr(e / s, r / s, nz / s)
            total = total + L.w_mrstft * parts["mrstft"]
        if L.w_sisdr:
            has = r.pow(2).sum(-1) > 1e-8                  # SI-SDR is undefined without speech
            if has.any():
                parts["sisdr"] = si_sdr(e[has], r[has]).mean()
                total = total - L.w_sisdr * parts["sisdr"]
        if L.w_spec:
            c = L.spec_compress
            sc = s.unsqueeze(-1)
            Yb, Xb = Y[:, :spec_bins] / sc, Xc[:, :spec_bins] / sc

            def comp(Z):
                m = Z.abs()
                return (m + 1e-8) ** c, Z * ((m + 1e-8) ** (c - 1))

            ym, yc = comp(Yb)
            xm, xc = comp(Xb)
            parts["spec"] = F.mse_loss(ym, xm) + 0.3 * F.mse_loss(torch.view_as_real(yc), torch.view_as_real(xc))
            total = total + L.w_spec * parts["spec"]
        if L.w_vad and extras.get("vad") is not None and vad_target is not None:
            v = extras["vad"]
            n = min(v.shape[1], vad_target.shape[1])
            parts["vad"] = F.binary_cross_entropy_with_logits(v[:, :n].float(), vad_target[:, :n])
            total = total + L.w_vad * parts["vad"]
        return total, {k: float(v.detach()) if torch.is_tensor(v) else float(v) for k, v in parts.items()}
