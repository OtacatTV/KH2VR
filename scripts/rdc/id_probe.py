"""RenderDoc analysis helper: how structured-file resource IDs map to replay resources (for orientation).

Run inside RenderDoc's embedded Python. Environment: KH2VR_RDC, KH2VR_OUT, KH2VR_IDS (comma-separated ints).
"""
import os
import traceback

import renderdoc as rd

OUT = open(os.environ["KH2VR_OUT"], "w", encoding="utf-8")


def log(*a):
    OUT.write(" ".join(str(x) for x in a) + "\n")


try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["KH2VR_RDC"], "", None)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    log("controller methods with Live/ID:", [m for m in dir(ctl) if "Live" in m or "ID" in m or "Id" in m])
    want = [int(x) for x in os.environ.get("KH2VR_IDS", "").split(",") if x]
    resources = ctl.GetResources()
    log("resources", len(resources))
    for r in resources[:3]:
        log("sample", str(r.resourceId), int(r.resourceId) if hasattr(r.resourceId, "__int__") else "?", r.name, r.type)
    for r in resources:
        rid_int = None
        try:
            rid_int = int(r.resourceId)
        except Exception:  # noqa: BLE001
            pass
        if any(str(w) in r.name or rid_int == w for w in want):
            log("MATCH", str(r.resourceId), rid_int, repr(r.name), r.type)
    for b in ctl.GetBuffers()[:3]:
        log("buffer sample", str(b.resourceId), b.length, hex(b.gpuAddress))
    ctl.Shutdown()
    cap.Shutdown()
except Exception:  # noqa: BLE001
    log("FATAL", traceback.format_exc())
finally:
    OUT.close()
    os._exit(0)
