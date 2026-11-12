"""Live test session for KH2FM with the discovery probe: one run owns the game install.

  kh2run.py start <run> [k=v ...]       pin check, install probe + ini, launch, wait for the first Present
  kh2run.py set <run> k=v ... [--cmd C] change live probe settings (and run a probe command)
  kh2run.py pad <run> <script.pad>      play a pad script through the probe's test driver; waits for the end
  kh2run.py wait <run> <regex> [secs]   wait for a probe log line (new lines only, from now)
  kh2run.py snap <run> <name>           full-resolution snapshot; refuses if the game window is covered
  kh2run.py state <run> <name> [--min x] snapshot + nearest reference screen (scripts/screen_state.py)
  kh2run.py stop <run>                  close the game and remove the probe; always safe to run
  kh2run.py status                      game running? probe files left in the mod folder?

Verdict lines: OK / FAIL / ERROR, exit 0 / 1 / 2. Run output goes to runs/<run>/ (git-ignored).
Paths: scripts/kh2_local.json (machine-specific, git-ignored; created with defaults on first use).
The pad driver needs drive=1 (start sets it with drive_focus=1, so input reaches the game while it is
not the foreground window). A pad press is a request: check the outcome (screen state, probe log).
"""
import ctypes
import ctypes.wintypes as wt
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PY = sys.executable
LOCAL = ROOT / "scripts" / "kh2_local.json"
DEFAULTS = {
    "game_dir": r"F:\SteamLibrary\steamapps\common\KINGDOM HEARTS -HD 1.5+2.5 ReMIX-",
    "exe": "KINGDOM HEARTS II FINAL MIX.exe",
    "exe_sha256": "9002b2de6a1f91a790bd0673de125d1cf833f7942bfec827cdcf6ba64d5849ed",  # build 15194255, RESEARCH.md
    "mod_dll_dir": r"C:\OpenKH\mod\kh2\dll",  # <OpenKH folder>\mod\kh2\dll: set yours in scripts/kh2_local.json
    "window_title": "KINGDOM HEARTS - HD 1.5+2.5 ReMIX -",
    "start_timeout_s": 120,
}
PROBE_FILES = ("kh2vr_probe.dll", "kh2vr_probe.ini")


def cfg():
    if not LOCAL.exists():
        LOCAL.write_text(json.dumps(DEFAULTS, indent=2), encoding="utf-8")
    c = dict(DEFAULTS)
    c.update(json.loads(LOCAL.read_text(encoding="utf-8")))
    return c


def out(verdict, msg):
    print("%s %s" % (verdict, msg))
    return {"OK": 0, "FAIL": 1}.get(verdict, 2)


def run_dir(run):
    d = ROOT / "runs" / run
    d.mkdir(parents=True, exist_ok=True)
    return d


def game_pids(c):
    r = subprocess.run(["tasklist", "/FO", "CSV", "/NH", "/FI", "IMAGENAME eq %s" % c["exe"]], capture_output=True, text=True)
    pids = []
    for line in r.stdout.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower() == c["exe"].lower():
            pids.append(int(parts[1]))
    return pids


def ini_path(c):
    return Path(c["mod_dll_dir"]) / "kh2vr_probe.ini"


def probe_ctl(c, args):
    r = subprocess.run([PY, "-I", str(ROOT / "scripts" / "probe_ctl.py"), str(ini_path(c))] + args, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)


def ini_get(c, key, default=None):
    p = ini_path(c)
    if p.exists():
        for line in p.read_text(encoding="utf-8").splitlines():
            if line.split("=", 1)[0].strip() == key:
                return line.split("=", 1)[1].strip()
    return default


class LogTail:
    """Reads new lines of the probe log from the current end (or from the start)."""

    def __init__(self, path, from_start=False):
        self.path = Path(path)
        self.pos = 0 if from_start or not self.path.exists() else self.path.stat().st_size

    def wait(self, regex, timeout, alive=None):
        rx = re.compile(regex)
        end = time.time() + timeout
        buf = ""
        while time.time() < end:
            if self.path.exists():
                with open(self.path, "r", encoding="utf-8", errors="replace") as f:
                    f.seek(self.pos)
                    chunk = f.read()
                    self.pos = f.tell()
                buf += chunk
                lines = buf.split("\n")
                buf = lines.pop()
                for ln in lines:
                    if rx.search(ln):
                        return ln
            if alive and not alive():
                return None
            time.sleep(0.2)
        return None


# ---- window checks (ctypes) -------------------------------------------------------------------
user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.FindWindowW.restype = wt.HWND
user32.WindowFromPoint.restype = wt.HWND
user32.GetAncestor.restype = wt.HWND


def game_window(c):
    return user32.FindWindowW(None, c["window_title"])


def window_unobscured(hwnd):
    """True when the client area's centre and four inner corners all belong to the game window."""
    rc = wt.RECT()
    if not user32.GetClientRect(hwnd, ctypes.byref(rc)):
        return False
    pts = []
    w, h = rc.right, rc.bottom
    for fx, fy in ((0.5, 0.5), (0.05, 0.05), (0.95, 0.05), (0.05, 0.95), (0.95, 0.95)):
        p = wt.POINT(int(w * fx), int(h * fy))
        user32.ClientToScreen(hwnd, ctypes.byref(p))
        pts.append(p)
    for p in pts:
        top = user32.WindowFromPoint(p)
        if not top or user32.GetAncestor(top, 2) != hwnd:  # GA_ROOT
            return False
    return not user32.IsIconic(hwnd)


# ---- commands ---------------------------------------------------------------------------------
def cmd_status(c):
    pids = game_pids(c)
    left = [f for f in PROBE_FILES if (Path(c["mod_dll_dir"]) / f).exists()]
    print("game pids: %s" % (pids or "none"))
    print("probe files in mod folder: %s" % (left or "none"))
    return 0


def remove_probe(c):
    for f in PROBE_FILES:
        p = Path(c["mod_dll_dir"]) / f
        if p.exists():
            p.unlink()


def cmd_start(c, run, sets):
    if game_pids(c):
        return out("ERROR", "game already running (another run owns the install); stop it first")
    remove_probe(c)  # leftovers from a killed launcher
    exe = Path(c["game_dir"]) / c["exe"]
    digest = hashlib.sha256(exe.read_bytes()).hexdigest()
    if digest != c["exe_sha256"]:
        return out("ERROR", "exe SHA-256 %s does not match the pinned build: no probe installed" % digest)
    d = run_dir(run)
    dll = ROOT / "build" / "probe" / "kh2vr_probe.dll"
    if not dll.exists():
        return out("ERROR", "build the probe first (scripts\\build_probe.cmd)")
    Path(c["mod_dll_dir"]).mkdir(parents=True, exist_ok=True)
    shutil.copy2(dll, Path(c["mod_dll_dir"]) / dll.name)
    probe_ctl(c, ["--reset", "log_dir=%s" % d, "stats_ms=5000", "pad_log=1", "drive=1", "drive_focus=1"] + sets)
    log = d / "kh2vr_probe.log"
    if log.exists():
        log.rename(d / ("kh2vr_probe.%d.log" % int(time.time())))
    tail = LogTail(log, from_start=True)
    p = subprocess.Popen([str(exe)], cwd=c["game_dir"], creationflags=0x00000008 | 0x00000200)  # DETACHED, NEW_GROUP
    (d / "session.json").write_text(json.dumps({"pid": p.pid, "started": time.time()}), encoding="utf-8")
    line = tail.wait(r"first Present", c["start_timeout_s"], alive=lambda: p.poll() is None)
    if not line:
        return out("FAIL", "no first Present within %ss (game %s); run `stop`" % (c["start_timeout_s"], "exited" if p.poll() is not None else "running"))
    line = LogTail(log, from_start=True).wait(r"DRIVE install", 10)  # logged at the first tick, before the Present
    return out("OK" if line and "patched=0" not in line.split("XInputGetState")[1][:20] else "FAIL",
               "game up, pid %d; %s" % (p.pid, line.split(" ", 1)[1] if line else "no DRIVE install line"))


def cmd_set(c, run, args):
    probe_ctl(c, args)
    return out("OK", "settings written")


def cmd_wait(c, run, regex, secs):
    line = LogTail(run_dir(run) / "kh2vr_probe.log").wait(regex, secs, alive=lambda: bool(game_pids(c)))
    return out("OK", line) if line else out("FAIL", "no match for /%s/ within %ss" % (regex, secs))


def cmd_pad(c, run, script):
    script = Path(script).resolve()
    if not script.exists():
        return out("ERROR", "no script %s" % script)
    total_ms = 0.0
    for ln in script.read_text(encoding="utf-8").splitlines():
        ln = ln.split("#", 1)[0].split()
        if len(ln) >= 2:
            total_ms += float(ln[0])
    seq = int(ini_get(c, "drive_seq", "0")) + 1
    tail = LogTail(run_dir(run) / "kh2vr_probe.log")
    probe_ctl(c, ["drive_script=%s" % script, "drive_seq=%d" % seq])
    started = tail.wait(r"DRIVE (start seq %d|seq %d refused|script)" % (seq, seq), 5)
    if not started or "start seq" not in started:
        return out("FAIL", "driver did not start: %s" % (started or "no log line"))
    done = tail.wait(r"DRIVE released", total_ms / 1000 + 10, alive=lambda: bool(game_pids(c)))
    return out("OK", "played %s (%.1f s)" % (script.name, total_ms / 1000)) if done else out("FAIL", "script did not finish")


def cmd_snap(c, run, name):
    hwnd = game_window(c)
    if not hwnd:
        return out("ERROR", "game window not found")
    if not window_unobscured(hwnd):
        return out("ERROR", "game window is covered or minimised: snapshot would show other windows")
    path = run_dir(run) / (name if name.endswith(".bmp") else name + ".bmp")
    tool = ROOT / "build" / "tools" / "frame_diff_capture.exe"
    r = subprocess.run([str(tool), str(path), "2", c["window_title"]], capture_output=True, text=True, timeout=30)
    if r.returncode or not path.exists():
        return out("ERROR", "capture failed: %s" % (r.stdout + r.stderr).strip()[-200:])
    if not window_unobscured(hwnd):
        path.unlink()
        return out("ERROR", "window got covered during the snapshot; discarded")
    return out("OK", str(path))


def cmd_snapwin(c, run, name, title):
    """Snapshot another window by exact title (e.g. SteamVR's "VR View"); refuses when it's covered."""
    hwnd = user32.FindWindowW(None, title)
    if not hwnd:
        return out("ERROR", "window %r not found" % title)
    if not window_unobscured(hwnd):
        return out("ERROR", "window %r is covered or minimised: snapshot would show other windows" % title)
    path = run_dir(run) / (name if name.endswith(".bmp") else name + ".bmp")
    tool = ROOT / "build" / "tools" / "frame_diff_capture.exe"
    r = subprocess.run([str(tool), str(path), "2", title], capture_output=True, text=True, timeout=30)
    if r.returncode or not path.exists():
        return out("ERROR", "capture failed: %s" % (r.stdout + r.stderr).strip()[-200:])
    if not window_unobscured(hwnd):
        path.unlink()
        return out("ERROR", "window got covered during the snapshot; discarded")
    return out("OK", str(path))


def cmd_state(c, run, name, extra=()):
    rc = cmd_snap(c, run, name)
    if rc:
        return rc
    path = run_dir(run) / (name + ".bmp")
    r = subprocess.run([PY, "-I", str(ROOT / "scripts" / "screen_state.py"), "match", str(path)] + list(extra), capture_output=True, text=True)
    print(r.stdout.strip())
    return r.returncode


def cmd_stop(c, run):
    pids = game_pids(c)
    for pid in pids:
        subprocess.run(["taskkill", "/PID", str(pid)], capture_output=True)
    end = time.time() + 10
    while game_pids(c) and time.time() < end:
        time.sleep(0.5)
    for pid in game_pids(c):
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
    time.sleep(1)
    remove_probe(c)
    left = [f for f in PROBE_FILES if (Path(c["mod_dll_dir"]) / f).exists()]
    still = game_pids(c)
    if left or still:
        return out("FAIL", "cleanup incomplete: pids %s, files %s" % (still, left))
    return out("OK", "game closed (%d process(es)), probe removed" % len(pids))


def main(a):
    if len(a) < 2 or a[1] in ("-h", "--help"):
        print(__doc__)
        return 2
    c = cfg()
    op = a[1]
    try:
        if op == "status":
            return cmd_status(c)
        run = a[2]
        if op == "start":
            return cmd_start(c, run, a[3:])
        if op == "set":
            return cmd_set(c, run, a[3:])
        if op == "pad":
            return cmd_pad(c, run, a[3])
        if op == "wait":
            return cmd_wait(c, run, a[3], float(a[4]) if len(a) > 4 else 30)
        if op == "snap":
            return cmd_snap(c, run, a[3])
        if op == "state":
            return cmd_state(c, run, a[3], a[4:])
        if op == "snapwin":
            return cmd_snapwin(c, run, a[3], a[4])
        if op == "stop":
            return cmd_stop(c, run)
    except Exception as e:  # noqa: BLE001
        return out("ERROR", "%s: %s" % (type(e).__name__, e))
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
