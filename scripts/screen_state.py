"""Name the game screen in a snapshot by comparing it with labelled reference screens.

  screen_state.py add <name> <image> [--crop x0,y0,x1,y1]   store a reference (fractions of the image)
  screen_state.py match <image> [--min 0.85] [--margin 0.03]
  screen_state.py list

References are small grayscale thumbnails in runs/refs/screens/ (git-ignored: they're game imagery).
Use a crop for screens whose background moves (title menus over animated art): crop to the stable
menu text. Matching uses normalised correlation on 160x90 (crop-scaled) thumbnails.
Prints `STATE <name> score=<s> next=<name2> <s2>` then OK (exit 0), or FAIL (1) when the best score is
below --min or too close to the runner-up, or ERROR (2) with no references.
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
REFS = ROOT / "runs" / "refs" / "screens"
SIZE = (160, 90)


def thumb(path, crop=None):
    im = Image.open(path).convert("L")
    if crop:
        w, h = im.size
        im = im.crop((int(crop[0] * w), int(crop[1] * h), int(crop[2] * w), int(crop[3] * h)))
    a = np.asarray(im.resize(SIZE, Image.BILINEAR), dtype=np.float32)
    a -= a.mean()
    n = np.linalg.norm(a)
    return a / n if n > 1e-6 else a


def load_refs():
    refs = []
    for meta in sorted(REFS.glob("*.json")):
        m = json.loads(meta.read_text(encoding="utf-8"))
        refs.append((m["name"], m.get("crop"), np.load(meta.with_suffix(".npy"))))
    return refs


def main(argv):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="op", required=True)
    a = sub.add_parser("add")
    a.add_argument("name")
    a.add_argument("image")
    a.add_argument("--crop")
    m = sub.add_parser("match")
    m.add_argument("image")
    m.add_argument("--min", type=float, default=0.85)
    m.add_argument("--margin", type=float, default=0.03)
    sub.add_parser("list")
    args = ap.parse_args(argv)
    if args.op == "add":
        crop = [float(x) for x in args.crop.split(",")] if args.crop else None
        REFS.mkdir(parents=True, exist_ok=True)
        np.save(REFS / (args.name + ".npy"), thumb(args.image, crop))
        (REFS / (args.name + ".json")).write_text(json.dumps({"name": args.name, "crop": crop, "from": str(args.image)}), encoding="utf-8")
        print("OK reference %s added" % args.name)
        return 0
    refs = load_refs()
    if args.op == "list":
        for name, crop, _ in refs:
            print(name, crop or "")
        return 0
    if not refs:
        print("ERROR no reference screens in %s" % REFS)
        return 2
    scores = sorted(((float((thumb(args.image, crop) * t).sum()), name) for name, crop, t in refs), reverse=True)
    best = scores[0]
    nxt = scores[1] if len(scores) > 1 else (-1.0, "-")
    print("STATE %s score=%.3f next=%s %.3f" % (best[1], best[0], nxt[1], nxt[0]))
    if best[0] < args.min:
        print("FAIL best score below %.2f: unknown screen" % args.min)
        return 1
    if best[0] - nxt[0] < args.margin:
        print("FAIL ambiguous: %s vs %s" % (best[1], nxt[1]))
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
