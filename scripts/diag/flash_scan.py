"""Find one-frame flashes in a RECOMP_GPU_PRESDUMP sequence.

A flash is a frame that differs strongly from both neighbours while the
neighbours themselves agree (A B A). Prints per-frame mean luma, the
transient score and the bounding box of the changed pixels.

    python scripts\\diag\\flash_scan.py analysis\\diag\\mf1\\pd [--thresh 6] [--all]
"""
import argparse
import glob
import os

import numpy as np
from PIL import Image


def load(p):
    return np.asarray(Image.open(p).convert("RGB").resize((320, 240)), dtype=np.int16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--thresh", type=float, default=6.0, help="mean abs diff that counts as a flash")
    ap.add_argument("--all", action="store_true", help="print every frame")
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.dir, "*.bmp")))
    if len(files) < 3:
        print("need >= 3 frames")
        return
    imgs = [load(f) for f in files]
    flashes = 0
    for i in range(len(imgs)):
        luma = imgs[i].mean()
        if 0 < i < len(imgs) - 1:
            dp = np.abs(imgs[i] - imgs[i - 1]).mean()
            dn = np.abs(imgs[i] - imgs[i + 1]).mean()
            dnb = np.abs(imgs[i - 1] - imgs[i + 1]).mean()
            score = min(dp, dn) - dnb
        else:
            dp = dn = dnb = score = 0.0
        tag = ""
        if score > a.thresh:
            flashes += 1
            m = (np.abs(imgs[i] - imgs[i - 1]).max(axis=2) > 40)  # box in 320x240 units
            ys, xs = np.nonzero(m)
            box = (xs.min(), ys.min(), xs.max(), ys.max()) if len(xs) else None
            tag = f"  FLASH box={box} changed={m.mean()*100:.1f}%"
        if tag or a.all:
            print(f"{os.path.basename(files[i])} luma={luma:6.1f} dprev={dp:5.1f} dnext={dn:5.1f} nb={dnb:5.1f}{tag}")
    print(f"frames={len(files)} flashes={flashes}")


if __name__ == "__main__":
    main()
