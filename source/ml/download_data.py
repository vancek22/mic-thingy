"""Download training/evaluation data into data/ (see README for sizes and licences).

    python download_data.py --small            # LibriSpeech dev/test + train-clean-100, OpenSLR 28, DEMAND, DNSMOS
    python download_data.py --librispeech dev-clean test-clean --rirs --dnsmos
    python download_data.py --demand --demand-rate 16k

Everything lands in data/ (git-ignored). Archives are deleted after extraction unless --keep.
The full Microsoft DNS Challenge set (~1 TB) is fetched with Microsoft's own scripts - see README.
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
import tarfile
import urllib.request
import zipfile

LIBRISPEECH = "https://www.openslr.org/resources/12/{}.tar.gz"          # CC BY 4.0
RIRS_NOISES = "https://www.openslr.org/resources/28/rirs_noises.zip"   # Apache 2.0
DEMAND = "https://zenodo.org/records/1227121/files/{}_{}.zip?download=1"   # CC BY-SA 3.0
DEMAND_ENVS = ["DKITCHEN", "DLIVING", "DWASHING", "NFIELD", "NPARK", "NRIVER", "OHALLWAY", "OMEETING", "OOFFICE",
               "PCAFETER", "PRESTO", "PSTATION", "SCAFE", "SPSQUARE", "STRAFFIC", "TBUS", "TCAR", "TMETRO"]
DEMAND_VAL = {"OMEETING", "PSTATION", "TBUS"}                         # held out for validation
DNSMOS = "https://github.com/microsoft/DNS-Challenge/raw/master/DNSMOS/DNSMOS/{}"   # CC BY 4.0 (repo licence: MIT/CC)


def fetch(url: str, dst: str) -> str:
    if os.path.exists(dst):
        return dst
    tmp = dst + ".part"
    print(f"downloading {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "speech-enhancement-downloader"})
    with urllib.request.urlopen(req) as r, open(tmp, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        done = 0
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            f.write(b)
            done += len(b)
            if total:
                sys.stdout.write(f"\r  {done / 1e6:8.1f} / {total / 1e6:.1f} MB")
                sys.stdout.flush()
    print()
    os.replace(tmp, dst)
    return dst


def extract(archive: str, dst: str, keep: bool) -> None:
    print(f"extracting {os.path.basename(archive)}")
    if archive.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            for m in z.namelist():                       # no absolute paths / .. escapes
                if m.startswith("/") or ".." in m.split("/"):
                    raise RuntimeError(f"unsafe path in archive: {m}")
            z.extractall(dst)
    else:
        with tarfile.open(archive) as t:
            t.extractall(dst, filter="data")
    if not keep:
        os.remove(archive)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="data")
    ap.add_argument("--small", action="store_true", help="the recommended single-machine set (~9 GB download)")
    ap.add_argument("--librispeech", nargs="*", default=None, help="splits, e.g. dev-clean test-clean train-clean-100")
    ap.add_argument("--rirs", action="store_true", help="OpenSLR 28 room impulse responses + point-source noises (1.3 GB)")
    ap.add_argument("--demand", action="store_true", help="DEMAND multi-channel environment noise (channel 1 used)")
    ap.add_argument("--demand-rate", default="48k", choices=["48k", "16k"])
    ap.add_argument("--dnsmos", action="store_true", help="DNSMOS P.835 ONNX model")
    ap.add_argument("--keep", action="store_true", help="keep downloaded archives")
    a = ap.parse_args()
    if a.small:
        a.librispeech = a.librispeech or ["dev-clean", "test-clean", "train-clean-100"]
        a.rirs = a.demand = a.dnsmos = True
    os.makedirs(a.root, exist_ok=True)

    for split in a.librispeech or []:
        if os.path.isdir(os.path.join(a.root, "LibriSpeech", split)):
            print(f"LibriSpeech {split}: present")
            continue
        extract(fetch(LIBRISPEECH.format(split), os.path.join(a.root, f"{split}.tar.gz")), a.root, a.keep)

    if a.rirs:
        if os.path.isdir(os.path.join(a.root, "RIRS_NOISES")):
            print("RIRS_NOISES: present")
        else:
            extract(fetch(RIRS_NOISES, os.path.join(a.root, "rirs_noises.zip")), a.root, a.keep)

    if a.demand:
        for env in DEMAND_ENVS:
            dst = os.path.join(a.root, "demand_val" if env in DEMAND_VAL else "demand_train")
            if os.path.isdir(os.path.join(dst, env)):
                continue
            tmpdir = os.path.join(a.root, "_demand_tmp")
            extract(fetch(DEMAND.format(env, a.demand_rate), os.path.join(a.root, f"{env}_{a.demand_rate}.zip")), tmpdir, a.keep)
            # keep one microphone (ch01) per environment: 16 nearly identical channels add nothing
            src = next((os.path.join(r, f) for r, _, fs in os.walk(tmpdir) for f in fs if f.lower() == "ch01.wav"), None)
            os.makedirs(os.path.join(dst, env), exist_ok=True)
            if src:
                shutil.move(src, os.path.join(dst, env, "ch01.wav"))
            shutil.rmtree(tmpdir, ignore_errors=True)
            print(f"DEMAND {env} -> {dst}")

    if a.dnsmos:
        d = os.path.join(a.root, "dnsmos")
        os.makedirs(d, exist_ok=True)
        for f in ("sig_bak_ovr.onnx", "model_v8.onnx"):
            fetch(DNSMOS.format(f), os.path.join(d, f))

    os.makedirs(os.path.join(a.root, "real"), exist_ok=True)
    print("done. Put your own noisy recordings (no reference needed) into data/real for DNSMOS during validation.")


if __name__ == "__main__":
    main()
