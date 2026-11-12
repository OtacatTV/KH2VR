"""Frame-to-frame shifts in a burst from frame_diff_capture (out_%03d.bmp), to spot views that flip back and forth.

If some frames carry a camera edit and others don't, consecutive shifts alternate in sign with a size near
the edit's own shift (e.g. ~+-600 px for a 30 deg yaw at full width). A consistent camera gives small shifts
of one sign while orbiting.
Usage: python -I burst_shifts.py "<folder>/burst_%03d.bmp" [--region x0,y0,x1,y1] [--jump PX] [--scale N]
  --scale  the capture's downsample step, so shifts are reported in full-resolution pixels (default 4)
  --jump   full-resolution |dx| counted as a jump (default 150)
"""
import argparse
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from measure_shift import load_gray, phase_corr  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pattern")
    ap.add_argument("--region", type=lambda v: [float(x) for x in v.split(",")], default=[0.25, 0.25, 0.75, 0.75])
    ap.add_argument("--jump", type=float, default=150.0)
    ap.add_argument("--scale", type=float, default=4.0)
    args = ap.parse_args()
    files = sorted(glob.glob(args.pattern.replace("%03d", "[0-9][0-9][0-9]")))
    if len(files) < 2:
        sys.exit("need at least 2 frames")
    frames = [load_gray(f, 1.0, args.region)[0] for f in files]
    dxs = []
    for a, b in zip(frames, frames[1:]):
        dx, dy, sharp = phase_corr(a, b)
        dxs.append((dx * args.scale, dy * args.scale, sharp))
    jumps = [d for d in dxs if abs(d[0]) >= args.jump]
    flips = sum(1 for p, q in zip(dxs, dxs[1:]) if abs(p[0]) >= args.jump and abs(q[0]) >= args.jump and p[0] * q[0] < 0)
    print("dx per step (full-res px): " + " ".join(f"{d[0]:+.0f}" for d in dxs))
    small = sorted(abs(d[0]) for d in dxs if abs(d[0]) < args.jump)
    med = small[len(small) // 2] if small else float("nan")
    print(f"frames={len(files)} steps={len(dxs)} jumps(|dx|>={args.jump:.0f})={len(jumps)} sign_flips_between_jumps={flips} "
          f"median_small_|dx|={med:.1f} low_confidence_steps(peak<3)={sum(1 for d in dxs if d[2] < 3)}")
    low = sum(1 for d in dxs if d[2] < 3)
    print("VERDICT " + ("INCONCLUSIVE (most steps low confidence)" if low > len(dxs) // 2 else
                        "FLICKER (edit missing on some frames)" if flips >= 2 else
                        "CONSISTENT (no back-and-forth jumps)" if not jumps else "INCONCLUSIVE (isolated jumps)"))


if __name__ == "__main__":
    main()
