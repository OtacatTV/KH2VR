"""RenderDoc analysis: every draw recorded inside the bundles, with its per-draw data.

The capture records bundle command lists inline (Reset(type=1 bundle) ... Close, then ExecuteBundle on the
main list), so draws inside bundles are in the structured chunks even though they aren't listed as actions.
For each draw this prints: bundle id, pipeline state, root CBVs in effect (bundle-local ones plus those the
main list set before ExecuteBundle), vertex buffer (buffer, offset, size, stride), index count, and:
  * the first floats behind each root CBV, classified (VIEW / WVP, see camera_math.py);
  * vertex position statistics: the first 3 floats of every vertex taken as a position (min / max / mean),
    plus the raw floats of the first vertex, so world-space and camera-space geometry can be told apart.

Run inside RenderDoc's embedded Python:  qrenderdoc.exe --python scripts\\rdc\\bundle_draws.py
Environment: KH2VR_RDC, KH2VR_OUT, KH2VR_SCRIPTS (folder of camera_math.py), KH2VR_CB_FLOATS (default 48),
KH2VR_BUNDLES (optional comma-separated bundle numbers to report; others are skipped).
"""
import math
import os
import struct
import sys
import traceback

import renderdoc as rd

OUT = open(os.environ["KH2VR_OUT"], "w", encoding="utf-8")
try:
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
    if t in (rd.SDBasic.UnsignedInteger, rd.SDBasic.Enum, rd.SDBasic.Resource):
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


def params(ch):
    return {ch.GetChild(i).name: sd_value(ch.GetChild(i)) for i in range(ch.NumChildren())}


def bufoff(v):
    d = dict(v) if isinstance(v, list) else {}
    return d.get("Buffer"), d.get("Offset")


def describe_matrices(fl):
    kinds = []
    for i in range(0, max(0, len(fl) - 15)):
        w = classify_wvp(fl[i:i + 16])
        if w:
            lay, sc, xs, ys, fwd = w
            kinds.append("WVP-%s@+%d s=%.3f hfov=%.1f" % (lay, i * 4, sc, 2 * math.degrees(math.atan(1 / xs))))
            continue
        c = classify(fl[i:i + 16])
        if c:
            kinds.append("%s@+%d" % (c[0], i * 4))
    return ";".join(kinds[:4]) or "-"


def main():
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["KH2VR_RDC"], "", None)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    sf = ctl.GetStructuredFile()
    nfl = int(os.environ.get("KH2VR_CB_FLOATS", "48"))
    by_int = {int(b.resourceId): b for b in ctl.GetBuffers()}

    # chunk index -> event id, for ExecuteBundle actions (to replay to the right point before reading memory)
    chunk_ev = {}

    def walk(acts):
        for a in acts:
            for e in a.events:
                chunk_ev[e.chunkIndex] = e.eventId
            walk(a.children)

    walk(ctl.GetRootActions())

    lists = {}   # command list id -> state dict
    draws_by_bundle = {}
    pending = []  # (bundle id, draw record)
    main_root = {}
    nb = 0
    for idx, ch in enumerate(sf.chunks):
        name = ch.name
        if not name.startswith("ID3D12GraphicsCommandList::"):
            continue
        p = params(ch)
        cl = p.get("pCommandList", p.get("CommandList"))
        op = name.split("::")[1]
        if op == "Reset":
            lists[cl] = {"bundle": p.get("type") == 1, "root": {}, "pso": None, "vb": None, "ib": None, "draws": []}
            continue
        st = lists.get(cl)
        if st is None:
            continue
        if op == "SetGraphicsRootConstantBufferView":
            st["root"][p["RootParameterIndex"]] = ("CBV",) + bufoff(p["BufferLocation"])
        elif op == "SetGraphicsRootDescriptorTable":
            d = dict(p["BaseDescriptor"])
            st["root"][p["RootParameterIndex"]] = ("TABLE", d.get("heap"), d.get("index"))
        elif op == "SetGraphicsRoot32BitConstants" or op == "SetGraphicsRoot32BitConstant":
            st["root"][p["RootParameterIndex"]] = ("CONST", str(p)[:80])
        elif op == "SetGraphicsRootSignature":
            st["root"] = {}
        elif op == "SetPipelineState":
            st["pso"] = p["pPipelineState"]
        elif op == "IASetVertexBuffers":
            v = dict(p["pViews"][0][1]) if p.get("pViews") else {}
            b, o = bufoff(v.get("BufferLocation", []))
            st["vb"] = (b, o, v.get("SizeInBytes"), v.get("StrideInBytes"))
        elif op == "IASetIndexBuffer":
            v = dict(p["pView"])
            b, o = bufoff(v.get("BufferLocation", []))
            st["ib"] = (b, o, v.get("SizeInBytes"), v.get("Format"))
        elif op in ("DrawIndexedInstanced", "DrawInstanced"):
            st["draws"].append({"chunk": idx, "op": op, "root": dict(st["root"]), "pso": st["pso"], "vb": st["vb"],
                                "ib": st["ib"], "count": p.get("IndexCountPerInstance", p.get("VertexCountPerInstance")),
                                "inst": p.get("InstanceCount")})
        elif op == "ExecuteBundle":
            nb += 1
            b = lists.get(p["pBundle"])
            ev = chunk_ev.get(idx)
            draws_by_bundle[nb] = (p["pBundle"], ev, dict(st["root"]), b["draws"] if b else [])

    cb_cache = {}
    only = set(int(x) for x in os.environ.get("KH2VR_BUNDLES", "").split(",") if x)
    for n, (bid, ev, inherited, draws) in sorted(draws_by_bundle.items()):
        if only and n not in only:
            continue
        if ev is not None:
            ctl.SetFrameEvent(ev, True)
        cb_cache.clear()
        log("BUNDLE#%d id %s event %s draws %d inherited %s" % (n, bid, ev, len(draws),
            " ".join("r%s=%s" % (k, v) for k, v in sorted(inherited.items()))))
        for i, d in enumerate(draws):
            root = dict(inherited)
            root.update(d["root"])
            cbs = []
            for k, v in sorted(root.items()):
                if v[0] != "CBV" or v[1] is None or v[1] not in by_int:
                    continue
                if k == 2 and i > 0 and v == inherited.get(2):
                    continue  # inherited camera block, printed once per bundle
                key = (v[1], v[2])
                if key not in cb_cache:
                    blen = by_int[v[1]].length
                    data = ctl.GetBufferData(by_int[v[1]].resourceId, v[2], min(nfl * 4, blen - v[2]))
                    fl = struct.unpack("<%df" % (len(data) // 4), data[: len(data) // 4 * 4])
                    cb_cache[key] = fl
                fl = cb_cache[key]
                cbs.append("r%d %d+0x%x [%s] {%s}" % (k, v[1], v[2], describe_matrices(fl), " ".join("%.3g" % x for x in fl)))
            vtxt = "-"
            vb = d["vb"]
            if vb and vb[0] in by_int and vb[3]:
                b, o, size, stride = vb
                data = ctl.GetBufferData(by_int[b].resourceId, o, size)
                nv = len(data) // stride
                if nv:
                    mins = [1e30] * 3
                    maxs = [-1e30] * 3
                    sums = [0.0] * 3
                    for j in range(nv):
                        x = struct.unpack_from("<3f", data, j * stride)
                        for c in range(3):
                            mins[c] = min(mins[c], x[c])
                            maxs[c] = max(maxs[c], x[c])
                            sums[c] += x[c]
                    first = struct.unpack_from("<%df" % (stride // 4), data, 0)
                    vtxt = "nv %d stride %d pos min (%s) max (%s) mean (%s) v0 [%s]" % (
                        nv, stride, " ".join("%.3g" % m for m in mins), " ".join("%.3g" % m for m in maxs),
                        " ".join("%.3g" % (s / nv) for s in sums), " ".join("%.4g" % x for x in first))
            log("  draw %d %s pso %s count %s inst %s vb %s" % (i, d["op"], d["pso"], d["count"], d["inst"], vb))
            log("     root:", " ".join("r%s=%s" % (k, v) for k, v in sorted(root.items())))
            log("     verts:", vtxt)
            for c in cbs:
                log("     cb:", c)
    log("\nSUMMARY bundles", len(draws_by_bundle), "draws", sum(len(v[3]) for v in draws_by_bundle.values()))
    ctl.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:  # noqa: BLE001
    log("FATAL", traceback.format_exc())
finally:
    OUT.close()
    os._exit(0)
