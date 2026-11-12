"""Measure the global image shift between two snapshots (phase correlation, sub-pixel).

Used for the flat camera checks (RESEARCH.md section 6): a pure camera yaw of theta shifts every
scene point near the centre by (W/2) * tan(theta) / tan(hfov/2) pixels, regardless of depth.

Usage: python -I measure_shift.py a.bmp b.bmp [--crop 0.5 | --region x0,y0,x1,y1] [--hfov DEG] [--yaw DEG]
  --crop  fraction of width/height kept around the centre (keeps the HUD out)
  --hfov/--yaw  if both given, prints the predicted shift and the error
Prints: dx dy (pixels, b relative to a; +dx = content moved right), peak sharpness, verdict.
"""
import argparse
import math

import numpy as np
from PIL import Image


def load_gray(path, crop, region=None):
    im = np.asarray(Image.open(path).convert("L"), dtype=np.float64)
    h, w = im.shape
    if region:  # fractions x0,y0,x1,y1 of the image
        fx0, fy0, fx1, fy1 = region
        return im[int(h * fy0):int(h * fy1), int(w * fx0):int(w * fx1)], w
    ch, cw = int(h * crop), int(w * crop)
    y0, x0 = (h - ch) // 2, (w - cw) // 2
    return im[y0:y0 + ch, x0:x0 + cw], w


def phase_corr(a, b):
    win = np.outer(np.hanning(a.shape[0]), np.hanning(a.shape[1]))
    fa, fb = np.fft.fft2((a - a.mean()) * win), np.fft.fft2((b - b.mean()) * win)
    r = fb * np.conj(fa)
    r /= np.abs(r) + 1e-12
    c = np.fft.ifft2(r).real
    iy, ix = np.unravel_index(np.argmax(c), c.shape)
    peak = c[iy, ix]
    sharp = peak / (np.sort(c.ravel())[-50:-1].mean() + 1e-12)  # peak vs the next strongest responses

    def sub(cm, cp, c0):  # parabola through three samples
        d = cm - 2 * c0 + cp
        return 0.0 if d == 0 else 0.5 * (cm - cp) / d

    h, w = c.shape
    dx = ix + sub(c[iy, (ix - 1) % w], c[iy, (ix + 1) % w], peak)
    dy = iy + sub(c[(iy - 1) % h, ix], c[(iy + 1) % h, ix], peak)
    if dx > w / 2:
        dx -= w
    if dy > h / 2:
        dy -= h
    return dx, dy, sharp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--crop", type=float, default=0.5)
    ap.add_argument("--region", type=lambda v: [float(x) for x in v.split(",")],
                    help="x0,y0,x1,y1 as fractions of the image; overrides --crop")
    ap.add_argument("--hfov", type=float)
    ap.add_argument("--yaw", type=float)
    ap.add_argument("--tol", type=float, default=0.05, help="allowed relative error vs prediction")
    args = ap.parse_args()
    a, width = load_gray(args.a, args.crop, args.region)
    b, _ = load_gray(args.b, args.crop, args.region)
    dx, dy, sharp = phase_corr(a, b)
    print(f"shift dx={dx:.2f}px dy={dy:.2f}px peak_sharpness={sharp:.1f} (crop {args.crop:.2f}, image width {width})")
    if args.hfov and args.yaw is not None:
        pred = (width / 2) * math.tan(math.radians(args.yaw)) / math.tan(math.radians(args.hfov / 2))
        err = (abs(dx) - abs(pred)) / abs(pred) if pred else float("nan")
        ok = abs(err) <= args.tol and sharp >= 3
        print(f"predicted |dx|={abs(pred):.2f}px for yaw {args.yaw} deg at hfov {args.hfov:.3f}; "
              f"error {err * 100:+.1f}% -> VERDICT {'PASS' if ok else ('INCONCLUSIVE' if sharp < 3 else 'FAIL')}")


if __name__ == "__main__":
    main()
