"""RenderDoc analysis: print the parameter tree of the first N chunks of each name (to learn field names).

Run inside RenderDoc's embedded Python. Environment: KH2VR_RDC, KH2VR_OUT, KH2VR_PER_NAME (default 2),
KH2VR_NAMES (optional comma-separated substrings to restrict to).
"""
import os
import traceback

import renderdoc as rd

OUT = open(os.environ["KH2VR_OUT"], "w", encoding="utf-8")


def log(*a):
    OUT.write(" ".join(str(x) for x in a) + "\n")
    OUT.flush()


def sd(o, depth=0, maxc=24):
    t = o.type.basetype
    if t in (rd.SDBasic.Array, rd.SDBasic.Struct):
        n = o.NumChildren()
        kids = [sd(o.GetChild(i), depth + 1) for i in range(min(n, maxc))]
        return "{" + ", ".join("%s=%s" % (o.GetChild(i).name, kids[i]) for i in range(len(kids))) + (" ...%d" % n if n > maxc else "") + "}"
    if t in (rd.SDBasic.UnsignedInteger, rd.SDBasic.Enum, rd.SDBasic.Resource):
        return str(o.data.basic.u)
    if t == rd.SDBasic.SignedInteger:
        return str(o.data.basic.i)
    if t == rd.SDBasic.Float:
        return "%.5g" % o.data.basic.d
    if t == rd.SDBasic.String:
        return repr(o.data.str[:60])
    if t == rd.SDBasic.Boolean:
        return str(o.data.basic.b)
    if t == rd.SDBasic.Buffer:
        return "<buffer %d>" % o.data.basic.u
    return "?"


try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["KH2VR_RDC"], "", None)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    sf = ctl.GetStructuredFile()
    per = int(os.environ.get("KH2VR_PER_NAME", "2"))
    only = [s for s in os.environ.get("KH2VR_NAMES", "").split(",") if s]
    seen = {}
    for idx, c in enumerate(sf.chunks):
        if only and not any(s in c.name for s in only):
            continue
        k = seen.get(c.name, 0)
        if k >= per:
            continue
        seen[c.name] = k + 1
        log("#%d %s {%s}" % (idx, c.name, ", ".join("%s=%s" % (c.GetChild(i).name, sd(c.GetChild(i))) for i in range(c.NumChildren()))))
    ctl.Shutdown()
    cap.Shutdown()
except Exception:  # noqa: BLE001
    log("FATAL", traceback.format_exc())
finally:
    OUT.close()
    os._exit(0)
