"""Live scenario (Frame Rate = Unlocked, at the Back Alley test spot): are forced in-between frames correct
during HUD animation, reaction commands and attacks?

  forced_interp_actions.py <run> [--frames 300] [--only command_menu,reaction_examine,attack_combo]

For each action (tests/pad/<action>.pad) it runs twice: natural (the game decides; at the test spot the
active reaction command keeps interpolation off) and forced (the probe sets the render object's switch
field exe+0x8a6338 to 1 at the render loop's read, exe+0x128af6). During each run it captures a burst and
judges it with scripts/blip_count.py (HUD regions: one-frame dropouts) and scripts/burst_shifts.py (world:
back-and-forth jumps), and reads the extra-frame rate from the probe log (stats_ms=1000).
Verdict per run: PASS / FAIL lines from the checkers; forced runs must also show extra frames (> 100/s),
or they are INCONCLUSIVE.
"""
import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
PY = sys.executable
FORCE = ["watch0=exe+0x128af6", "watch_rw=0,1,1", "watch_len=1,4,4", "exec_log=0", "poke_slot=0",
         "poke_abs=exe+0x8a6338", "poke_int=1", "poke_add=1"]


def kh2run(*a):
    r = subprocess.run([PY, "-I", str(ROOT / "scripts" / "kh2run.py")] + list(a), capture_output=True, text=True)
    return r.returncode, r.stdout.strip()


def extra_rate(log, n):
    vals = []
    for ln in list(open(log, encoding="utf-8", errors="replace"))[-60:]:
        m = re.search(r"STATS window=\d+ms presents=\d+ \(([\d.]+)/s\) on_main_thread=\d+ onframe=\d+ \(([\d.]+)/s\)", ln)
        if m:
            vals.append(float(m.group(1)) - float(m.group(2)))
    return vals[-n:]


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--only")
    a = ap.parse_args(argv)
    d = ROOT / "runs" / a.run
    log = d / "kh2vr_probe.log"
    tool = ROOT / "build" / "tools" / "frame_diff_capture.exe"
    kh2run("set", a.run, "stats_ms=1000")
    actions = (a.only or "command_menu,reaction_examine,attack_combo").split(",")
    for act in actions:
        for mode in ("natural", "forced"):
            if mode == "forced":
                kh2run("set", a.run, *FORCE, "--cmd", "watch")
            else:
                kh2run("set", a.run, "poke_add=0", "poke_slot=-1", "--cmd", "unwatch")
            time.sleep(2.0)
            out = d / ("act_%s_%s" % (act, mode))
            out.mkdir(parents=True, exist_ok=True)
            pad = subprocess.Popen([PY, "-I", str(ROOT / "scripts" / "kh2run.py"), "pad", a.run, str(ROOT / "tests" / "pad" / (act + ".pad"))],
                                   stdout=subprocess.DEVNULL)
            time.sleep(0.25)
            subprocess.run([str(tool), str(out / "f_%03d.bmp"), str(a.frames), "KINGDOM HEARTS", "4"], capture_output=True, text=True, timeout=120)
            pad.wait(timeout=60)
            rates = extra_rate(log, 3)
            if mode == "forced":
                kh2run("set", a.run, "poke_add=0", "poke_slot=-1", "--cmd", "unwatch")
            b = subprocess.run([PY, "-I", str(ROOT / "scripts" / "blip_count.py"), str(out / "f_%03d.bmp"), "--region", "dummy=0,0,0.01,0.01"],
                               capture_output=True, text=True).stdout.strip().splitlines()
            s = subprocess.run([PY, "-I", str(ROOT / "scripts" / "burst_shifts.py"), str(out / "f_%03d.bmp"), "--region", "0.03,0.08,0.38,0.45",
                                "--jump", "60", "--scale", "4"], capture_output=True, text=True).stdout.strip().splitlines()
            hud = [ln for ln in b if ln.startswith(("hud_", "VERDICT"))]
            extra = max(rates) if rates else 0
            tag = "INCONCLUSIVE (no extra frames)" if mode == "forced" and extra < 100 else ""
            print("%-17s %-8s extra/s %s | blips: %s | world: %s %s" % (
                act, mode, ",".join("%.0f" % r for r in rates), " / ".join(h.split("  step")[0] for h in hud),
                (s[-1] if s else "?"), tag))
        time.sleep(1.0)
    kh2run("set", a.run, "stats_ms=5000")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
