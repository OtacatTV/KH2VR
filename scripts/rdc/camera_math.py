"""Matrix classifiers shared by the RenderDoc analysis scripts (pure functions, no RenderDoc import)."""
import math


def dot3(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def norm3(a):
    return math.sqrt(dot3(a, a))


def classify(m):
    for v in m:
        if v != v or abs(v) > 1e7:
            return None
    rows = [m[0:3], m[4:7], m[8:11]]
    # VIEW (orthonormal 3x3), translation either in m[12..14] or m[3],m[7],m[11]
    if all(abs(norm3(r) - 1) < 2e-3 for r in rows) and all(abs(dot3(rows[i], rows[j])) < 2e-3 for i, j in ((0, 1), (0, 2), (1, 2))):
        if abs(m[15] - 1) < 1e-4 and not all(r[k] in (0.0, 1.0, -1.0) for r in rows for k in range(3)):
            return ("VIEW", None)
        return None
    # VIEWPROJ, row-vector: columns c0..c3 over the first three rows; c3 = forward (unit), m[15] free.
    for layout in ("row", "col"):
        if layout == "row":
            c = [(m[0 + k], m[4 + k], m[8 + k]) for k in range(4)]
        else:
            c = [(m[4 * k + 0], m[4 * k + 1], m[4 * k + 2]) for k in range(4)]
        fwd, cx, cy = c[3], c[0], c[1]
        if abs(norm3(fwd) - 1) > 2e-3:
            continue
        xs, ys = norm3(cx), norm3(cy)
        if not (0.2 < xs < 10 and 0.2 < ys < 10):
            continue
        if abs(dot3(cx, fwd)) > 2e-3 * xs or abs(dot3(cy, fwd)) > 2e-3 * ys or abs(dot3(cx, cy)) > 2e-3 * xs * ys:
            continue
        return ("VIEWPROJ-" + layout, (xs, ys, fwd))
    return None


def classify_wvp(m):
    """Scale-tolerant perspective check for a combined world*view*proj matrix (row- or column-vector).

    Columns cx, cy, fwd (row-vector) must be mutually orthogonal; |fwd| = object scale s, |cx| = s*xs,
    |cy| = s*ys. Returns (layout, s, xs, ys, fwd_unit) or None.
    """
    for v in m:
        if v != v or abs(v) > 1e7:
            return None
    for layout in ("row", "col"):
        if layout == "row":
            c = [(m[0 + k], m[4 + k], m[8 + k]) for k in range(4)]
        else:
            c = [(m[4 * k + 0], m[4 * k + 1], m[4 * k + 2]) for k in range(4)]
        fwd, cx, cy = c[3], c[0], c[1]
        s, nx, ny = norm3(fwd), norm3(cx), norm3(cy)
        if s < 1e-4 or nx < 1e-4 or ny < 1e-4:
            continue
        if abs(dot3(cx, fwd)) > 3e-3 * nx * s or abs(dot3(cy, fwd)) > 3e-3 * ny * s or abs(dot3(cx, cy)) > 3e-3 * nx * ny:
            continue
        xs, ys = nx / s, ny / s
        if not (0.2 < xs < 10 and 0.2 < ys < 10):
            continue
        return (layout, s, xs, ys, (fwd[0] / s, fwd[1] / s, fwd[2] / s))
    return None
