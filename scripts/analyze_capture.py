"""Summarise a frame_diff_capture CSV: per-second distinct images, split by how much of the screen changed.

A camera-moving 3D render changes a large share of pixels; a HUD-only or noise change touches few.
Usage: python analyze_capture.py capture.csv [big_frac=0.2] [small_frac=0.005]
"""
import csv, sys, statistics

path = sys.argv[1]
big = float(sys.argv[2]) if len(sys.argv) > 2 else 0.2
small = float(sys.argv[3]) if len(sys.argv) > 3 else 0.005
rows = list(csv.DictReader(open(path)))[1:]
buckets = {}
for r in rows:
    s = int(float(r["t_ms"]) // 1000)
    b = buckets.setdefault(s, [0, 0, 0, 0])
    f = float(r["changed_frac"])
    b[0] += 1
    b[1] += f == 0
    b[2] += small <= f < big
    b[3] += f >= big
print(f"sec updates identical small_change big_change(>= {big:.0%} of samples)")
for s in sorted(buckets):
    print(f"{s:3d} {buckets[s][0]:7d} {buckets[s][1]:9d} {buckets[s][2]:12d} {buckets[s][3]:10d}")
moving = [b for b in buckets.values() if b[3] >= 30]
if moving:
    print(f"seconds with sustained motion (>=30 big changes): {len(moving)}; "
          f"median big changes/s {statistics.median(b[3] for b in moving)}, "
          f"median updates/s {statistics.median(b[0] for b in moving)}, "
          f"median identical/s {statistics.median(b[1] for b in moving)}")
fr = sorted(float(r["changed_frac"]) for r in rows)
q = lambda p: fr[min(len(fr) - 1, int(p * (len(fr) - 1)))]
print(f"changed_frac quantiles p10={q(.1):.3f} p25={q(.25):.3f} p50={q(.5):.3f} p75={q(.75):.3f} p90={q(.9):.3f}")
