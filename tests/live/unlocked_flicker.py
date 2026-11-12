"""Live scenario (Frame Rate = Unlocked): do the render thread's in-between frames keep the per-pass edits?

  unlocked_flicker.py <run> [--frames 60]

fn exe+0x125d30 (the per-pass camera setup where the stereo edits go) runs only on the main thread
(2026-11-10), while at Unlocked the render thread presents extra frames. Method (camera at rest): capture
a reference burst with no edit, then a burst with each edit, and measure every frame's absolute shift
against the reference's first frame in a fixed region. If every edited frame shows the edit's shift, the
extra frames keep it; a mix of ~0 and the full shift means the extra frames don't (that edit would flicker).
Prints the present rate per phase from the probe log: with no extra frames (presents ~= main-thread presents)
the case can't tell and is reported INCONCLUSIVE.
  projection: rdx -> row2.x set to 0.25 (whole image +320 px)
  eye:        r8 -> view translation x += 60 (world region ~ +70 px)
"""
import argparse
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
PY = sys.executable
sys.path.insert(0, str(ROOT / "scripts"))
from measure_shift import load_gray, phase_corr  # noqa: E402

CASES = {
    "reference": [],
    "projection": ["poke_slot=0", "poke_reg=1", "poke_offset=32", "poke_set=1", "poke_add=0.25"],
    "eye": ["poke_slot=0", "poke_reg=2", "poke_offset=48", "poke_set=0", "poke_add=60"],
}
REGION = (0.03, 0.08, 0.38, 0.45)  # left buildings (static at rest)


def kh2run(*a):
    r = subprocess.run([PY, "-I", str(ROOT / "scripts" / "kh2run.py")] + list(a), capture_output=True, text=True)
    return r.returncode, r.stdout.strip()


def last_stats(log):
    lines = [ln for ln in open(log, encoding="utf-8", errors="replace") if "STATS window" in ln]
    m = re.search(r"presents=\d+ \(([\d.]+)/s\) on_main_thread=(\d+) onframe=\d+ \(([\d.]+)/s\)", lines[-1]) if lines else None
    return (float(m.group(1)), float(m.group(3))) if m else (0.0, 0.0)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--frames", type=int, default=60)
    a = ap.parse_args(argv)
    d = ROOT / "runs" / a.run
    tool = ROOT / "build" / "tools" / "frame_diff_capture.exe"
    ref0 = None
    for case, sets in CASES.items():
        kh2run("set", a.run, "watch0=exe+0x125d30", "watch_rw=0,1,1", "watch_len=1,4,4", "exec_log=0", "mat_slot=-1",
               "poke_slot=-1", "poke_add=0", *sets, "--cmd", "watch")
        time.sleep(1.5)
        out = d / ("rest_%s" % case)
        out.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(tool), str(out / "f_%03d.bmp"), str(a.frames), "KINGDOM HEARTS", "4"], capture_output=True, text=True, timeout=60)
        time.sleep(5.5)  # one STATS window while the edit is still on
        rate, main_rate = last_stats(d / "kh2vr_probe.log")
        kh2run("set", a.run, "poke_add=0", "poke_slot=-1", "--cmd", "unwatch")
        time.sleep(1.0)
        files = sorted(out.glob("f_*.bmp"))
        if not files:
            print("ERROR %s: no frames captured" % case)
            return 2
        frames = [load_gray(str(f), 1.0, REGION)[0] for f in files]
        if ref0 is None:
            ref0 = frames[0]
        dx = [phase_corr(ref0, f)[0] * 4 for f in frames]  # capture step 4 -> full-res px
        groups = {}
        for v in dx:
            groups.setdefault(int(round(v / 10.0)) * 10, 0)
            groups[int(round(v / 10.0)) * 10] += 1
        dist = " ".join("%+d:%d" % kv for kv in sorted(groups.items()))
        extra = rate - main_rate
        tag = "INCONCLUSIVE (no extra frames)" if extra < 20 else ("ONE_SHIFT" if len(groups) <= 2 and max(groups.values()) >= 0.9 * len(dx) else "MIXED")
        print("%-10s presents %.0f/s (main %.0f/s)  frames=%d  shift distribution (px:count) %s  median %+.0f  -> %s" % (
            case, rate, main_rate, len(dx), dist, statistics.median(dx), tag))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
