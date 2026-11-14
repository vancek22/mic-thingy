"""CLI demo.

    python demo.py --ckpt runs/crn48k/best.pt file.wav            # writes file.enh.wav + prints DNSMOS before/after
    python demo.py --ckpt runs/crn48k/best.pt --live --in "Mic" --out "Headphones"   # real-time, Ctrl+C to stop

--live opens a duplex audio stream (sounddevice) and runs the streaming path block by block: you hear your
microphone enhanced with the model's latency + the audio buffers. Use headphones (feedback!).
"""
import argparse
import os
import time

import numpy as np

from se import audio as A
from se import checkpoint
from se.metrics import Dnsmos
from se.stream import Streamer
from inference import enhance_offline


def find(name, kind):
    import sounddevice as sd
    for i, d in enumerate(sd.query_devices()):
        if name.lower() in d["name"].lower() and d["max_input_channels" if kind == "in" else "max_output_channels"] > 0:
            return i
    raise SystemExit(f"no {kind} device matching '{name}'")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("file", nargs="?")
    ap.add_argument("--live", action="store_true")
    ap.add_argument("--in", dest="inp", default="")
    ap.add_argument("--out", default="")
    ap.add_argument("--dnsmos-dir", default="data/dnsmos")
    a = ap.parse_args()
    model, cfg, stft, _ = checkpoint.load(a.ckpt)
    sr = cfg.data.sample_rate
    if a.live:
        import sounddevice as sd
        s = Streamer(model, stft)

        def cb(indata, outdata, frames, t, status):
            y = s.process(indata[:, 0].copy())
            outdata[:, 0] = np.pad(y, (0, frames - len(y)))[:frames]
        dev = (find(a.inp, "in") if a.inp else None, find(a.out, "out") if a.out else None)
        with sd.Stream(samplerate=sr, blocksize=stft.hop, channels=1, dtype="float32", callback=cb, device=dev):
            print(f"running ({stft.latency() / sr * 1000:.0f} ms algorithmic latency); Ctrl+C to stop")
            while True:
                time.sleep(0.5)
    if not a.file:
        ap.error("give a file or --live")
    x = A.load(a.file, sr)
    t0 = time.time()
    y = enhance_offline(model, stft, x)
    out = os.path.splitext(a.file)[0] + ".enh.wav"
    A.write(out, y, sr)
    print(f"{out}  ({len(x) / sr:.1f} s in {time.time() - t0:.2f} s)")
    try:
        d = Dnsmos(a.dnsmos_dir)
        b, e = d(x, sr), d(y, sr)
        print("DNSMOS  SIG/BAK/OVRL  before %.2f/%.2f/%.2f  after %.2f/%.2f/%.2f" % (
            b["dnsmos_sig"], b["dnsmos_bak"], b["dnsmos_ovrl"], e["dnsmos_sig"], e["dnsmos_bak"], e["dnsmos_ovrl"]))
    except Exception as ex:
        print(f"(DNSMOS unavailable: {ex})")


if __name__ == "__main__":
    main()
