"""DNSMOS P.835 (no reference needed) for several systems on the same evaluate.py / krisp_sim.py items.

    python dnsmos_compare.py --work <eval work dir> --systems noisy,out_base_rnnoise,out_r3s1500_nn,out_krisp

"noisy" = the inputs in <work>/set; any other name is a folder <work>/<name> with <item>.wav outputs.
Only items present for every system are scored. Prints per family and overall SIG / BAK / OVRL.
"""
import argparse
import os
import sys
from collections import defaultdict

import numpy as np
import soundfile as sf

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "ml"))
from se.metrics import Dnsmos  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--systems", required=True)
    ap.add_argument("--dnsmos-dir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "ml", "data", "dnsmos"))
    a = ap.parse_args()
    d = Dnsmos(a.dnsmos_dir)
    systems = a.systems.split(",")
    items = [l.split() for l in open(os.path.join(a.work, "set", "items.txt"))]

    def path(sys_, name):
        return os.path.join(a.work, "set", name + "_noisy.wav") if sys_ == "noisy" else os.path.join(a.work, sys_, name + ".wav")

    items = [it for it in items if all(os.path.exists(path(s, it[0])) for s in systems)]
    res = defaultdict(lambda: defaultdict(list))
    for k, (name, fam, snr) in enumerate(items):
        for s in systems:
            x, sr = sf.read(path(s, name), dtype="float32")
            r = d(x, sr)
            for m in ("dnsmos_sig", "dnsmos_bak", "dnsmos_ovrl"):
                res[(s, fam)][m].append(r[m])
                res[(s, "ALL")][m].append(r[m])
        print(f"{k + 1}/{len(items)}", end="\r")
    fams = sorted({it[1] for it in items}) + ["ALL"]
    print(f"{len(items)} items; DNSMOS SIG / BAK / OVRL")
    print(f"{'family':12s}" + "".join(f"{s[-18:]:>22s}" for s in systems))
    for f in fams:
        line = f"{f:12s}"
        for s in systems:
            v = res[(s, f)]
            line += f"   {np.mean(v['dnsmos_sig']):.2f}/{np.mean(v['dnsmos_bak']):.2f}/{np.mean(v['dnsmos_ovrl']):.2f}"
        print(line)


if __name__ == "__main__":
    main()
