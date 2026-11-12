"""Live scenario: does an off-axis term in the CPU projection move every draw path the same way?

  projection_shift.py <run> [--addr exe+0xac18e4] [--offset 0.1] [--mode poke|freeze] [--watch exe+0xac1900]

The applied projection (row-vector, 102.3 deg) sits at exe+0xac18c4 on build 15194255 (found 2026-11-10);
float 8 (row 2, x: +0x20 -> exe+0xac18e4) multiplies view-space z into clip x, so adding `offset` there
shifts the whole image by offset * width/2 pixels at every depth: the per-eye asymmetric frustum term.
mode poke (default): the probe adds the offset each time the game writes the projection's last float
(--watch, row 3 w), i.e. right after the per-tick rebuild (fn exe+0x19c820 path) and before its readers.
mode exec: an execute breakpoint at --func (default fn exe+0x125d30, the per-pass camera setup, whose rdx
points at the GPU projection) sets the float at rdx + --reg-offset (32 = row 2 x) to --offset on every call.
Only exec mode reaches the GPU (2026-11-10); poke and freeze edit CPU copies the picture doesn't use.
mode freeze: the probe's once-per-tick freeze (overwritten by the rebuild: no effect, 2026-11-10). Snapshot before / frozen / after,
measure each region's shift.
Verdict: PASS when every region moves within --tol of the prediction and the after-pair is back at rest;
FAIL otherwise (names the regions that didn't follow); ERROR when a snapshot fails.
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

REGIONS = {  # test spot (runs/2026-11-10-auto4/spot1.bmp)
    "sora": (0.44, 0.42, 0.56, 0.75),
    "donald": (0.30, 0.62, 0.43, 0.92),
    "world": (0.03, 0.08, 0.38, 0.45),
    "far": (0.45, 0.10, 0.75, 0.35),
}
WIDTH = 2560


def parse(addr):
    return int(addr.split("+", 1)[1], 0) if addr.startswith("exe+") else int(addr, 0)


def kh2run(*a):
    r = subprocess.run([PY, "-I", str(ROOT / "scripts" / "kh2run.py")] + list(a), capture_output=True, text=True)
    return r.returncode, r.stdout.strip()


def shift(a, b, region):
    sa, _ = load_gray(a, 1.0, region)
    sb, _ = load_gray(b, 1.0, region)
    return phase_corr(sa, sb)[0]


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--addr", default="exe+0xac18e4")
    ap.add_argument("--offset", type=float, default=0.1)
    ap.add_argument("--tol", type=float, default=0.15)
    ap.add_argument("--mode", choices=("exec", "poke", "freeze"), default="exec")
    ap.add_argument("--func", default="exe+0x125d30")
    ap.add_argument("--reg", type=int, default=1)
    ap.add_argument("--reg-offset", type=int, default=32)
    ap.add_argument("--watch", default="exe+0xac1900")
    a = ap.parse_args(argv)
    d = ROOT / "runs" / a.run
    kh2run("set", a.run, "addr=%s" % a.addr, "--cmd", "dump")
    snaps = []
    for tag in ("before", "frozen", "after"):
        if tag == "frozen" and a.mode == "freeze":
            kh2run("set", a.run, "addr=%s" % a.addr, "count=1", "add=%g,0,0" % a.offset, "--cmd", "freeze")
        elif tag == "frozen" and a.mode == "exec":
            kh2run("set", a.run, "watch0=%s" % a.func, "watch_rw=0,1,1", "watch_len=1,4,4", "watch1=", "watch2=", "mat_slot=-1",
                   "exec_log=0", "poke_slot=0", "poke_reg=%d" % a.reg, "poke_offset=%d" % a.reg_offset, "poke_set=1",
                   "poke_add=%g" % a.offset, "--cmd", "watch")
        elif tag == "frozen":
            off = parse(a.addr) - parse(a.watch)
            kh2run("set", a.run, "watch0=%s" % a.watch, "watch_rw=1,1,1", "watch_len=4,4,4", "mat_slot=-1", "poke_slot=0",
                   "poke_offset=%d" % off, "poke_add=%g" % a.offset, "--cmd", "watch")
        elif tag == "after" and a.mode == "freeze":
            kh2run("set", a.run, "--cmd", "unfreeze")
        elif tag == "after":
            kh2run("set", a.run, "poke_add=0", "poke_set=0", "--cmd", "watch_report")
            time.sleep(0.8)
            kh2run("set", a.run, "poke_slot=-1", "--cmd", "unwatch")
        time.sleep(1.5)
        rc, msg = kh2run("snap", a.run, "proj_%s" % tag)
        if rc:
            print("ERROR snapshot %s: %s" % (tag, msg))
            kh2run("set", a.run, "--cmd", "unfreeze")
            return 2
        snaps.append(d / ("proj_%s.bmp" % tag))
    pred = a.offset * WIDTH / 2
    bad = []
    for rn, reg in REGIONS.items():
        on = shift(snaps[0], snaps[1], reg)
        rest = shift(snaps[0], snaps[2], reg)
        ok = abs(abs(on) - pred) <= a.tol * pred and abs(rest) < 6
        print("%-7s frozen %+7.1f px (predicted %.0f)  after %+5.1f px  %s" % (rn, on, pred, rest, "ok" if ok else "MISMATCH"))
        if not ok:
            bad.append(rn)
    print("VERDICT %s%s" % ("PASS" if not bad else "FAIL", "" if not bad else " regions: " + ",".join(bad)))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
