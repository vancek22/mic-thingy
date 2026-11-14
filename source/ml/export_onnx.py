"""Export a checkpoint as a STREAMING ONNX model: one STFT frame in, one enhanced frame out, state carried
explicitly (inputs state_0..N, outputs state_out_0..N). The host does the windowed FFT / overlap-add
(see se/stream.py for the exact arithmetic).

    python export_onnx.py runs/crn48k/best.pt [-o model.onnx]

Inputs:  re, im [1, F] (rfft of the windowed frame), state_* (zeros at start)
Outputs: re_out, im_out [1, F], state_out_* (feed back next frame) [, vad for nsnet]
The script runs 50 random frames through torch and onnxruntime and prints the largest difference.
"""
from __future__ import annotations

import argparse

import numpy as np
import torch

from se import checkpoint


class StepWrapper(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        self.m = model

    def forward(self, re, im, *state):
        return self.m.step_real(re, im, *state)


def zero_state(model):
    if hasattr(model, "zero_state"):
        return model.flat_state(model.zero_state(1))
    return model.flat_state(model.init_state(1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ckpt")
    ap.add_argument("-o", "--out")
    a = ap.parse_args()
    model, cfg, stft, _ = checkpoint.load(a.ckpt)
    out = a.out or a.ckpt.rsplit(".", 1)[0] + ".onnx"
    st = zero_state(model)
    F = model.F
    re, im = torch.randn(1, F), torch.randn(1, F)
    w = StepWrapper(model).eval()
    names_in = ["re", "im"] + [f"state_{i}" for i in range(len(st))]
    n_out = len(w(re, im, *st))
    names_out = ["re_out", "im_out"] + [f"state_out_{i}" for i in range(len(st))] + (["vad"] if n_out > 2 + len(st) else [])
    torch.onnx.export(w, (re, im, *st), out, input_names=names_in, output_names=names_out, opset_version=17,
                      dynamo=False)
    import onnxruntime as ort
    sess = ort.InferenceSession(out, providers=["CPUExecutionProvider"])
    s_t, s_o = st, [s.numpy() for s in st]
    worst = 0.0
    rng = np.random.default_rng(0)
    with torch.no_grad():
        for _ in range(50):
            r = rng.standard_normal((1, F)).astype(np.float32)
            i = rng.standard_normal((1, F)).astype(np.float32)
            t = w(torch.from_numpy(r), torch.from_numpy(i), *s_t)
            o = sess.run(None, {"re": r, "im": i, **{f"state_{k}": v for k, v in enumerate(s_o)}})
            worst = max(worst, float(np.abs(t[0].numpy() - o[0]).max()), float(np.abs(t[1].numpy() - o[1]).max()))
            s_t = list(t[2:2 + len(st)])
            s_o = o[2:2 + len(st)]
    print(f"wrote {out}: {len(st)} state tensors, frame = {cfg.stft.n_fft} samples / hop {cfg.stft.hop} at "
          f"{cfg.data.sample_rate} Hz, torch vs onnxruntime max diff {worst:.2e}")


if __name__ == "__main__":
    main()
