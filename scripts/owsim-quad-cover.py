"""Splat-coverage sim + far-side-nearest sampling sim for the compute-openwarp
edge protrusion/nick investigation (see docs/openwarp-edge-sharpness.md).

Scene: bg wall at z=5 (color 0.1), foreground at z=1 (color 1.0) with
FG_SHAPE in {'diamond' (diagonal straight edges), 'disc' (curved
silhouette), 'bar' (thin finger-like geometry)}. Pinhole camera.
Warp = lateral head translation t. Ground truth = scene rasterized at warp.

Compares, on the CURRENT bbox splat: legacy disagreement sampling vs the
far-side-nearest candidate (UV-side tap behind the warp surface -> exact
nearest texel instead of bilinear; zero extra taps). Splat modes 'exact'
(center-in-quad test), 'nodes' (node-minimum corner depths) and 'combo'
(wider clamp + inside test) were also tried here: all trade fringe<->cut,
none dominates — see the work log. Kept for the record.

Metrics mirror scripts/owsim-edge-sharp.py: bad (abs err > 0.1), fringe
(bg-truth painted fg), cut (fg-truth painted bg), holes (honest black), mass
(total abs error).

Run: python3 scripts/owsim-quad-cover.py
"""

import math

import numpy as np

W, H, F = 200, 100, 100.0  # width, height, focal px (90 deg FOV)
Z_BG, Z_FG = 5.0, 1.0
C_BG, C_FG = 0.1, 1.0
# Diamond: square centered origin, "radius" (center to vertex) in world units
# at the fg plane, rotated 30 deg so all four edges are diagonal on screen.
DIAM_R = 0.22
DIAM_ROT = math.radians(30.0)
COS_R, SIN_R = math.cos(DIAM_ROT), math.sin(DIAM_ROT)
# Foreground shape selector: 'diamond' (diagonal straight edges), 'disc'
# (curved silhouette — real-game generality check), 'bar' (6px-wide thin
# vertical bar — finger-like thin geometry with edges on both sides).
FG_SHAPE = "diamond"
DISC_R = 0.16
BAR_HW = 0.03  # half-width in world units at z=1 (~6px)


def inside_fg(xw, yw):
    if FG_SHAPE == "disc":
        return xw * xw + yw * yw <= DISC_R * DISC_R
    if FG_SHAPE == "bar":
        return abs(xw) <= BAR_HW and abs(yw) <= 0.15
    # diamond: rotate point back by -30 deg, then |x|+|y| <= R.
    xr = xw * COS_R + yw * SIN_R
    yr = -xw * SIN_R + yw * COS_R
    return abs(xr) + abs(yr) <= DIAM_R


def inside_diamond(xw, yw):
    return inside_fg(xw, yw)


def render_scene(cam_x):
    """Exact color+eyeZ images from a camera at world x=cam_x. Returns (c, z)."""
    c = np.full((H, W), C_BG)
    z = np.full((H, W), Z_BG)
    for j in range(H):
        for i in range(W):
            dx = (i + 0.5 - W / 2) / F
            dy = (j + 0.5 - H / 2) / F
            xw = cam_x + dx * Z_FG
            yw = dy * Z_FG
            if inside_diamond(xw, yw):
                c[j, i] = C_FG
                z[j, i] = Z_FG
    return c, z


def project_pt(Xc, Yc, zc, t):
    """Project a render-cam-space point (same center depth zc) to warp pixel
    coords (float). Warp cam at world x=t. Returns None if behind warp cam."""
    Xw = Xc - t
    if zc <= 0:
        return None
    u = Xw / zc * F + W / 2 - 0.5
    v = Yc / zc * F + H / 2 - 0.5
    return (u, v)


def point_in_convex_quad(px, py, quad):
    """True if (px,py) is inside the convex quad (list of 4 (x,y)).
    Orientation-free: an interior point has all four edge cross products
    with the same sign (boundary counts as inside)."""
    pos = neg = False
    for k in range(4):
        x0, y0 = quad[k]
        x1, y1 = quad[(k + 1) % 4]
        c = (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0)
        if c > 1e-9:
            pos = True
        elif c < -1e-9:
            neg = True
        if pos and neg:
            return False
    return True


def splat(src_z, t, mode):
    """Forward splat source eyeZ to warp view (cam at x=t). Nearest wins.
    mode='bbox': current shader (every bbox pixel splatted, clamp +-2).
    mode='exact': + center-in-quad test (single-pixel bboxes unchanged).
    mode='nodes': per-corner node-minimum depths (seamless tiling at steps)
                  + center-in-quad test.
    mode='combo': widened clamp (+-3) + center-in-quad test (single-pixel
                  bboxes splat directly). Tests whether exact coverage plus
                  room for sheared quads removes both overreach (fringe)
                  and clamp-cut gaps (cut) at once.
    Returns (wdepth, (tested, rejected))."""
    wdepth = np.full((H, W), np.inf)  # warp eyeZ
    n_tested = 0
    n_rejected = 0

    def node_depth(fx, fy):
        # depth nodes live at integer texel-index coords; surrounding texel
        # centers are at (fx,fy) with fx,fy = index+0.5. The four texels
        # around node (nx,ny) have indices nx-1..nx, ny-1..ny. Minimum wins
        # (nearest surface owns the shared boundary).
        best = np.inf
        for ax in (int(math.floor(fx - 0.5)), int(math.floor(fx - 0.5)) + 1):
            for ay in (int(math.floor(fy - 0.5)), int(math.floor(fy - 0.5)) + 1):
                axc = min(max(ax, 0), W - 1)
                ayc = min(max(ay, 0), H - 1)
                best = min(best, src_z[ayc, axc])
        return best

    for j in range(H):
        for i in range(W):
            zc = src_z[j, i]
            dx = (i + 0.5 - W / 2) / F
            dy = (j + 0.5 - H / 2) / F
            X, Y = dx * zc, dy * zc  # world (render cam at origin)
            # corner list: (ox, oy) texel offsets, then per-corner depth.
            offs = [(0, 0), (-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5)]
            if mode == "nodes":
                corner_z = [zc]
                for ox, oy in offs[1:]:
                    # node position of this corner in depth-texel index space:
                    # center is at index+0.5, corner offset by (ox,oy).
                    corner_z.append(node_depth(i + 0.5 + ox, j + 0.5 + oy))
            else:
                corner_z = [zc] * 5
            pts = []
            ok = True
            for (ox, oy), zcc in zip(offs, corner_z):
                ddx = (i + 0.5 + ox - W / 2) / F
                ddy = (j + 0.5 + oy - H / 2) / F
                Xc, Yc = ddx * zcc, ddy * zcc
                q = project_pt(Xc, Yc, zcc, t)
                if q is None:
                    ok = False
                    break
                pts.append(q)
            if not ok:
                continue
            center, corners = pts[0], pts[1:]
            cx = int(math.floor(center[0]))
            cy = int(math.floor(center[1]))
            clamp_r = 3 if mode == "combo" else 2
            xs = [p[0] for p in corners]
            ys = [p[1] for p in corners]
            lox = max(min(int(math.floor(v)) for v in xs), cx - clamp_r, 0)
            hix = min(max(int(math.floor(v)) for v in xs), cx + clamp_r, W - 1)
            loy = max(min(int(math.floor(v)) for v in ys), cy - clamp_r, 0)
            hiy = min(max(int(math.floor(v)) for v in ys), cy + clamp_r, H - 1)
            single = (lox == hix and loy == hiy)
            test_inside = mode in ("exact", "nodes", "combo") and not single
            for yy in range(loy, hiy + 1):
                for xx in range(lox, hix + 1):
                    if test_inside:
                        n_tested += 1
                        if not point_in_convex_quad(xx + 0.5, yy + 0.5, corners):
                            n_rejected += 1
                            continue
                    if zc < wdepth[yy, xx]:
                        wdepth[yy, xx] = zc
    return wdepth, (n_tested, n_rejected)


def bilinear(img, u, v):
    x0, y0 = int(math.floor(u)), int(math.floor(v))
    fx, fy = u - x0, v - y0
    vals = []
    for oy in (0, 1):
        for ox in (0, 1):
            xx = min(max(x0 + ox, 0), W - 1)
            yy = min(max(y0 + oy, 0), H - 1)
            vals.append(img[yy, xx])
    return vals[0] * (1 - fx) * (1 - fy) + vals[1] * fx * (1 - fy) + vals[2] * (1 - fx) * fy + vals[3] * fx * fy


def taps2x2(img, u, v):
    x0, y0 = int(math.floor(u)), int(math.floor(v))
    out = []
    for oy in (0, 1):
        for ox in (0, 1):
            xx = min(max(x0 + ox, 0), W - 1)
            yy = min(max(y0 + oy, 0), H - 1)
            out.append(img[yy, xx])
    return out


def reproject_final(src_c, src_z, wdepth, t, stretch=True, farside_nearest=False):
    """Mirror of sample_source_sharp (agreement -> sharp, else legacy) plus
    the 13x13 closest-valid stretch fill (ties toward farthest, reproject
    through the neighbour's center with nearest-texel sampling).
    farside_nearest (candidate fix): on the disagreement path, when the
    UV-side tap is FARTHER than the warp surface (UV addresses background
    behind this pixel's surface), fetch the exact nearest texel instead of
    bilinear. No extra taps: z_ref/zt already fetched. Rationale: the
    bilinear kernel at a step always mixes surfaces; the nearest tap is the
    surface the UV actually addresses. Should blacken dilation/excess
    pixels (truth bg) while leaving agreement pixels bit-identical."""
    out = np.zeros((H, W))

    def shade(i, j, zw, ci, cj, force_nearest):
        # reproject warp pixel (i,j) with depth zw through the sample
        # position of pixel (ci,cj) (own center, or neighbour center when
        # stretched). Returns color or None (occluded / out of range).
        Xw = (ci + 0.5 - W / 2) / F * zw
        Yw = (cj + 0.5 - H / 2) / F * zw
        X, Y = Xw + t, Yw
        u = X / zw * F + W / 2 - 0.5
        v = Y / zw * F + H / 2 - 0.5
        if u < 0 or u > W - 1 or v < 0 or v > H - 1:
            return None
        # expected render-view eyeZ of this exact point: render cam at
        # origin, so eyeZ == world z == zw for frontoparallel scene; use
        # the unprojected world point in general.
        ox = (i + 0.5 - W / 2) / F * zw
        oy = (j + 0.5 - H / 2) / F * zw
        z_exp = max(zw, 1e-6)
        _ = (ox, oy)
        zt = taps2x2(src_z, u, v)
        x0, y0 = int(math.floor(u)), int(math.floor(v))
        best, bestd = None, 1e30
        for k in range(4):
            tx = min(max(x0 + (k % 2), 0), W - 1)
            ty = min(max(y0 + (k // 2), 0), H - 1)
            dd = (tx - u) ** 2 + (ty - v) ** 2
            if dd < bestd:
                bestd, best = dd, zt[k]
        if best >= z_exp * 0.95 and best <= max(z_exp / 0.95, 1e-6):
            zmin, zmax = min(zt), max(zt)
            if not force_nearest and zmax <= max(zmin / 0.95, 1e-6):
                return bilinear(src_c, u, v)
            xx = min(max(int(math.floor(u + 0.5)), 0), W - 1)
            yy = min(max(int(math.floor(v + 0.5)), 0), H - 1)
            return src_c[yy, xx]
        if farside_nearest and not force_nearest and best > max(z_exp / 0.95, 1e-6):
            # UV-side tap is behind the warp surface: the UV addresses
            # background, so take that exact texel instead of a mix.
            xx = min(max(int(math.floor(u + 0.5)), 0), W - 1)
            yy = min(max(int(math.floor(v + 0.5)), 0), H - 1)
            return src_c[yy, xx]
        z_src = bilinear(src_z, u, v)
        if z_src < z_exp * 0.95:
            return None  # occluded: honest black
        if force_nearest:
            xx = min(max(int(math.floor(u + 0.5)), 0), W - 1)
            yy = min(max(int(math.floor(v + 0.5)), 0), H - 1)
            return src_c[yy, xx]
        return bilinear(src_c, u, v)

    for j in range(H):
        for i in range(W):
            zw = wdepth[j, i]
            ci, cj, force = i, j, False
            if not np.isfinite(zw):
                if not stretch:
                    continue  # honest black (no stretch fill in this sim)
                best_d2, best_z, bi, bj = None, None, -1, -1
                for jj in range(max(0, j - 6), min(H, j + 7)):
                    for ii in range(max(0, i - 6), min(W, i + 7)):
                        zc = wdepth[jj, ii]
                        if not np.isfinite(zc):
                            continue
                        d2 = (ii - i) ** 2 + (jj - j) ** 2
                        if best_d2 is None or d2 < best_d2 or (d2 == best_d2 and zc > best_z):
                            best_d2, best_z, bi, bj = d2, zc, ii, jj
                if bi < 0:
                    continue
                zw, ci, cj, force = best_z, bi, bj, True
            col = shade(i, j, zw, ci, cj, force)
            if col is not None:
                out[j, i] = col
    return out


def metrics(out, gt_c):
    err = np.abs(out - gt_c)
    bad = err > 0.1
    fringe = bad & (gt_c < 0.5) & (out > 0.5)  # bg truth, fg color
    cut = bad & (gt_c > 0.5) & (out < 0.5)  # fg truth, bg color
    holes = bad & (out == 0.0) & (gt_c > 0.0)
    return bad.sum(), fringe.sum(), cut.sum(), holes.sum(), err.sum()


print(f"shape={FG_SHAPE}")
print("t      | bbox: bad fringe cut holes mass | farside-nearest: bad fringe cut holes mass")
for t in (0.008, 0.023, 0.05, 0.057, 0.13, 0.15):
    src_c, src_z = render_scene(0.0)
    gt_c, _ = render_scene(t)
    wd, _ = splat(src_z, t, "bbox")  # current shader splat
    outs = {
        "bbox": reproject_final(src_c, src_z, wd, t),
        "far": reproject_final(src_c, src_z, wd, t, farside_nearest=True),
    }
    m = {}
    for tag in outs:
        m[tag] = metrics(outs[tag], gt_c)
    bo, fo, co, ho, mo = m["bbox"]
    ba, fa, ca, ha, ma = m["far"]
    print(
        f"{t * 1000:5.1f}mm | bbox: {bo:4d} {fo:4d} {co:3d} {ho:4d} {mo:6.1f} "
        f"| far:  {ba:4d} {fa:4d} {ca:3d} {ha:4d} {ma:6.1f}"
    )
