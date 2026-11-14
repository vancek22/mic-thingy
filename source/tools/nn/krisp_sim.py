"""Benchmark an external noise suppressor (e.g. the Krisp desktop app) on the same test set as evaluate.py.

Loop: this script plays the noisy mixtures into a virtual cable ("CABLE Input"), the external app takes
"CABLE Output" as its microphone and exposes its processed signal as a capture device ("Krisp Microphone"),
which this script records at the same time. The recording is aligned and cut back into items and scored with the
same metrics as evaluate.py, plus the clean-wideband high-band check.

    python krisp_sim.py --work <evaluate.py work dir> --tag krisp [--play "CABLE Input"] [--rec "Krisp"]
                        [--reps 1] [--families keyboard,fan,...]

Only the external app's OUTPUT is measured; nothing is trained on it (the model trains on clean speech).
Anything listening to CABLE Output (Discord, OBS) hears the test while it runs.
"""
import argparse
import csv
import os

import numpy as np
import scipy.signal as sig
import sounddevice as sd
import soundfile as sf

import evaluate as E
import nnlib as L


def find_dev(name, kind):
    for i, d in enumerate(sd.query_devices()):
        api = sd.query_hostapis(d["hostapi"])["name"]
        ch = d["max_input_channels"] if kind == "in" else d["max_output_channels"]
        if "WASAPI" in api and ch > 0 and name.lower() in d["name"].lower():
            return i, d
    raise SystemExit(f"no WASAPI {kind} device containing '{name}' - is the app running and signed in?")


PLAYER = r'''
import sys, numpy as np, sounddevice as sd, soundfile as sf
x, sr = sf.read(sys.argv[1], dtype="float32")
dev = int(sys.argv[2])
ch = sd.query_devices(dev)["max_output_channels"]
sd.play(np.repeat(x[:, None], ch, axis=1), samplerate=sr, device=dev)
sd.wait()
'''


def run_through(sig_in, play, rec):
    """Plays sig_in (mono 48 k) and records the app's output; returns the recording aligned to sig_in.
    The playback runs in a SEPARATE process: Krisp captures the speaker output of the program that records its
    microphone (the "call app") as the echo reference, so playing from the recording process gets the test
    signal cancelled as echo."""
    import subprocess
    import sys
    import tempfile
    pi, pd = find_dev(play, "out")
    ri, rd = find_dev(rec, "in")
    lead = np.concatenate([np.zeros(L.SR, np.float32), sig_in, np.zeros(2 * L.SR, np.float32)])
    tmp = os.path.join(tempfile.gettempdir(), "krisp_sim_play.wav")
    sf.write(tmp, lead, L.SR, subtype="FLOAT")
    n = len(lead) + 2 * L.SR
    rec_buf = sd.rec(n, samplerate=L.SR, channels=min(rd["max_input_channels"], 2), device=ri, dtype="float32")
    # started through WMI so it is not our child: Windows' per-process loopback capture includes the process tree
    script = os.path.join(tempfile.gettempdir(), "krisp_sim_player.py")
    with open(script, "w") as f:
        f.write(PLAYER)
    cmd = f'"{sys.executable}" "{script}" "{tmp}" {pi}'
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    f"Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{{CommandLine='{cmd}'}} | Out-Null"],
                   check=True)
    sd.wait()
    y = rec_buf.mean(1)
    if np.abs(y).max() < 1e-6:
        raise SystemExit(f"'{rd['name']}' recorded digital silence: the app is not signed in, not running, or its "
                         f"microphone is not set to the cable this script plays into ({pd['name']})")
    # global lag: cross-correlation of the full signals (speech survives any denoiser)
    c = sig.fftconvolve(y, lead[::-1], mode="full")
    mid = len(lead) - 1
    lag = int(np.argmax(c[mid:mid + 2 * L.SR]))      # the player process starts a little after the recording
    print(f"device round trip {lag / L.SR * 1000:.1f} ms")
    return y[L.SR + lag:L.SR + lag + len(sig_in)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--tag", default="krisp")
    ap.add_argument("--play", default="CABLE Input")
    ap.add_argument("--rec", default="Krisp")
    ap.add_argument("--reps", type=int, default=1, help="items per family and SNR (the set has 2)")
    ap.add_argument("--families", default="")
    a = ap.parse_args()
    setdir = os.path.join(a.work, "set")
    items = [(l.split()[0], l.split()[1], int(l.split()[2])) for l in open(os.path.join(setdir, "items.txt"))]
    fams = set(a.families.split(",")) if a.families else None
    seen = {}
    pick = []
    for it in items:
        k = (it[1], it[2])
        if (fams is None or it[1] in fams) and seen.get(k, 0) < a.reps:
            seen[k] = seen.get(k, 0) + 1
            pick.append(it)
    gap = np.zeros(L.SR, np.float32)
    parts, spans, pos = [], [], 0
    for name, _, _ in pick:
        x = sf.read(os.path.join(setdir, name + "_noisy.wav"), dtype="float32")[0]
        parts += [x, gap]
        spans.append((pos, len(x)))
        pos += len(x) + len(gap)
    prog = np.concatenate(parts)
    print(f"{len(pick)} items, {len(prog) / L.SR / 60:.1f} min of audio")
    y = run_through(prog, a.play, a.rec)
    od = os.path.join(a.work, f"out_{a.tag}")
    os.makedirs(od, exist_ok=True)
    rows = []
    for (name, fam, snr), (s, n) in zip(pick, spans):
        out = y[s:s + n]
        sf.write(os.path.join(od, name + ".wav"), out, L.SR, subtype="FLOAT")
        noisy = sf.read(os.path.join(setdir, name + "_noisy.wav"), dtype="float32")[0]
        clean = sf.read(os.path.join(setdir, name + "_clean.wav"), dtype="float32")[0]
        rows.append((a.tag, name, fam, snr) + E.metrics(out, noisy, clean))
    with open(os.path.join(a.work, f"results_{a.tag}.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["config", "name", "family", "snr", "sisdr", "sisdr_in", "pause_att", "level", "lost", "lag"])
        w.writerows(rows)
    print(f"{'family':16s} {'dSDR':>7s} {'pause':>6s} {'lvl':>5s} {'lost%':>6s}")
    for fam in sorted(set(r[2] for r in rows)) + ["ALL"]:
        r = [x for x in rows if fam == "ALL" or x[2] == fam]
        print(f"{fam:16s} {np.mean([x[4] - x[5] for x in r]):7.2f} {np.nanmean([x[6] for x in r]):6.1f} "
              f"{np.mean([x[7] for x in r]):5.1f} {100 * np.mean([x[8] for x in r]):6.1f}")
    # clean wideband speech: does the app keep the treble?
    tts_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "audiotest", "speech_pl.wav")
    if os.path.exists(tts_path):
        x = sf.read(tts_path, dtype="float32")[0][:20 * L.SR]
        yy = run_through(x, a.play, a.rec)
        o, c, _ = E.align(yy, x)
        nfr = (len(c) - L.N) // L.HOP + 1
        idx = np.arange(nfr)[:, None] * L.HOP + np.arange(L.N)[None, :]
        C = np.abs(np.fft.rfft(c[idx] * L.VORBIS, axis=1)) ** 2
        O = np.abs(np.fft.rfft(o[idx] * L.VORBIS, axis=1)) ** 2
        act = C.sum(1) > C.sum(1).max() * 1e-3
        f = np.arange(L.NBINS) * 50.0
        print("clean wideband speech, level change per band (dB): " + "  ".join(
            f"{a_}-{b_}: {10 * np.log10(O[act][:, (f >= a_) & (f < b_)].sum() / (C[act][:, (f >= a_) & (f < b_)].sum() + 1e-20)):.1f}"
            for a_, b_ in E.HF_BANDS))


if __name__ == "__main__":
    main()
