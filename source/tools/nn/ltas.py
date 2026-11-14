"""Long-term average speech spectrum targets for the calibration's auto-EQ (src/app/calibrate.c).

    python ltas.py <bank prefix> [<bank prefix> ...]

Per-bin mean power of voiced frames (960-point Vorbis frames at 48 kHz, frames within 30 dB of the
utterance peak) in four bands; prints the low/mid/high band levels relative to the 300-1000 Hz band,
per speaker median. LibriSpeech has nothing above 8 kHz, so the high band stops there.
"""
import sys
import numpy as np
import nnlib as L

BANDS = {"low": (100, 250), "ref": (300, 1000), "mid": (2000, 4000), "high": (5000, 8000)}


def band_levels(x):
    nfr = (len(x) - L.N) // L.HOP + 1
    idx = np.arange(nfr)[:, None] * L.HOP + np.arange(L.N)[None, :]
    P = np.abs(np.fft.rfft(x[idx] * L.VORBIS, axis=1)) ** 2
    e = P.sum(1)
    act = e > e.max() * 1e-3
    f = np.arange(L.NBINS) * 50.0
    return {k: P[act][:, (f >= a) & (f < b)].mean() for k, (a, b) in BANDS.items()}


res = {k: [] for k in ("low", "mid", "high")}
for pre in sys.argv[1:]:
    b = L.SpeechBank(pre)
    for spk in range(b.n_spk):
        sel = b.idx[b.idx[:, 2] == spk][:20]
        acc = {k: 0.0 for k in BANDS}
        for s, n, _ in sel:
            lv = band_levels(L.up3(np.asarray(b.audio[s:s + n], np.float32) / 32768))
            for k in BANDS:
                acc[k] += lv[k]
        for k in res:
            res[k].append(10 * np.log10(acc[k] / acc["ref"]))
for k, v in res.items():
    v = np.array(v)
    print(f"{k}: median {np.median(v):.1f} dB  (p10 {np.percentile(v, 10):.1f}, p90 {np.percentile(v, 90):.1f}), {len(v)} speakers")
