"""Second-round openwarp study: the residual after sharp sampling is the
splat's half-texel foreground dilation ring. This harness (corner-quad
splat with optional directional erosion + agreement-gated sharp reproject
+ 13x13 closest-valid stretch fill) decides between candidate ring fixes.

Findings (see docs/openwarp-edge-sharpness.md):
- Directional erosion (splat(..., edge_ratio, inset)): inset 0.25 changes
  nothing (floor() quantization), inset 0.5 punches cut holes that the
  stretch's farthest-tie fills with background. Erosion is a halo<->cut
  slider, not a fix.
- Disagreement 50/50 blend (BLEND=True: nearest + nearest-matching tap
  averaged when they disagree): worse error mass at every shift on dark
  backgrounds — position-weighted bilinear is already the best static
  estimate without coverage data.
Run: python3 scripts/owsim-erode.py
"""
import numpy as np

W, H, F = 200, 100, 100.0
Z_BG, Z_FG = 5.0, 1.0
C_BG, C_FG = 0.1, 1.0
FG_X0, FG_X1 = -0.2, 0.2
FG_Y0, FG_Y1 = -0.15, 0.15


def render_scene(cam_x):
    c = np.full((H, W), C_BG)
    z = np.full((H, W), Z_BG)
    for j in range(H):
        for i in range(W):
            dx = (i + 0.5 - W / 2) / F
            dy = (j + 0.5 - H / 2) / F
            if FG_X0 <= cam_x + dx * Z_FG <= FG_X1 and FG_Y0 <= dy * Z_FG <= FG_Y1:
                c[j, i] = C_FG
                z[j, i] = Z_FG
    return c, z


def splat(src_z, t, edge_ratio=0.0, inset=0.0):
    """edge_ratio=0 -> baseline quads. Else directional erosion: sides whose
    4-neighbour is farther by >edge_ratio get corners inset by `inset`
    (fraction of half-texel; 1.0 = pull to center plane)."""
    wdepth = np.full((H, W), np.inf)
    for j in range(H):
        for i in range(W):
            zc = src_z[j, i]
            ins = [0.0, 0.0, 0.0, 0.0]  # -x,+x,-y,+y
            if edge_ratio > 0:
                nb = [src_z[j, min(i + 1, W - 1)], src_z[j, max(i - 1, 0)],
                      src_z[min(j + 1, H - 1), i], src_z[max(j - 1, 0), i]]
                # order: +x,-x,+y,-y neighbours -> inset that side
                for k, zn in enumerate(nb):
                    if zn > zc * edge_ratio:
                        ins[[1, 0, 3, 2][k]] = inset
            pts = []
            for ox, oy in [(-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5)]:
                # inset per-side: corner x pulled toward 0 if its side eroded
                ix = -ins[0] if ox < 0 else ins[1]
                iy = -ins[2] if oy < 0 else ins[3]
                qx = ox + (0.5 * ix if ox < 0 else -0.5 * ix)
                qy = oy + (0.5 * iy if oy < 0 else -0.5 * iy)
                ddx = (i + 0.5 + qx - W / 2) / F
                ddy = (j + 0.5 + qy - H / 2) / F
                Xc, Yc = ddx * zc, ddy * zc
                u = (Xc - t) / zc * F + W / 2 - 0.5
                v = Yc / zc * F + H / 2 - 0.5
                pts.append((u, v))
            corner = np.array(pts)
            lo = np.floor(corner.min(axis=0) + 1e-9).astype(int)
            hi = np.ceil(corner.max(axis=0) - 1e-9).astype(int) - 1
            if hi[0] < lo[0] or hi[1] < lo[1]:
                # degenerate (thin line eroded both sides): point splat
                cx = int(np.floor(((i + 0.5 - W / 2) / F * zc - t) / zc * F + W / 2 - 0.5))
                cy = j
                lo = hi = np.array([cx, cy])
            # clamp to center +-2 like the shader (center = un-eroded middle;
            # lateral t shifts columns only, center row is j)
            ddx0 = (i + 0.5 - W / 2) / F
            cx = int(np.floor((ddx0 * zc - t) / zc * F + W / 2 - 0.5))
            cy = j
            lo[0] = max(lo[0], cx - 2, 0)
            lo[1] = max(lo[1], cy - 2, 0)
            hi[0] = min(hi[0], cx + 2, W - 1)
            hi[1] = min(hi[1], cy + 2, H - 1)
            for yy in range(lo[1], hi[1] + 1):
                for xx in range(lo[0], hi[0] + 1):
                    if zc < wdepth[yy, xx]:
                        wdepth[yy, xx] = zc
    return wdepth


def bilinear(img, u, v):
    x0, y0 = int(np.floor(u)), int(np.floor(v))
    fx, fy = u - x0, v - y0
    q = lambda xx, yy: img[min(max(yy, 0), H - 1), min(max(xx, 0), W - 1)]
    return (q(x0, y0) * (1 - fx) * (1 - fy) + q(x0 + 1, y0) * fx * (1 - fy) +
            q(x0, y0 + 1) * (1 - fx) * fy + q(x0 + 1, y0 + 1) * fx * fy)


def taps2x2(img, u, v):
    x0, y0 = int(np.floor(u)), int(np.floor(v))
    return [img[min(max(y0 + oy, 0), H - 1), min(max(x0 + ox, 0), W - 1)]
            for oy in (0, 1) for ox in (0, 1)], (x0, y0)


def sample_sharp(u, v, src_c, src_z, z_exp, force_nearest=False, blend=False):
    """Returns (color, occluded). Mirrors sample_source_sharp. blend=True
    adds the disagreement blend (nearest + nearest-matching tap)."""
    zt, (x0, y0) = taps2x2(src_z, u, v)
    # tap coords + ref (nearest) tap
    tc = []
    for k in range(4):
        tc.append((min(max(x0 + (k % 2), 0), W - 1),
                   min(max(y0 + (k // 2), 0), H - 1)))
    ref = min(range(4), key=lambda k: (tc[k][0] - u) ** 2 + (tc[k][1] - v) ** 2)
    best = zt[ref]
    if best >= z_exp * 0.95 and best <= max(z_exp / 0.95, 1e-6):
        zmin, zmax = min(zt), max(zt)
        if not force_nearest and zmax <= max(zmin / 0.95, 1e-6):
            return bilinear(src_c, u, v), False
        xx = min(max(int(np.floor(u + 0.5)), 0), W - 1)
        yy = min(max(int(np.floor(v + 0.5)), 0), H - 1)
        return src_c[yy, xx], False
    z_src = bilinear(src_z, u, v)
    if z_src < z_exp * 0.95:
        return 0.0, True
    if force_nearest:
        xx = min(max(int(np.floor(u + 0.5)), 0), W - 1)
        yy = min(max(int(np.floor(v + 0.5)), 0), H - 1)
        return src_c[yy, xx], False
    if blend:
        zmin, zmax = min(zt), max(zt)
        if zmax > max(zmin / 0.95, 1e-6):
            mt = [k for k in range(4)
                  if zt[k] >= z_exp * 0.95 and zt[k] <= max(z_exp / 0.95, 1e-6)]
            if mt:
                m = min(mt, key=lambda k: (tc[k][0] - u) ** 2 + (tc[k][1] - v) ** 2)
                xx = min(max(int(np.floor(u + 0.5)), 0), W - 1)
                yy = min(max(int(np.floor(v + 0.5)), 0), H - 1)
                return 0.5 * src_c[yy, xx] + 0.5 * src_c[tc[m][1], tc[m][0]], False
    return bilinear(src_c, u, v), False


def reproject(src_c, src_z, wdepth, t):
    out = np.zeros((H, W))
    stretched = np.zeros((H, W), bool)
    for j in range(H):
        for i in range(W):
            zw = wdepth[j, i]
            so = None
            if not np.isfinite(zw):
                # 13x13 closest-valid stretch via neighbour center
                bb, bq, bd2 = None, None, 1e18
                for dj in range(-6, 7):
                    for di in range(-6, 7):
                        xx = min(max(i + di, 0), W - 1)
                        yy = min(max(j + dj, 0), H - 1)
                        v = wdepth[yy, xx]
                        if not np.isfinite(v):
                            continue
                        dd = di * di + dj * dj
                        if dd < bd2 or (dd == bd2 and v > bb):
                            bd2, bb, bq = dd, v, (xx, yy)
                if bq is None:
                    continue
                zw = bb
                so = bq
                stretched[j, i] = True
            if so is None:
                Xw = (i + 0.5 - W / 2) / F * zw
                Yw = (j + 0.5 - H / 2) / F * zw
            else:
                Xw = (so[0] + 0.5 - W / 2) / F * zw
                Yw = (so[1] + 0.5 - H / 2) / F * zw
            X, Y = Xw + t, Yw
            u = X / zw * F + W / 2 - 0.5
            v = Y / zw * F + H / 2 - 0.5
            if u < 0 or u > W - 1 or v < 0 or v > H - 1:
                continue
            c, _ = sample_sharp(u, v, src_c, src_z, max(zw, 1e-6),
                                force_nearest=stretched[j, i], blend=BLEND)
            out[j, i] = c
    return out


def metrics(out, gt_c):
    err = np.abs(out - gt_c)
    bad = err > 0.1
    fringe = bad & (gt_c < 0.5) & (out > 0.5)
    cut = bad & (gt_c > 0.5) & (out < 0.5)
    holes = bad & (out == 0.0)
    return bad.sum(), fringe.sum(), cut.sum(), holes.sum(), err.sum()


for t in (0.008, 0.023, 0.057, 0.13):
    src_c, src_z = render_scene(0.0)
    gt_c, gt_z = render_scene(t)
    for BLEND in (False, True):
        wd = splat(src_z, t)
        out = reproject(src_c, src_z, wd, t)
        b, f, c, h, m = metrics(out, gt_c)
        print(f't={t * 1000:5.1f}mm blend={int(BLEND)}: bad={b} fr={f} cut={c} ho={h} mass={m:6.1f}',
              flush=True)
