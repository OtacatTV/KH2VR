"""RenderDoc analysis: list every action in a capture (event id, flags, name, outputs), for orientation.

Run inside RenderDoc's embedded Python:  qrenderdoc.exe --python scripts\\rdc\\list_actions.py
Environment: KH2VR_RDC (capture), KH2VR_OUT (report file).
"""
import os
import traceback

import renderdoc as rd

OUT = open(os.environ["KH2VR_OUT"], "w", encoding="utf-8")


def log(*a):
    OUT.write(" ".join(str(x) for x in a) + "\n")


def walk(actions, depth, ctl, sf):
    for a in actions:
        outs = [str(o) for o in a.outputs if o != rd.ResourceId.Null()]
        log("%s%d flags=%s idx=%d inst=%d %s outs=%s" % ("  " * depth, a.eventId, str(a.flags).replace("ActionFlags.", ""),
                                                      a.numIndices, a.numInstances, a.GetName(sf), ",".join(outs)))
        walk(a.children, depth + 1, ctl, sf)


try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["KH2VR_RDC"], "", None)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    sf = ctl.GetStructuredFile()
    log("API", ctl.GetAPIProperties().pipelineType, "chunks", len(sf.chunks))
    walk(ctl.GetRootActions(), 0, ctl, sf)
    ctl.Shutdown()
    cap.Shutdown()
except Exception:  # noqa: BLE001
    log("FATAL", traceback.format_exc())
finally:
    OUT.close()
    os._exit(0)
