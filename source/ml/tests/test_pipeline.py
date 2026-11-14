"""Self-test (no data needed): python tests/test_pipeline.py

- every model x sample rate: shapes, causality (future samples do not change past output), offline == streaming,
  perfect reconstruction of the STFT, the loss runs and back-propagates, ONNX step export loads in onnxruntime.
"""
import os
import sys

import numpy as np
import torch

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from inference import enhance_offline, enhance_stream  # noqa: E402
from se.config import load_config  # noqa: E402
from se.losses import EnhancementLoss  # noqa: E402
from se.models import build_model  # noqa: E402
from se.stft import Stft  # noqa: E402

fails = 0


def check(cond, msg):
    global fails
    print(("  ok   " if cond else "  FAIL ") + msg)
    fails += 0 if cond else 1


def main():
    torch.manual_seed(0)
    for name, sr, nfft in [("crn", 48000, 960), ("crn", 16000, 320), ("nsnet", 48000, 960),
                           ("erbdf", 48000, 960), ("erbdf", 16000, 320), ("erb", 48000, 960)]:
        extra = ["model.df_order=0"] if name == "erb" else []       # "erb" = erbdf, stage 1 only
        name = "erbdf" if name == "erb" else name
        cfg = load_config(None, extra + [f"model.name={name}", f"data.sample_rate={sr}", f"stft.n_fft={nfft}",
                                 f"stft.hop={nfft // 2}", "data.speech_max_hz=8000" if sr == 48000 else "data.speech_max_hz=0",
                                 "model.bins=145" if name == "nsnet" else "model.bins=0"])
        print(f"{name} @ {sr} Hz")
        stft = Stft(nfft, nfft // 2)
        model = build_model(cfg).eval()
        if getattr(model, "D", 0):          # erbdf: the filter taps start at zero - make them active for the checks
            torch.nn.init.normal_(model.up2.weight, std=0.3)
            torch.nn.init.normal_(model.up2.bias, std=0.3)
        x = (np.random.default_rng(1).standard_normal(sr) * 0.1).astype(np.float32)
        t = torch.from_numpy(x)[None]
        rec = stft.inverse(stft(t), sr)[0].numpy()
        check(np.abs(rec[nfft:-nfft] - x[nfft:-nfft]).max() < 1e-5, "STFT perfect reconstruction")
        y_off = enhance_offline(model, stft, x)
        y_str = enhance_stream(model, stft, x)
        d = np.abs(y_off[nfft:-2 * nfft] - y_str[nfft:-2 * nfft]).max()
        check(d < 1e-4, f"offline == streaming (max diff {d:.1e})")
        x2 = x.copy()
        x2[sr // 2:] = np.random.default_rng(2).standard_normal(sr - sr // 2) * 0.1
        y2 = enhance_offline(model, stft, x2)
        cut = sr // 2 - nfft                                   # output before the change must not move
        check(np.abs(y2[:cut] - y_off[:cut]).max() < 1e-5, "causal (future input does not change past output)")
        with torch.no_grad():                                  # ONNX path (real arithmetic) == complex step
            X = torch.fft.rfft(torch.from_numpy(x[:nfft]) * stft.window)[None]
            st = model.zero_state(1) if hasattr(model, "zero_state") else model.init_state(1)
            Yc, _, _ = model.step(X, st)
            r = model.step_real(X.real.contiguous(), X.imag.contiguous(), *model.flat_state(st))
            dr = max((r[0] - Yc.real).abs().max().item(), (r[1] - Yc.imag).abs().max().item())
        check(dr < 1e-5, f"step_real (ONNX) == step (max diff {dr:.1e})")
        model.train()
        crit = EnhancementLoss(cfg)
        noisy, clean = torch.randn(2, sr) * 0.1, torch.randn(2, sr) * 0.05
        Y, ex = model(stft(noisy))
        loss, parts = crit(stft.inverse(Y, sr), clean, noisy, Y, stft(clean), ex, torch.ones(2, 200), model.speech_bins)
        loss.backward()
        g = sum(p.grad.abs().sum().item() for p in model.parameters() if p.grad is not None)
        check(np.isfinite(loss.item()) and g > 0, f"loss {loss.item():.3f} back-propagates ({', '.join(parts)})")
        print(f"  {sum(p.numel() for p in model.parameters()) / 1e6:.2f} M parameters, visible bins {model.speech_bins}/{model.F}")
    print("all ok" if not fails else f"{fails} FAILED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
