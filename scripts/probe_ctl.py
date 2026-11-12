"""Drive the discovery probe's live settings (kh2vr_probe.ini next to the DLL).

  probe_ctl.py <ini> key=value ... [--cmd NAME]     set keys (others kept); --cmd also bumps cmd_seq
  probe_ctl.py <ini> --reset key=value ...          start from an empty file, then set keys

The probe reloads the file every reload_ms and runs `cmd` once whenever cmd_seq changes. Keys missing from
the file fall back to the probe's defaults on every reload.
"""
import sys
from pathlib import Path


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    ini = Path(argv[1])
    args = argv[2:]
    reset = "--reset" in args
    cmd = None
    if "--cmd" in args:
        i = args.index("--cmd")
        cmd = args[i + 1]
        del args[i:i + 2]
    args = [a for a in args if a != "--reset"]
    keys = {}
    if ini.exists() and not reset:
        for line in ini.read_text(encoding="utf-8").splitlines():
            if "=" in line and not line.lstrip().startswith(("#", ";")):
                k, v = line.split("=", 1)
                keys[k.strip()] = v.strip()
    for a in args:
        k, v = a.split("=", 1)
        keys[k.strip()] = v.strip()
    if cmd:
        keys["cmd"] = cmd
        keys["cmd_seq"] = str(int(keys.get("cmd_seq", "0")) + 1)
    tmp = ini.with_suffix(".tmp")
    tmp.write_text("".join("%s=%s\n" % kv for kv in keys.items()), encoding="utf-8")
    tmp.replace(ini)
    print(" ".join("%s=%s" % kv for kv in keys.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
