"""Live scenario (Frame Rate = Unlocked): which conditions make the render thread add in-between frames?

  extra_frames_matrix.py <run> [--secs 8] [--only NAME,...]

Each phase changes one thing and reads the probe's present counters (stats_ms is set to 1000 for the run):
total presents/s and main-thread presents/s; extra = total - main. Phases:
  idle              nothing
  idle_dup          Desktop Duplication capture running (frame_diff_capture CSV mode), pad idle
  sweep             right-stick camera sweep (tests/pad/orbit_right_long.pad)
  sweep_dup         the same with Desktop Duplication running
  walk              left-stick walk (tests/pad/walk_circle.pad)
  idle_watch        an execute breakpoint armed on fn exe+0x125d30 (~50 traps/tick), pad idle
  sweep_watch       sweep with that breakpoint armed
Prints one line per phase: median and max extra frames/s over the phase's 1 s windows.
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
RX = re.compile(r"^(\d\d):(\d\d):(\d\d)\.(\d+) STATS window=\d+ms presents=\d+ \(([\d.]+)/s\) on_main_thread=\d+ onframe=\d+ \(([\d.]+)/s\)")


import ctypes  # noqa: E402
_u32 = ctypes.WinDLL("user32")
_u32.FindWindowW.restype = ctypes.c_void_p
_u32.GetForegroundWindow.restype = ctypes.c_void_p


def fg_state():
    """'fg' when the game window is the foreground window, else 'bg' (or 'nowin')."""
    h = _u32.FindWindowW(None, "KINGDOM HEARTS - HD 1.5+2.5 ReMIX -")
    if not h:
        return "nowin"
    return "fg" if _u32.GetForegroundWindow() == h else "bg"


def kh2run(*a):
    r = subprocess.run([PY, "-I", str(ROOT / "scripts" / "kh2run.py")] + list(a), capture_output=True, text=True)
    return r.returncode, r.stdout.strip()


def stats_since(log, t0):
    out = []
    for ln in open(log, encoding="utf-8", errors="replace"):
        m = RX.match(ln)
        if not m:
            continue
        lt = time.localtime()
        ts = time.mktime((lt.tm_year, lt.tm_mon, lt.tm_mday, int(m.group(1)), int(m.group(2)), int(m.group(3)), 0, 0, -1)) + int(m.group(4)) / 1000
        if ts >= t0:
            out.append((float(m.group(5)), float(m.group(6))))
    return out


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--secs", type=float, default=8.0)
    ap.add_argument("--only")
    a = ap.parse_args(argv)
    log = ROOT / "runs" / a.run / "kh2vr_probe.log"
    tool = ROOT / "build" / "tools" / "frame_diff_capture.exe"
    kh2run("set", a.run, "stats_ms=1000", "poke_slot=-1", "poke_add=0", "--cmd", "unwatch")
    time.sleep(2)
    phases = [
        ("idle", None, False, False),
        ("idle_dup", None, True, False),
        ("sweep", "orbit_right_long.pad", False, False),
        ("sweep_dup", "orbit_right_long.pad", True, False),
        ("walk", "walk_circle.pad", False, False),
        ("idle_watch", None, False, True),
        ("sweep_watch", "orbit_right_long.pad", False, True),
    ]
    only = set(a.only.split(",")) if a.only else None
    for name, pad, dup, watch in phases:
        if only and name not in only:
            continue
        if watch:
            kh2run("set", a.run, "watch0=exe+0x125d30", "watch_rw=0,1,1", "watch_len=1,4,4", "exec_log=0", "--cmd", "watch")
            time.sleep(1)
        t0 = time.time() + 1.0  # skip the first window (transition)
        procs = []
        if dup:
            procs.append(subprocess.Popen([str(tool), str(ROOT / "runs" / a.run / ("%s.csv" % name)), str(int(a.secs) + 1), "KINGDOM HEARTS"],
                                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        if pad:
            procs.append(subprocess.Popen([PY, "-I", str(ROOT / "scripts" / "kh2run.py"), "pad", a.run, str(ROOT / "tests" / "pad" / pad)],
                                          stdout=subprocess.DEVNULL))
        fgs = []
        for _ in range(int(a.secs * 2)):
            fgs.append(fg_state())
            time.sleep(0.5)
        for p in procs:
            p.wait(timeout=30)
        if watch:
            kh2run("set", a.run, "--cmd", "unwatch")
        time.sleep(1.5)
        s = [x for x in stats_since(log, t0) if True][: int(a.secs) - 1]
        extra = [tot - main for tot, main in s]
        if not extra:
            print("%-12s no STATS windows" % name)
            continue
        fgsum = "/".join("%s:%d" % (k, fgs.count(k)) for k in sorted(set(fgs)))
        print("%-12s windows=%d extra/s median %.0f max %.0f | total median %.0f main median %.0f | window %s" % (
            name, len(extra), statistics.median(extra), max(extra), statistics.median([t for t, _ in s]),
            statistics.median([m for _, m in s]), fgsum))
        time.sleep(1.0)
    kh2run("set", a.run, "stats_ms=5000")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
