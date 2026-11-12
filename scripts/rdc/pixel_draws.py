"""RenderDoc analysis: which draws produced given screen pixels, and what camera data they were given.

Run inside RenderDoc's embedded Python (qrenderdoc ships the `renderdoc` module):
    qrenderdoc.exe --python scripts\\rdc\\pixel_draws.py
Arguments are passed through environment variables (qrenderdoc's --python takes no script args):
    KH2VR_RDC     capture file (.rdc)
    KH2VR_OUT     output text file
    KH2VR_PIXELS  "name:fx,fy;name:fx,fy" pixel positions as fractions of the frame
Writes a text report; nothing from the capture is copied except numbers it prints (constant-block floats,
event IDs, call-stack addresses). Keep the report in the git-ignored runs/ folder.
"""
import os
import struct
import traceback

import renderdoc as rd

OUT = open(os.environ["KH2VR_OUT"], "w", encoding="utf-8")


def log(*a):
    OUT.write(" ".join(str(x) for x in a) + "\n")
    OUT.flush()


def walk(actions, out):
    for a in actions:
        out.append(a)
        walk(a.children, out)


def floats(data, n=64):
    k = min(len(data) // 4, n)
    return struct.unpack("<%df" % k, data[: k * 4])


def fmt_floats(fs):
    rows = []
    for i in range(0, len(fs), 4):
        rows.append("[" + " ".join("%9.3f" % v for v in fs[i:i + 4]) + "]")
    return " ".join(rows)


def main():
    cap = rd.OpenCaptureFile()
    res = cap.OpenFile(os.environ["KH2VR_RDC"], "", None)
    if res != rd.ResultCode.Succeeded:
        log("open file failed", res)
        return
    has_cs = cap.HasCallstacks()
    resolver_ok = False
    if has_cs:
        try:
            resolver_ok = cap.InitResolver(False, None)
        except Exception as e:  # noqa: BLE001
            log("resolver init failed:", e)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    if res != rd.ResultCode.Succeeded:
        log("open capture failed", res)
        return
    acts = []
    walk(ctl.GetRootActions(), acts)
    draws = [a for a in acts if a.flags & rd.ActionFlags.Drawcall]
    log("actions", len(acts), "draws", len(draws), "callstacks", has_cs, "resolver", resolver_ok)

    # Main colour target = the output written by the most draws.
    counts = {}
    for a in draws:
        for o in a.outputs:
            if o != rd.ResourceId.Null():
                counts[o] = counts.get(o, 0) + 1
    texs = {t.resourceId: t for t in ctl.GetTextures()}
    ranked = sorted(counts.items(), key=lambda kv: -kv[1])[:6]
    for rid, n in ranked:
        t = texs.get(rid)
        log("target", rid, "draws", n, "size", (t.width, t.height) if t else "?", "fmt", t.format.Name() if t else "?")
    target, _ = ranked[0]
    tex = texs[target]

    by_eid = {a.eventId: a for a in acts}
    pixels = []
    for part in os.environ["KH2VR_PIXELS"].split(";"):
        name, xy = part.split(":")
        fx, fy = (float(v) for v in xy.split(","))
        pixels.append((name, int(fx * tex.width), int(fy * tex.height)))

    seen_events = set()
    for name, x, y in pixels:
        hist = ctl.PixelHistory(target, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        passed = [h.eventId for h in hist if h.Passed()]
        uniq = []
        for e in passed:
            if e not in uniq:
                uniq.append(e)
        log("\nPIXEL", name, (x, y), "writes passed:", len(passed), "events:", uniq[:20])
        for eid in uniq[:8]:
            seen_events.add(eid)
            a = by_eid.get(eid)
            log("  event", eid, a.GetName(ctl.GetStructuredFile()) if a else "?",
                "indices", a.numIndices if a else "?", "instances", a.numInstances if a else "?")

    log("\n=== per-event vertex-shader data ===")
    for eid in sorted(seen_events):
        ctl.SetFrameEvent(eid, True)
        pipe = ctl.GetPipelineState()
        a = by_eid.get(eid)
        log("\nEVENT", eid, a.GetName(ctl.GetStructuredFile()) if a else "?")
        try:
            vs = pipe.GetShader(rd.ShaderStage.Vertex)
            log("  VS", vs)
            refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
            nblocks = len(refl.constantBlocks) if refl else 0
            log("  VS constant blocks (reflection):", nblocks)
            for i in range(max(nblocks, 1) if refl else 4):
                try:
                    used = pipe.GetConstantBlock(rd.ShaderStage.Vertex, i, 0)
                    d = used.descriptor
                    name = refl.constantBlocks[i].name if refl and i < nblocks else "?"
                    if d.resource == rd.ResourceId.Null():
                        log("   block", i, name, "no buffer (inline/root constants?)")
                        continue
                    size = min(d.byteSize if d.byteSize else 256, 512)
                    data = ctl.GetBufferData(d.resource, d.byteOffset, size)
                    log("   block", i, name, "buf", d.resource, "off", d.byteOffset, "size", d.byteSize)
                    log("     ", fmt_floats(floats(data, 64)))
                except Exception as e:  # noqa: BLE001
                    log("   block", i, "error", e)
        except Exception as e:  # noqa: BLE001
            log("  pipeline error", e)
        if has_cs and a is not None:
            try:
                ev = a.events[-1] if len(a.events) else None
                cs = list(ev.callstack) if ev is not None else []
                log("  callstack raw:", " ".join("%x" % c for c in cs[:16]))
                if resolver_ok and cs:
                    log("  callstack resolved:", " | ".join(cap.GetResolve(cs[:16])))
            except Exception as e:  # noqa: BLE001
                log("  callstack error", e)
    ctl.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:  # noqa: BLE001
    log("FATAL", traceback.format_exc())
finally:
    OUT.close()
    os._exit(0)
