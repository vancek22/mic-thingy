"""Compare denoisers through the real C program (mic-thingy-file) on held-out speakers.

    python evaluate.py --bank <test prefix> --exe ../../build/mic-thingy-file.exe --work <dir> [--tag T] [--extra "set nn floor -30"]

Builds a fixed set of mixtures (test speakers, every noise family, SNR 0/5/10/20 dB, 6 s each; plus the Polish TTS
voice from build/audiotest if present), runs each configuration and prints per-family means of
  SI-SDR (whole file), dSI-SDR vs the noisy input, pause attenuation (noise removed where nobody speaks, dB),
  speech level change (dB, should be ~0) and the share of "lost speech" frames (> 6 dB quieter than the clean voice).
Results go to <work>/results_<tag>.csv as well.
"""
import argparse
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor

import numpy as np
import scipy.signal as sig
import soundfile as sf

import nnlib as L

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = ["-c", "bypass highpass on", "-c", "bypass gate on", "-c", "bypass comp on", "-c", "bypass eq on",
        "-c", "bypass limiter on"]
CONFIGS = {
    "rnnoise": BASE,
    "nn": BASE + ["-c", "bypass rnnoise on", "-c", "bypass nn off"],
    "rnn+nn": BASE + ["-c", "bypass nn off"],
}
FAMILIES = ["colored", "fan", "hum", "keyboard", "mouse", "clatter", "thump", "rustle", "music", "beeps", "sfx",
            "traffic", "babble"]


def speech_track(rng, bank, n, tts=None):
    clean = np.zeros(n, np.float32)
    pos = int(0.5 * L.SR)
    while pos < n:
        if tts is not None:
            a = int(rng.integers(0, len(tts) - 3 * L.SR))
            u = tts[a:a + int(rng.uniform(1.5, 3.0) * L.SR)]
        else:
            u = L.up3(bank.utterance(rng)[0])
        m = min(len(u), n - pos)
        clean[pos:pos + m] = u[:m]
        pos += m + int(rng.uniform(0.3, 1.0) * L.SR)
    return clean * (10 ** (rng.uniform(-35, -22) / 20) / L.active_rms(clean))


def family_noise(rng, fam, n, bank):
    if fam == "babble":
        y = L.n_babble(rng, n, bank)
    else:
        y = getattr(L, "n_" + fam)(rng, n)
    return y / (np.sqrt((y ** 2).mean()) + 1e-9)


def build_set(work, bank, tts):
    os.makedirs(work, exist_ok=True)
    items = []
    rng = np.random.default_rng(12345)
    n = 6 * L.SR
    for fam in FAMILIES:
        for snr in (0, 5, 10, 20):
            for rep in range(2):
                use_tts = tts is not None and rep == 1
                clean = speech_track(rng, bank, n, tts if use_tts else None)
                noise = family_noise(rng, fam, n, bank) * L.active_rms(clean) / 10 ** (snr / 20)
                noisy = clean + noise + rng.standard_normal(n).astype(np.float32) * 1e-4
                pk = np.abs(noisy).max()
                if pk > 0.99:
                    clean, noisy = clean * 0.99 / pk, noisy * 0.99 / pk
                name = f"{fam}_{snr}_{'pl' if use_tts else 'en'}{rep}"
                sf.write(os.path.join(work, name + "_clean.wav"), clean, L.SR, subtype="FLOAT")
                sf.write(os.path.join(work, name + "_noisy.wav"), noisy, L.SR, subtype="FLOAT")
                items.append((name, fam, snr))
    return items


def align(out, ref, maxlag=2400):
    c = sig.fftconvolve(out, ref[::-1], mode="full")
    mid = len(ref) - 1
    lag = int(np.argmax(c[mid:mid + maxlag + 1]))
    return out[lag:lag + len(ref) - maxlag], ref[:len(ref) - maxlag], lag


def si_sdr(est, ref):
    a = (est * ref).sum() / ((ref ** 2).sum() + 1e-12)
    t = a * ref
    return 10 * np.log10((t ** 2).sum() / (((est - t) ** 2).sum() + 1e-12) + 1e-12)


def metrics(out, noisy, clean):
    o, c, lag = align(out, clean)
    nz = noisy[:len(c)]
    fr = lambda x: x[: len(x) // 480 * 480].reshape(-1, 480)
    ec, eo, en = [(fr(x) ** 2).mean(1) for x in (c, o, nz)]
    act = ec > ec.max() * 10 ** (-35 / 10)
    pause = ec < ec.max() * 10 ** (-60 / 10)
    pa = 10 * np.log10(en[pause].mean() / (eo[pause].mean() + 1e-15)) if pause.sum() > 20 else np.nan
    lvl = 10 * np.log10(eo[act].sum() / ec[act].sum())
    lost = float((10 * np.log10(eo[act] / ec[act] + 1e-12) < -6).mean())
    return si_sdr(o, c), si_sdr(nz, c), pa, lvl, lost, lag


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bank", required=True)
    ap.add_argument("--exe", default=os.path.join(HERE, "..", "..", "build", "mic-thingy-file.exe"))
    ap.add_argument("--work", required=True)
    ap.add_argument("--tag", default="run")
    ap.add_argument("--extra", default="")
    ap.add_argument("--configs", default="rnnoise,nn,rnn+nn")
    a = ap.parse_args()
    bank = L.SpeechBank(a.bank)
    tts_path = os.path.join(HERE, "..", "..", "build", "audiotest", "speech_pl.wav")
    tts = sf.read(tts_path, dtype="float32")[0] if os.path.exists(tts_path) else None
    setdir = os.path.join(a.work, "set")
    marker = os.path.join(setdir, "items.txt")
    if os.path.exists(marker):
        items = [(l.split()[0], l.split()[1], int(l.split()[2])) for l in open(marker)]
    else:
        items = build_set(setdir, bank, tts)
        with open(marker, "w") as f:
            f.writelines(f"{n} {fam} {s}\n" for n, fam, s in items)
    extra = []
    for c in [x.strip() for x in a.extra.split(";") if x.strip()]:
        extra += ["-c", c]
    rows = []
    for cfg in a.configs.split(","):
        od = os.path.join(a.work, f"out_{a.tag}_{cfg}")
        os.makedirs(od, exist_ok=True)

        def run(it):
            name = it[0]
            outp = os.path.join(od, name + ".wav")
            subprocess.run([a.exe, "-i", os.path.join(setdir, name + "_noisy.wav"), "-o", outp] + CONFIGS[cfg] + extra,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            out = sf.read(outp, dtype="float32")[0]
            noisy = sf.read(os.path.join(setdir, name + "_noisy.wav"), dtype="float32")[0]
            clean = sf.read(os.path.join(setdir, name + "_clean.wav"), dtype="float32")[0]
            return (cfg,) + it + metrics(out, noisy, clean)

        with ThreadPoolExecutor(16) as ex:
            rows += list(ex.map(run, items))
    import csv
    with open(os.path.join(a.work, f"results_{a.tag}.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["config", "name", "family", "snr", "sisdr", "sisdr_in", "pause_att", "level", "lost", "lag"])
        w.writerows(rows)
    cfgs = a.configs.split(",")
    print(f"{'family':10s} " + " ".join(f"{c:>26s}" for c in cfgs))
    print(f"{'':10s} " + " ".join(f"{'dSDR  pause  lvl  lost%':>26s}" for _ in cfgs))
    for fam in FAMILIES + ["ALL"]:
        line = f"{fam:10s} "
        for c in cfgs:
            r = [x for x in rows if x[0] == c and (fam == "ALL" or x[2] == fam)]
            d = np.mean([x[4] - x[5] for x in r])
            pa = np.nanmean([x[6] for x in r])
            lv = np.mean([x[7] for x in r])
            lo = 100 * np.mean([x[8] for x in r])
            line += f"{d:8.2f} {pa:6.1f} {lv:5.1f} {lo:5.1f}"
        print(line)
    lags = sorted(set(x[-1] for x in rows))
    print("lags seen:", lags[:10])


if __name__ == "__main__":
    main()
