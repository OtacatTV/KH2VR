"""Scripted menu flows for live tests, keyed off recognised screens (scripts/screen_state.py), not timers.

  kh2_flow.py <flow> <run>                run a flow from FLOWS: load_spot (slot 2, Back Alley test spot),
                                          load_combat (slot 3, Land of Dragons encampment near enemies)
  kh2_flow.py wait-state <run> <state> [secs]

Each step: wait until the screen matches `want` (polling snapshots), then play `pad`. `slot: N` selects a
save file closed-loop on the load list (the cursor starts on the most recently used file, so a fixed
number of presses isn't enough): find the pointer hand (template runs/refs/screens/load_hand.png), press
UP/DOWN, re-check, then A. A step whose
screen never appears fails the flow (FAIL, exit 1) and names the last state seen. Snapshots go to
runs/<run>/flow_*.bmp. The game must already be running (kh2run.py start).
"""
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PY = sys.executable
FLOWS = {
    "load": [
        {"want": "title_menu", "timeout": 120, "pad": "tests/pad/title_to_load.pad"},
        {"want": "load_menu", "timeout": 20, "slot": 2, "pad": None, "retry": {"after": 8, "times": 2}},
        {"want": "save_room", "timeout": 60, "pad": None},
    ],
    # Slot 3 (made by the human 2026-11-10: Bamboo Grove save point, Land of Dragons) -> walk into the
    # encampment and stop just before the enemies (recorded walk, split at the area change).
    "load_combat": [
        {"want": "title_menu", "timeout": 120, "pad": "tests/pad/title_to_load.pad"},
        {"want": "load_menu", "timeout": 20, "slot": 3, "pad": None, "retry": {"after": 8, "times": 2}},
        {"want": "bamboo_grove", "timeout": 60, "delay": 2.0, "pad": "tests/pad/combat_walk1_grove_to_exit.pad"},
        # The area change into the encampment doesn't fade fully to black (missed by __transition,
        # 2026-11-10) and the Heartless close in by themselves, so: wait for the encampment, no second walk.
        # Minimap reference; the map scrolls as Sora moves and the fight starts, hence the lower minimum.
        {"want": "combat_spot", "timeout": 30, "min": 0.75, "pad": None},
    ],
    # Save room (The Usual Spot) -> Back Alley test spot (the human's recorded walk, split at the area
# transition so the second half starts after the new area has loaded, whatever the load time).
    "spot": [
        {"want": "save_room", "timeout": 10, "pad": None},
        # The save point's glow keeps the room picture changing, so no settle check here: a fixed delay.
        {"want": "save_room", "timeout": 10, "delay": 2.0, "pad": "tests/pad/walk1_save_room_to_exit.pad"},
        {"want": "__transition", "timeout": 30, "pad": "tests/pad/walk2_alley_to_spot.pad"},
        {"want": "alley_spot", "timeout": 10, "pad": None},
    ],
}
FLOWS["load_spot"] = FLOWS["load"] + FLOWS["spot"][1:]


def snap_thumb(run, tag):
    """Small grayscale thumbnail of a fresh snapshot, or None (covered window etc.)."""
    import numpy as np
    from PIL import Image
    r = subprocess.run([PY, "-I", str(ROOT / "scripts" / "kh2run.py"), "snap", run, tag], capture_output=True, text=True)
    if r.returncode:
        return None
    p = ROOT / "runs" / run / (tag + ".bmp")
    return np.asarray(Image.open(p).convert("L").resize((160, 90)), dtype=np.float32)


def corr(a, b):
    import numpy as np
    a = a - a.mean()
    b = b - b.mean()
    d = float(np.linalg.norm(a) * np.linalg.norm(b))
    return float((a * b).sum()) / d if d > 1e-6 else 0.0


def wait_settled(run, timeout, k, need_fade):
    """need_fade: first see a dark frame (area transition), then two similar bright frames in a row
    (the picture has stopped changing much: the new area is in and the camera has come to rest)."""
    end = time.time() + timeout
    faded = not need_fade
    prev = None
    n = 0
    while time.time() < end:
        t = snap_thumb(run, "flow_settle_%02d_%d" % (k, n % 3))
        n += 1
        if t is None:
            time.sleep(0.3)
            continue
        bright = float(t.mean())
        if not faded:
            faded = bright < 12
        elif bright > 30 and prev is not None and corr(prev, t) > 0.97:
            return True, "settled (brightness %.0f)" % bright
        prev = t if faded and bright > 30 else None
        time.sleep(0.3)
    return False, "faded=%s" % faded


def state_of(run, tag, minscore=None):
    extra = ["--min", str(minscore)] if minscore else []
    r = subprocess.run([PY, "-I", str(ROOT / "scripts" / "kh2run.py"), "state", run, tag] + extra, capture_output=True, text=True)
    lines = [ln for ln in r.stdout.splitlines() if ln.startswith("STATE ")]
    if lines:
        return (lines[0].split()[1] if r.returncode == 0 else None), lines[0]
    return None, (r.stdout.strip() or r.stderr.strip())[:160]


def wait_state(run, want, timeout, k, minscore=None):
    end = time.time() + timeout
    last = ""
    n = 0
    while time.time() < end:
        st, line = state_of(run, "flow_%s_%02d_%03d" % (want, k, n % 5), minscore)  # keep the last 5 per step
        n += 1
        if st == want:
            return True, line
        last = line
        time.sleep(0.5)
    return False, last


# Positions measured on a 1440-pixel-high frame; cursor_row scales everything (and the frame) to that height,
# so the finder works at any window size (the game window went from 2560x1440 to 3840x2160, 2026-11-10).
LOAD_ROW0_Y = 368      # y of the hand's centre on row 1 (rows 208 px apart; row 4 measured at 992)
LOAD_ROW_DY = 208
HAND_X = 330           # x of the template's left edge (the hand stays in one column)
REF_H = 1440


def cursor_row(run, tag):
    """Row (1-based) the load list's pointer hand is on, and the match score, or (None, msg)."""
    import numpy as np
    from PIL import Image
    r = subprocess.run([PY, "-I", str(ROOT / "scripts" / "kh2run.py"), "snap", run, tag], capture_output=True, text=True)
    if r.returncode:
        return None, r.stdout.strip()
    im = Image.open(ROOT / "runs" / run / (tag + ".bmp")).convert("L")
    if im.height != REF_H:
        im = im.resize((round(im.width * REF_H / im.height), REF_H), Image.BILINEAR)
    img = np.asarray(im, dtype=np.float32)
    t = np.asarray(Image.open(ROOT / "runs" / "refs" / "screens" / "load_hand.png").convert("L"), dtype=np.float32)
    th, tw = t.shape
    t = (t - t.mean()) / (t.std() + 1e-6)
    best = (-2.0, 0)
    for x in range(HAND_X - 40, HAND_X + 41, 4):  # the pointer bobs sideways (missed with +-8, 2026-11-10)
        for y in range(200, img.shape[0] - th - 100, 4):
            w = img[y:y + th, x:x + tw]
            sd = w.std()
            if sd < 1e-3:
                continue
            c = float(((w - w.mean()) / sd * t).mean())
            if c > best[0]:
                best = (c, y + th // 2)
    score, yc = best
    if score < 0.6:
        return None, "hand not found (best %.2f)" % score
    return int(round((yc - LOAD_ROW0_Y) / LOAD_ROW_DY)) + 1, "row at y=%d score %.2f" % (yc, score)


def select_slot(run, want, k):
    for attempt in range(6):
        row, msg = cursor_row(run, "flow_slot_%02d_%d" % (k, attempt))
        if row is None:
            time.sleep(0.5)
            continue
        if row == want:
            ok, m = pad_text(run, "300 -\n150 A\n300 -\n", "select_a")
            return ok, "cursor on %d (%s), pressed A" % (row, msg)
        key = "UP" if row > want else "DOWN"
        steps = "".join("150 %s\n350 -\n" % key for _ in range(abs(row - want)))
        pad_text(run, "200 -\n" + steps, "select_move")
        time.sleep(0.4)
    return False, "could not put the cursor on file %d (last: %s)" % (want, msg)


def pad_text(run, text, name):
    p = ROOT / "runs" / run / ("gen_%s.pad" % name)
    p.write_text(text, encoding="utf-8")
    r = subprocess.run([PY, "-I", str(ROOT / "scripts" / "kh2run.py"), "pad", run, str(p)], capture_output=True, text=True)
    return r.returncode == 0, r.stdout.strip()


def pad(run, script):
    r = subprocess.run([PY, "-I", str(ROOT / "scripts" / "kh2run.py"), "pad", run, str(ROOT / script)], capture_output=True, text=True)
    return r.returncode == 0, r.stdout.strip()


def main(a):
    if len(a) < 3:
        print(__doc__)
        return 2
    op, run = a[1], a[2]
    if op == "wait-state":
        ok, line = wait_state(run, a[3], float(a[4]) if len(a) > 4 else 30, 0)
        print(("OK " if ok else "FAIL ") + line)
        return 0 if ok else 1
    if op not in FLOWS:
        print(__doc__)
        return 2
    t0 = time.time()
    for k, step in enumerate(FLOWS[op]):
        if step["want"] in ("__settled", "__transition"):
            ok, line = wait_settled(run, step["timeout"], k, step["want"] == "__transition")
        elif step.get("retry") and k > 0 and FLOWS[op][k - 1].get("pad"):
            # A menu press can be missed (seen 2026-11-10 at ~48 fps with the debug layer): if the wanted screen
            # hasn't appeared after `after` seconds, replay the previous step's pad (up to `times` times).
            rt = step["retry"]
            ok, line = wait_state(run, step["want"], rt["after"], k, step.get("min"))
            for attempt in range(rt["times"]):
                if ok:
                    break
                prev = FLOWS[op][k - 1]
                st, _ = state_of(run, "flow_retry_%02d_%d" % (k, attempt), prev.get("min"))
                if st != prev["want"]:
                    break  # not back on the previous screen: replaying its presses could do something else
                okp, msg = pad(run, prev["pad"])
                print("   retry %d: still on %s, replayed %s: %s" % (attempt + 1, prev["want"], prev["pad"], msg))
                ok, line = wait_state(run, step["want"], rt["after"], k, step.get("min"))
        else:
            ok, line = wait_state(run, step["want"], step["timeout"], k, step.get("min"))
        print("step %d want %s: %s (%.0f s)" % (k, step["want"], "seen" if ok else "NOT seen, last: " + line, time.time() - t0))
        if not ok:
            print("FAIL flow %s stopped at step %d" % (op, k))
            return 1
        if step.get("delay"):
            time.sleep(step["delay"])
        if step.get("slot"):
            time.sleep(0.8)  # list fully drawn
            ok, msg = select_slot(run, step["slot"], k)
            print("   slot %d: %s" % (step["slot"], msg))
            if not ok:
                print("FAIL slot selection")
                return 1
        if step["pad"]:
            ok, msg = pad(run, step["pad"])
            print("   pad %s: %s" % (step["pad"], msg))
            if not ok:
                print("FAIL pad step")
                return 1
    print("OK flow %s done in %.0f s" % (op, time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
