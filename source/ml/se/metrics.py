"""Objective metrics: SI-SDR, PESQ (optional `pesq` package), STOI (`pystoi`), DNSMOS (official ONNX models,
`onnxruntime`), plus alignment helpers for outputs of external tools (e.g. Krisp recordings) that carry a delay."""
from __future__ import annotations

import os
from typing import Dict, Optional

import numpy as np
import scipy.signal as sig

from . import audio as A

try:
    from pesq import pesq as _pesq
except Exception:                       # needs a C compiler on some platforms
    _pesq = None
try:
    from pystoi import stoi as _stoi
except Exception:
    _stoi = None


def si_sdr(est: np.ndarray, ref: np.ndarray) -> float:
    est = est - est.mean()
    ref = ref - ref.mean()
    a = (est * ref).sum() / ((ref ** 2).sum() + 1e-12)
    t = a * ref
    return float(10 * np.log10((t ** 2).sum() / (((est - t) ** 2).sum() + 1e-12) + 1e-12))


def align(est: np.ndarray, ref: np.ndarray, max_lag: int) -> tuple[np.ndarray, np.ndarray, int]:
    """Shift `est` back by its delay against `ref` (0..max_lag samples, cross-correlation)."""
    n = min(len(est), len(ref))
    c = sig.fftconvolve(est[:n], ref[:n][::-1], mode="full")
    mid = n - 1
    lag = int(np.argmax(c[mid:mid + max_lag + 1]))
    m = n - lag
    return est[lag:lag + m], ref[:m], lag


class Dnsmos:
    """DNSMOS P.835 (SIG, BAK, OVRL) from Microsoft's ONNX model (download_data.py --dnsmos). 16 kHz, 9 s windows."""

    def __init__(self, model_dir: str):
        import onnxruntime as ort
        path = os.path.join(model_dir, "sig_bak_ovr.onnx")
        if not os.path.exists(path):
            raise FileNotFoundError(path)
        self.sess = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
        self.inp = self.sess.get_inputs()[0].name

    @staticmethod
    def _poly(sig_, bak, ovr):
        # polynomial fit from the DNS-Challenge repository (personalised MOS off)
        p_ovr = np.poly1d([-0.06766283, 1.11546468, 0.04602535])
        p_sig = np.poly1d([-0.08397278, 1.22083953, 0.0052439])
        p_bak = np.poly1d([-0.13166888, 1.60915514, -0.39604546])
        return p_sig(sig_), p_bak(bak), p_ovr(ovr)

    def __call__(self, x: np.ndarray, sr: int) -> Dict[str, float]:
        x = A.resample(x, sr, 16000)
        win = int(9.01 * 16000)
        if len(x) < win:
            x = np.tile(x, int(np.ceil(win / max(1, len(x)))))
        hop = 16000
        res = []
        for s in range(0, max(1, len(x) - win + 1), hop):
            seg = x[s:s + win].astype(np.float32)[None]
            out = self.sess.run(None, {self.inp: seg})[0][0]
            res.append(self._poly(*out))
        r = np.mean(res, 0)
        return {"dnsmos_sig": float(r[0]), "dnsmos_bak": float(r[1]), "dnsmos_ovrl": float(r[2])}


def score(est: np.ndarray, clean: Optional[np.ndarray], noisy: Optional[np.ndarray], sr: int,
          dnsmos: Optional[Dnsmos] = None, max_lag: int = 0) -> Dict[str, float]:
    """All available metrics. With max_lag > 0, `est` is first aligned to the reference (external tools)."""
    out: Dict[str, float] = {}
    if clean is not None:
        e, c = est, clean
        if max_lag:
            e, c, lag = align(est, clean, max_lag)
            out["lag_ms"] = lag / sr * 1000
        n = min(len(e), len(c))
        e, c = e[:n], c[:n]
        if np.abs(c).max() > 1e-6:
            out["si_sdr"] = si_sdr(e, c)
            if noisy is not None:
                out["si_sdr_in"] = si_sdr(noisy[:n], c)
            if _stoi is not None:
                out["stoi"] = float(_stoi(c, e, sr, extended=False))
            if _pesq is not None:
                try:
                    out["pesq_wb"] = float(_pesq(16000, A.resample(c, sr, 16000), A.resample(e, sr, 16000), "wb"))
                except Exception:
                    pass
        # noise left where nobody speaks (10 ms frames 60 dB under the speech peak)
        hop = sr // 100
        k = n // hop
        ec = (c[: k * hop].reshape(k, hop) ** 2).mean(1)
        eo = (e[: k * hop].reshape(k, hop) ** 2).mean(1)
        pause = ec < ec.max() * 1e-6
        if noisy is not None and pause.sum() > 20:
            en = (noisy[: k * hop].reshape(k, hop) ** 2).mean(1)
            out["pause_atten_db"] = float(10 * np.log10(en[pause].mean() / (eo[pause].mean() + 1e-15)))
    if dnsmos is not None:
        out.update(dnsmos(est, sr))
    return out
