"""Live scenario: which camera copy drives Sora? Turn one candidate matrix by a yaw on every write and
measure the image shift separately in Sora's, Donald's and the world's screen regions.

  camera_copy_edit.py <run> [--yaw 6] [--only NAME]

Needs the game at the Back Alley test spot (kh2_flow.py load_spot) with the probe running. For each
candidate: arm a write watch on the matrix's translation w, snapshot at yaw 0, yaw on, yaw 0 again
(control), and measure the shift between the snapshots per region. Prints one line per candidate and
region, then a verdict per candidate:
  MOVES_SORA_ONLY / MOVES_WORLD_ONLY / MOVES_BOTH / NO_EFFECT / UNSTABLE (control pair not at rest).
The probe's watch hit counts are listed so a candidate that is never written isn't read as NO_EFFECT.
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
PY = sys.executable
sys.path.insert(0, str(ROOT / "scripts"))
from measure_shift import load_gray, phase_corr  # noqa: E402

# Row-vector view matrices: rows at +0, translation at +0x30, its w at +0x3c (watched).
# (name, watched address, include_t): include_t=1 rotates rows + translation (camera yaw for a view
# matrix); 0 rotates rows only (camera yaw in place for a camera-to-world matrix).
CANDIDATES = [
    ("M2_frame_view", "exe+0x8aedbc", 1),     # control: world + companions move, Sora doesn't (2026-11-10)
    ("M1_frame_input", "exe+0x8aed7c", 1),
    ("M1_frame_input_rows", "exe+0x8aed7c", 0),
    ("setter_view", "exe+0xac1064", 1),
    ("setter_view_rows", "exe+0xac1064", 0),
    ("setter_view_copy", "exe+0xac1b94", 1),
    ("setter_view_copy_rows", "exe+0xac1b94", 0),
]
REGIONS = {  # fractions of the frame at the test spot (spot reference runs/2026-11-10-auto4/spot1.bmp)
    "sora": (0.44, 0.42, 0.56, 0.75),
    "donald": (0.30, 0.62, 0.43, 0.92),
    "world": (0.03, 0.08, 0.38, 0.45),   # left-hand buildings: no HUD, no sky, no characters
}
MOVE_PX = 20.0   # a region counts as moved above this shift
REST_PX = 6.0    # the control pair (yaw 0 vs yaw 0) must stay below this


def kh2run(*a):
    r = subprocess.run([PY, "-I", str(ROOT / "scripts" / "kh2run.py")] + list(a), capture_output=True, text=True)
    return r.returncode, r.stdout.strip()


def shift(a, b, region):
    sa, _ = load_gray(a, 1.0, region)
    sb, _ = load_gray(b, 1.0, region)
    dx, dy, _ = phase_corr(sa, sb)
    return dx


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--yaw", type=float, default=6.0)  # ~110 px at 102 deg hfov: inside phase correlation's range
    ap.add_argument("--only")
    a = ap.parse_args(argv)
    run = a.run
    d = ROOT / "runs" / run
    results = {}
    for name, addr, inc_t in CANDIDATES:
        if a.only and name != a.only:
            continue
        kh2run("set", run, "watch0=%s" % addr, "watch_len=4,4,4", "mat_slot=0", "mat_rows_offset=60", "mat_include_t=%d" % inc_t,
               "mat_threads=3", "mat_yaw_deg=0", "--cmd", "watch")
        time.sleep(1.5)
        snaps = []
        for tag, yaw in (("0", 0), ("on", a.yaw), ("back", 0)):
            kh2run("set", run, "mat_yaw_deg=%g" % yaw)
            time.sleep(1.5)
            rc, msg = kh2run("snap", run, "cc_%s_%s" % (name, tag))
            if rc:
                print("ERROR %s snapshot %s: %s" % (name, tag, msg))
                return 2
            snaps.append(d / ("cc_%s_%s.bmp" % (name, tag)))
        kh2run("set", run, "--cmd", "watch_report")
        time.sleep(1.0)
        kh2run("set", run, "mat_yaw_deg=0", "--cmd", "unwatch")
        time.sleep(1.0)
        moved, rest = {}, {}
        for rn, reg in REGIONS.items():
            moved[rn] = shift(snaps[0], snaps[1], reg)
            rest[rn] = shift(snaps[0], snaps[2], reg)
            print("%-18s %-7s on-off %+7.1f px   control %+5.1f px" % (name, rn, moved[rn], rest[rn]))
        if max(abs(v) for v in rest.values()) > REST_PX:
            v = "UNSTABLE"
        else:
            s, w = abs(moved["sora"]) > MOVE_PX, abs(moved["world"]) > MOVE_PX
            v = {(True, True): "MOVES_BOTH", (True, False): "MOVES_SORA_ONLY", (False, True): "MOVES_WORLD_ONLY",
                 (False, False): "NO_EFFECT"}[(s, w)]
        results[name] = v
        print("VERDICT %s %s" % (name, v))
    log = d / "kh2vr_probe.log"
    if log.exists():
        lines = [ln.rstrip() for ln in open(log, encoding="utf-8", errors="replace") if "WATCH_REPORT" in ln or "watch: slots" in ln]
        print("watch report lines (last 12):")
        for ln in lines[-12:]:
            print("  " + ln[:200])
    return 0 if results else 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
