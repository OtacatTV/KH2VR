"""RenderDoc analysis: which ExecuteBundle calls change which screen regions, and what the capture records
inside bundles.

Bundle contents aren't listed as separate actions, so pixel history stops at the bundle. Instead this replays
to each ExecuteBundle and diffs its colour output against the state before it (bytes compared row by row,
then in cells). Prints per bundle: output id, changed-pixel count, bounding box and a coarse cell map
(KH2VR_GRID cols x rows, default 32x18; '#' many changed, '+' some, '.' none). Also prints a histogram of
structured chunk names (to see whether bundle command lists, i.e. their draws, are in the capture) and
saves each distinct output's final image as PNG for orientation.

Run inside RenderDoc's embedded Python:  qrenderdoc.exe --python scripts\\rdc\\bundle_pixels.py
Environment: KH2VR_RDC (capture), KH2VR_OUT (report), KH2VR_PNG_DIR (optional folder for final images),
KH2VR_GRID ("32x18").
"""
import collections
import os
import traceback

import renderdoc as rd

OUT = open(os.environ["KH2VR_OUT"], "w", encoding="utf-8")


def log(*a):
    OUT.write(" ".join(str(x) for x in a) + "\n")
    OUT.flush()


def main():
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["KH2VR_RDC"], "", None)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    sf = ctl.GetStructuredFile()
    gx, gy = (int(v) for v in os.environ.get("KH2VR_GRID", "32x18").split("x"))

    hist = collections.Counter(c.name for c in sf.chunks)
    log("CHUNK NAMES")
    for n, k in hist.most_common():
        log("  %6d %s" % (k, n))

    tex = {int(t.resourceId): t for t in ctl.GetTextures()}
    acts = []

    def walk(a):
        for x in a:
            acts.append(x)
            walk(x.children)

    walk(ctl.GetRootActions())
    prev_ev = {}
    last_data = {}
    finals = {}
    sub = rd.Subresource(0, 0, 0)
    for i, a in enumerate(acts):
        outs = [o for o in a.outputs if o != rd.ResourceId.Null()]
        if not outs:
            continue
        o = outs[0]
        oi = int(o)
        t = tex.get(oi)
        if t is None:
            continue
        name = a.GetName(sf)
        if "ExecuteBundle" not in name:
            prev_ev[oi] = a.eventId
            last_data.pop(oi, None)
            continue
        w, h = t.width, t.height
        bpp = t.format.compByteWidth * t.format.compCount or 4
        if oi not in last_data:
            ctl.SetFrameEvent(prev_ev.get(oi, a.eventId - 1), True)
            last_data[oi] = bytes(ctl.GetTextureData(o, sub))
        before = last_data[oi]
        ctl.SetFrameEvent(a.eventId, True)
        after = bytes(ctl.GetTextureData(o, sub))
        last_data[oi] = after
        finals[oi] = a.eventId
        prev_ev[oi] = a.eventId
        row = w * bpp
        if len(after) < row * h or len(before) != len(after):
            log("BUNDLE ev %d out %s size mismatch %d/%d (w %d h %d bpp %d)" % (a.eventId, o, len(before), len(after), w, h, bpp))
            continue
        cells = [[0] * gx for _ in range(gy)]
        cw = max(1, w // gx)
        ch = max(1, h // gy)
        changed = 0
        x0, y0, x1, y1 = w, h, -1, -1
        for y in range(h):
            s = y * row
            if before[s:s + row] == after[s:s + row]:
                continue
            for cx in range(gx):
                a0 = s + cx * cw * bpp
                a1 = s + min(w, (cx + 1) * cw) * bpp
                if before[a0:a1] == after[a0:a1]:
                    continue
                # count changed pixels in this cell row segment
                n = 0
                for p in range(a0, a1, bpp):
                    if before[p:p + bpp] != after[p:p + bpp]:
                        n += 1
                        px = (p - s) // bpp
                        x0 = min(x0, px)
                        x1 = max(x1, px)
                changed += n
                cells[min(gy - 1, y // ch)][cx] += n
                y0 = min(y0, y)
                y1 = max(y1, y)
        log("BUNDLE ev %d out %s %dx%d fmt %s changed %d bbox %s" % (
            a.eventId, o, w, h, t.format.Name(), changed, (x0, y0, x1, y1) if changed else "-"))
        if changed:
            full = cw * ch
            for r in cells:
                log("      " + "".join("#" if v > full // 4 else ("+" if v else ".") for v in r))
    pngdir = os.environ.get("KH2VR_PNG_DIR")
    if pngdir:
        os.makedirs(pngdir, exist_ok=True)
        for oi, ev in finals.items():
            ctl.SetFrameEvent(ev, True)
            s = rd.TextureSave()
            s.resourceId = tex[oi].resourceId
            s.destType = rd.FileType.PNG
            s.alpha = rd.AlphaMapping.Discard
            path = os.path.join(pngdir, "out_%d_ev%d.png" % (oi, ev))
            ctl.SaveTexture(s, path)
            log("saved", path)
    ctl.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:  # noqa: BLE001
    log("FATAL", traceback.format_exc())
finally:
    OUT.close()
    os._exit(0)
