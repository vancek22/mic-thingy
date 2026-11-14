"""Write an nsnet checkpoint (48 kHz, n_fft 960) into mic thingy's C weights (src/dsp/nn_weights.c) and the
golden test data (tests/nn_golden.h), reusing tools/nn/export.py's writer.

    python export_c.py runs/nsnet48k/best.pt
"""
import argparse
import os
import sys

import torch

from se import checkpoint

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools", "nn"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ckpt")
    a = ap.parse_args()
    model, cfg, _, _ = checkpoint.load(a.ckpt)
    if cfg.model.name != "nsnet" or cfg.data.sample_rate != 48000 or cfg.stft.n_fft != 960 or cfg.stft.window != "vorbis":
        raise SystemExit("the C effect runs nsnet at 48 kHz with 960-sample Vorbis frames")
    import nnlib
    import export as tools_export
    # tools/nn's model and writer use module constants: point them at this model's band layout
    nnlib.NB, nnlib.HF_LO, nnlib.HF_HI = model.speech_bins, model.hf_lo, model.hf_hi
    tmp = os.path.join(HERE, "runs", "_export_c.pt")
    os.makedirs(os.path.dirname(tmp), exist_ok=True)
    torch.save({"model": model.state_dict(), "hidden": model.h, "step": 0}, tmp)
    sys.argv = ["export.py", tmp]
    tools_export.main()
    os.remove(tmp)


if __name__ == "__main__":
    main()
