"""RenderDoc analysis: find camera matrices in every buffer of a capture.

Looks (4-byte steps) for 4x4 float matrices that are
  * VIEW: orthonormal 3x3 with a translation row/column (as the CPU scans did), or
  * VIEWPROJ (perspective, D3D): one column (row-vector) or row (column-vector) is a unit vector (camera
    forward), two others are orthogonal to it and to each other with norms xs, ys (lens scale) -> hfov, vfov.
Reports buffer id, offset, kind, lens values and the forward vector, grouped so copies collapse.
Also lists CopyResource source/destination pairs (per-frame constant uploads).

Run inside RenderDoc's embedded Python:  qrenderdoc.exe --python scripts\\rdc\\find_cameras.py
Environment: KH2VR_RDC (capture), KH2VR_OUT (report file), KH2VR_MAXBUF (max buffer bytes, default 64 MiB).
"""
import math
import os
import struct
import traceback

import sys

import renderdoc as rd


OUT = open(os.environ["KH2VR_OUT"], "w", encoding="utf-8")
try:
    # qrenderdoc's embedded Python may not define __file__: take the folder from KH2VR_SCRIPTS if set.
    sys.path.insert(0, os.environ.get("KH2VR_SCRIPTS") or os.path.dirname(os.path.abspath(globals().get("__file__", "."))))
    from camera_math import classify  # noqa: E402
except Exception:  # noqa: BLE001
    OUT.write("FATAL import " + traceback.format_exc())
    OUT.close()
    os._exit(1)


def log(*a):
    OUT.write(" ".join(str(x) for x in a) + "\n")
    OUT.flush()


def main():
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["KH2VR_RDC"], "", None)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    sf = ctl.GetStructuredFile()
    maxbuf = int(os.environ.get("KH2VR_MAXBUF", str(64 << 20)))

    acts = []

    def walk(a):
        for x in a:
            acts.append(x)
            walk(x.children)

    walk(ctl.GetRootActions())
    # CopyResource pairs, read from the structured chunk parameters.
    for a in acts:
        name = a.GetName(sf)
        if "CopyResource" in name or "CopyBufferRegion" in name:
            log("COPY event", a.eventId, name, "copySource", a.copySource, "copyDest", a.copyDestination)
    last = acts[-1].eventId
    ctl.SetFrameEvent(last, True)

    bufs = ctl.GetBuffers()
    log("buffers", len(bufs))
    found = {}
    for b in bufs:
        if b.length < 64 or b.length > maxbuf:
            continue
        try:
            data = ctl.GetBufferData(b.resourceId, 0, b.length)
        except Exception as e:  # noqa: BLE001
            log("read fail", b.resourceId, e)
            continue
        n = len(data) // 4
        if n < 16:
            continue
        fl = struct.unpack("<%df" % n, data[: n * 4])
        for i in range(0, n - 15):
            m = fl[i:i + 16]
            r = classify(m)
            if not r:
                continue
            kind, extra = r
            if extra:
                xs, ys, fwd = extra
                key = (kind, round(xs, 3), round(ys, 3), tuple(round(v, 3) for v in fwd))
                desc = "xs=%.4f ys=%.4f hfov=%.2f vfov=%.2f aspect=%.4f fwd=(%.3f %.3f %.3f)" % (
                    xs, ys, 2 * math.degrees(math.atan(1 / xs)), 2 * math.degrees(math.atan(1 / ys)), xs / ys if ys else 0, *fwd)
            else:
                key = (kind, tuple(round(v, 3) for v in m[:12]))
                desc = "r0=(%.3f %.3f %.3f) r1=(%.3f %.3f %.3f) r2=(%.3f %.3f %.3f) t=(%.2f %.2f %.2f)" % (
                    m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14])
            found.setdefault(key, {"kind": kind, "desc": desc, "where": []})["where"].append("%s+0x%x" % (b.resourceId, i * 4))
    groups = sorted(found.values(), key=lambda g: (g["kind"], -len(g["where"])))
    log("matrix groups", len(groups))
    for g in groups:
        if g["kind"] == "VIEW" and len(g["where"]) < 1:
            continue
        log(g["kind"], "copies=%d" % len(g["where"]), g["desc"], "at", " ".join(g["where"][:6]))
    ctl.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:  # noqa: BLE001
    log("FATAL", traceback.format_exc())
finally:
    OUT.close()
    os._exit(0)
