"""Count one-frame blips in a burst of consecutive frames: frame i differs from both neighbours while the
neighbours match each other (A-B-A). An in-between frame that renders something wrong (a missing HUD
element, a pose from the wrong time, a stale camera) shows up as a blip; smooth motion and genuine changes
don't (their neighbours differ from each other too). Limit: in regions that move every frame (camera
motion, animation) the neighbours differ too, so blips there go unseen (proven on synthetic moving noise,
2026-11-10); use scripts/burst_shifts.py (back-and-forth jumps) for moving content. Good for HUD regions.

  blip_count.py "<folder>/f_%03d.bmp" [--region name=x0,y0,x1,y1 ...] [--t 3.0] [--ret 0.35]

Per region: mean absolute grey-level difference between frames (frames downsampled to 1/2). A blip at i:
d(i-1,i) > t and d(i,i+1) > t and d(i-1,i+1) < ret * min(d(i-1,i), d(i,i+1)).
Prints per region: frames, blips, the worst blip's frame index and size; VERDICT PASS when no region has a
blip, FAIL otherwise (exit 1), ERROR (2) with fewer than 3 frames.
"""
import argparse
import glob
import sys

import numpy as np
from PIL import Image

DEFAULT = {"hud_cmd": (0.0, 0.62, 0.27, 0.98), "hud_right": (0.75, 0.5, 1.0, 1.0), "sora": (0.40, 0.35, 0.60, 0.80),
           "world": (0.03, 0.08, 0.38, 0.45)}


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("pattern")
    ap.add_argument("--region", action="append", default=[])
    ap.add_argument("--t", type=float, default=3.0)
    ap.add_argument("--ret", type=float, default=0.35)
    a = ap.parse_args(argv)
    regions = dict(DEFAULT)
    for r in a.region:
        n, v = r.split("=")
        regions[n] = tuple(float(x) for x in v.split(","))
    files = sorted(glob.glob(a.pattern.replace("%03d", "[0-9][0-9][0-9]")))
    if len(files) < 3:
        print("ERROR need at least 3 frames (%d)" % len(files))
        return 2
    ims = []
    for f in files:
        im = Image.open(f).convert("L")
        im = im.resize((im.width // 2, im.height // 2))
        ims.append(np.asarray(im, dtype=np.float32))
    h, w = ims[0].shape
    bad = False
    for name, (x0, y0, x1, y1) in regions.items():
        crop = [im[int(y0 * h):int(y1 * h), int(x0 * w):int(x1 * w)] for im in ims]
        d1 = [float(np.abs(crop[i + 1] - crop[i]).mean()) for i in range(len(crop) - 1)]
        blips = []
        for i in range(1, len(crop) - 1):
            a1, a2 = d1[i - 1], d1[i]
            if a1 > a.t and a2 > a.t:
                d02 = float(np.abs(crop[i + 1] - crop[i - 1]).mean())
                if d02 < a.ret * min(a1, a2):
                    blips.append((min(a1, a2), i))
        worst = max(blips) if blips else None
        print("%-10s frames=%d blips=%d%s  step diff mean %.2f max %.2f" % (
            name, len(crop), len(blips), (" worst=frame %d (%.1f)" % (worst[1], worst[0])) if worst else "",
            sum(d1) / len(d1), max(d1)))
        bad = bad or bool(blips)
    print("VERDICT %s" % ("FAIL" if bad else "PASS"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
