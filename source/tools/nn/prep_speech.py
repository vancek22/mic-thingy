"""Turn a LibriSpeech split (FLAC, 16 kHz) into one flat int16 array + utterance index.

    python prep_speech.py <LibriSpeech/dev-clean (a split dir)> <out_prefix> [max_utt_per_speaker]

Writes <out_prefix>.npy (int16, 16 kHz, all utterances back to back) and <out_prefix>_idx.npy
(int64 array [n, 3]: start, length, speaker id). LibriSpeech is CC BY 4.0 (openslr.org/12); the audio
itself is never committed, only the trained weights.
"""
import os
import sys
import glob
from multiprocessing import Pool

import numpy as np
import soundfile as sf


def load(path):
    x, sr = sf.read(path, dtype="int16")
    assert sr == 16000
    return x


def main():
    root, out = sys.argv[1], sys.argv[2]
    max_per = int(sys.argv[3]) if len(sys.argv) > 3 else 10**9
    spk_dirs = sorted(glob.glob(os.path.join(root, "*")))
    files, spk = [], []
    rng = np.random.default_rng(0)
    for i, d in enumerate(spk_dirs):
        fl = sorted(glob.glob(os.path.join(d, "*", "*.flac")))
        if len(fl) > max_per:
            fl = [fl[j] for j in sorted(rng.choice(len(fl), max_per, replace=False))]
        files += fl
        spk += [i] * len(fl)
    with Pool(16) as p:
        wavs = p.map(load, files, chunksize=16)
    keep = [(w, s) for w, s in zip(wavs, spk) if len(w) >= 16000 * 1.5]
    flat = np.concatenate([w for w, _ in keep])
    idx, pos = [], 0
    for w, s in keep:
        idx.append((pos, len(w), s))
        pos += len(w)
    np.save(out + ".npy", flat)
    np.save(out + "_idx.npy", np.array(idx, dtype=np.int64))
    print(f"{len(keep)} utterances, {len(set(s for _, s in keep))} speakers, {len(flat) / 16000 / 3600:.2f} h -> {out}")


if __name__ == "__main__":
    main()
