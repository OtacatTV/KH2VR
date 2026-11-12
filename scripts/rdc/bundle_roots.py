"""RenderDoc analysis: root parameters set before each ExecuteBundle, and the camera data they point at.

D3D12 bundles inherit root arguments from the calling command list, so per-batch data (camera, transforms)
is set with SetGraphicsRoot* just before ExecuteBundle. For each bundle call this prints the root
parameters in effect (CBV GPU addresses resolved to buffer+offset, root constants inline) and classifies
the first 256 bytes behind each CBV (VIEW / VIEWPROJ matrices, see find_cameras.py).

Run inside RenderDoc's embedded Python:  qrenderdoc.exe --python scripts\\rdc\\bundle_roots.py
Environment: KH2VR_RDC (capture), KH2VR_OUT (report file), KH2VR_FIRST_STRUCT=1 to also dump the first
SetGraphicsRoot* chunk's full parameter tree (to learn its field names).
"""
import math
import os
import struct
import sys
import traceback

import renderdoc as rd


OUT = open(os.environ["KH2VR_OUT"], "w", encoding="utf-8")
try:
    # qrenderdoc's embedded Python may not define __file__: take the folder from KH2VR_SCRIPTS if set.
    sys.path.insert(0, os.environ.get("KH2VR_SCRIPTS") or os.path.dirname(os.path.abspath(globals().get("__file__", "."))))
    from camera_math import classify, classify_wvp  # noqa: E402
except Exception:  # noqa: BLE001
    OUT.write("FATAL import " + traceback.format_exc())
    OUT.close()
    os._exit(1)


def log(*a):
    OUT.write(" ".join(str(x) for x in a) + "\n")
    OUT.flush()


def sd_value(o):
    t = o.type.basetype
    if t == rd.SDBasic.UnsignedInteger or t == rd.SDBasic.Enum or t == rd.SDBasic.Resource:
        return o.data.basic.u
    if t == rd.SDBasic.SignedInteger:
        return o.data.basic.i
    if t == rd.SDBasic.Float:
        return o.data.basic.d
    if t == rd.SDBasic.String:
        return o.data.str
    if t == rd.SDBasic.Boolean:
        return o.data.basic.b
    if t in (rd.SDBasic.Array, rd.SDBasic.Struct):
        return [(o.GetChild(i).name, sd_value(o.GetChild(i))) for i in range(o.NumChildren())]
    return None


def chunk_params(ch):
    return {ch.GetChild(i).name: sd_value(ch.GetChild(i)) for i in range(ch.NumChildren())}


def main():
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["KH2VR_RDC"], "", None)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    sf = ctl.GetStructuredFile()
    by_int = {int(b.resourceId): b for b in ctl.GetBuffers()}
    bufs = sorted(((b.gpuAddress, b.length, b.resourceId) for b in ctl.GetBuffers() if b.gpuAddress), key=lambda t: t[0])
    log("buffers with GPU address:", len(bufs), "largest:", max((b[1] for b in bufs), default=0))

    def resolve(va):
        lo, hi = 0, len(bufs) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            base, length, rid = bufs[mid]
            if va < base:
                hi = mid - 1
            elif va >= base + length:
                lo = mid + 1
            else:
                return rid, va - base, length
        return None

    acts = []

    def walk(a):
        for x in a:
            acts.append(x)
            walk(x.children)

    walk(ctl.GetRootActions())
    dumped = os.environ.get("KH2VR_FIRST_STRUCT") != "1"
    state = {}  # root index -> description
    nbundle = 0
    cam_hits = {}
    for a in acts:
        for ev in a.events:
            ch = sf.chunks[ev.chunkIndex]
            name = ch.name
            if "SetGraphicsRoot" in name or "SetGraphicsRootSignature" in name:
                p = chunk_params(ch)
                if not dumped:
                    log("STRUCT", name, p)
                    dumped = True
                if "Signature" in name:
                    state = {}
                    continue
                idx = p.get("RootParameterIndex")
                if "ConstantBufferView" in name or "ShaderResourceView" in name or "UnorderedAccessView" in name:
                    va = p.get("BufferLocation")
                    if isinstance(va, list):  # RenderDoc serialises GPU VAs as (Buffer id, Offset)
                        d = dict(va)
                        va = ("BUFOFF", d.get("Buffer"), d.get("Offset"))
                    state[idx] = ("CBV" if "Constant" in name else "SRV/UAV", va)
                elif "32BitConstant" in name:
                    vals = p.get("pSrcData") or p.get("SrcData")
                    state[idx] = ("CONST", vals)
                elif "DescriptorTable" in name:
                    state[idx] = ("TABLE", p.get("BaseDescriptor"))
        if "ExecuteBundle" not in a.GetName(sf):
            continue
        nbundle += 1
        line = []
        for idx in sorted(k for k in state if k is not None):
            kind, v = state[idx]
            if kind in ("CBV", "SRV/UAV") and (isinstance(v, int) or (isinstance(v, tuple) and v[0] == "BUFOFF")):
                if isinstance(v, tuple):
                    b = by_int.get(v[1])
                    r = (b.resourceId, v[2], b.length) if b else None
                else:
                    r = resolve(v)
                if r:
                    rid, off, length = r
                    data = ctl.GetBufferData(rid, off, min(512, length - off))
                    n = len(data) // 4
                    fl = struct.unpack("<%df" % n, data[: n * 4])
                    kinds = []
                    for i in range(0, max(0, n - 15)):
                        w = classify_wvp(fl[i:i + 16])
                        if w:
                            lay, sc, xs, ys, fwd = w
                            kinds.append("WVP-%s@+%d scale=%.3f hfov=%.2f vfov=%.2f fwd=(%.3f %.3f %.3f)" % (
                                lay, i * 4, sc, 2 * math.degrees(math.atan(1 / xs)), 2 * math.degrees(math.atan(1 / ys)), *fwd))
                            continue
                        c = classify(fl[i:i + 16])
                        if c:
                            if c[1]:
                                xs, ys, fwd = c[1]
                                kinds.append("%s@+%d hfov=%.2f vfov=%.2f fwd=(%.3f %.3f %.3f)" % (
                                    c[0], i * 4, 2 * math.degrees(math.atan(1 / xs)), 2 * math.degrees(math.atan(1 / ys)), *fwd))
                            else:
                                kinds.append("%s@+%d" % (c[0], i * 4))
                    key = (idx, str(rid))
                    cam_hits.setdefault(key, set()).update(k.split("@")[0] for k in kinds)
                    line.append("r%d %s %s+0x%x %s first32=[%s]" % (idx, kind, rid, off, ";".join(kinds[:3]) or "-",
                                                                     " ".join("%.3f" % x for x in fl[:32])))
                else:
                    line.append("r%d %s va=0x%x (no buffer)" % (idx, kind, v))
            elif kind in ("CBV", "SRV/UAV"):
                line.append("r%d %s unresolved %s" % (idx, kind, str(v)[:80]))
            else:
                line.append("r%d %s %s" % (idx, kind, str(v)[:120]))
        log("BUNDLE#%d event %d" % (nbundle, a.eventId))
        for l in line:
            log("   ", l)
    log("\nSUMMARY bundles", nbundle)
    for (idx, rid), kinds in sorted(cam_hits.items()):
        log("  root %s buffer %s matrix kinds seen: %s" % (idx, rid, ",".join(sorted(kinds)) or "-"))
    ctl.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:  # noqa: BLE001
    log("FATAL", traceback.format_exc())
finally:
    OUT.close()
    os._exit(0)
