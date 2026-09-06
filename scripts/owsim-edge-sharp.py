"""CPU replica of compute-openwarp pass-1 sampling: legacy (bilinear color +
bilinear-depth occlusion test) vs the shipped agreement-gated sharp pick
(sample_source_sharp in openwarp.comp).

Scene: bg wall at z=5 (color 0.1), fg quad at z=1 (color 1.0), pinhole camera.
Warp = lateral head translation t. Ground truth = scene rasterized at warp.
Metrics mirror doc/openwarp_integration.md: bad (abs err > 0.1), fringe
(bg-truth painted fg), cut (fg-truth painted bg), holes (honest black),
mass (total abs error).

Re rejected alternatives, see docs/openwarp-edge-sharpness.md: the
nearest-matching-tap and nearest-only variants live on in that doc's table
(their code was removed here to keep one decision matrix). Run:
  python3 scripts/owsim-edge-sharp.py
"""
import numpy as np

W, H, F = 200, 100, 100.0  # width, height, focal px (90 deg FOV)
Z_BG, Z_FG = 5.0, 1.0
C_BG, C_FG = 0.1, 1.0
FG_X0, FG_X1 = -0.2, 0.2  # fg quad world x-range at render pose
FG_Y0, FG_Y1 = -0.15, 0.15


def render_scene(cam_x):
    """Exact color+eyeZ images from a camera at world x=cam_x. Returns (c, z)."""
    c = np.full((H, W), C_BG)
    z = np.full((H, W), Z_BG)
    for j in range(H):
        for i in range(W):
            # ray through pixel center in cam space
            dx = (i + 0.5 - W / 2) / F
            dy = (j + 0.5 - H / 2) / F
            # intersect fg plane z=1 -> world x = cam_x + dx*1
            xw = cam_x + dx * Z_FG
            yw = dy * Z_FG
            if FG_X0 <= xw <= FG_X1 and FG_Y0 <= yw <= FG_Y1:
                c[j, i] = C_FG
                z[j, i] = Z_FG
    return c, z


def splat(src_z, t):
    """Forward splat source eyeZ to warp view (cam at x=t). Nearest wins.
    Corner-quad footprint with center depth, bbox clamped to center +-2."""
    wdepth = np.full((H, W), np.inf)  # warp eyeZ
    for j in range(H):
        for i in range(W):
            zc = src_z[j, i]
            # source pixel center -> cam/render space -> world
            dx = (i + 0.5 - W / 2) / F
            dy = (j + 0.5 - H / 2) / F
            X, Y = dx * zc, dy * zc  # world (render cam at origin)
            # project center + 4 corners (same center depth) to warp cam
            pts = []
            ok = True
            for ox, oy in [(0, 0), (-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5)]:
                ddx = (i + 0.5 + ox - W / 2) / F
                ddy = (j + 0.5 + oy - H / 2) / F
                Xc, Yc = ddx * zc, ddy * zc
                Xw = Xc - t  # warp cam space
                if zc <= 0:
                    ok = False
                    break
                u = Xw / zc * F + W / 2 - 0.5
                v = Yc / zc * F + H / 2 - 0.5
                pts.append((u, v))
            if not ok:
                continue
            corner = np.array(pts[1:])
            lo = np.floor(corner.min(axis=0)).astype(int)
            hi = np.floor(corner.max(axis=0)).astype(int)
            cx = int(np.floor(pts[0][0]))
            cy = int(np.floor(pts[0][1]))
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
    vals = []
    for oy in (0, 1):
        for ox in (0, 1):
            xx = min(max(x0 + ox, 0), W - 1)
            yy = min(max(y0 + oy, 0), H - 1)
            vals.append(img[yy, xx])
    # vals order: (0,0),(1,0),(0,1),(1,1)
    return vals[0] * (1 - fx) * (1 - fy) + vals[1] * fx * (1 - fy) + vals[2] * (1 - fx) * fy + vals[3] * fx * fy


def taps2x2(img, u, v):
    x0, y0 = int(np.floor(u)), int(np.floor(v))
    out = []
    for oy in (0, 1):
        for ox in (0, 1):
            xx = min(max(x0 + ox, 0), W - 1)
            yy = min(max(y0 + oy, 0), H - 1)
            out.append(img[yy, xx])
    return out, (x0, y0)


def reproject_old(src_c, src_z, wdepth, t):
    out = np.zeros((H, W))
    for j in range(H):
        for i in range(W):
            zw = wdepth[j, i]
            if not np.isfinite(zw):
                continue  # honest black (stretch fill disabled in this sim)
            # warp center -> world -> source uv
            Xw = (i + 0.5 - W / 2) / F * zw
            Yw = (j + 0.5 - H / 2) / F * zw
            X, Y = Xw + t, Yw  # world
            u = X / zw * F + W / 2 - 0.5
            v = Y / zw * F + H / 2 - 0.5
            if u < 0 or u > W - 1 or v < 0 or v > H - 1:
                continue
            z_exp = zw  # render cam at origin: eyeZ == world z for this scene
            z_src = bilinear(src_z, u, v)
            if z_src < z_exp * 0.95:
                continue  # occluded
            out[j, i] = bilinear(src_c, u, v)
    return out


def reproject_final(src_c, src_z, wdepth, t):
    """Exact mirror of sample_source_sharp: agreement -> sharp (no occlusion
    test on that path); otherwise the legacy bilinear path."""
    out = np.zeros((H, W))
    for j in range(H):
        for i in range(W):
            zw = wdepth[j, i]
            if not np.isfinite(zw):
                continue  # honest black (no stretch fill in this sim)
            Xw = (i + 0.5 - W / 2) / F * zw
            Yw = (j + 0.5 - H / 2) / F * zw
            X, Y = Xw + t, Yw
            u = X / zw * F + W / 2 - 0.5
            v = Y / zw * F + H / 2 - 0.5
            if u < 0 or u > W - 1 or v < 0 or v > H - 1:
                continue
            z_exp = max(zw, 1e-6)
            zt, (x0, y0) = taps2x2(src_z, u, v)
            # ref tap nearest uv (tap centers at integer coords here)
            best, bestd = None, 1e30
            for k in range(4):
                ox, oy = (k % 2, k // 2)
                tx = min(max(x0 + ox, 0), W - 1)
                ty = min(max(y0 + oy, 0), H - 1)
                dd = (tx - u) ** 2 + (ty - v) ** 2
                if dd < bestd:
                    bestd, best = dd, zt[k]
            if best >= z_exp * 0.95 and best <= max(z_exp / 0.95, 1e-6):
                zmin, zmax = min(zt), max(zt)
                if zmax <= max(zmin / 0.95, 1e-6):
                    out[j, i] = bilinear(src_c, u, v)
                else:
                    xx = min(max(int(np.floor(u + 0.5)), 0), W - 1)
                    yy = min(max(int(np.floor(v + 0.5)), 0), H - 1)
                    out[j, i] = src_c[yy, xx]
                continue
            z_src = bilinear(src_z, u, v)
            if z_src < z_exp * 0.95:
                continue  # occluded: honest black
            out[j, i] = bilinear(src_c, u, v)
    return out


def metrics(out, gt_c, gt_z):
    err = np.abs(out - gt_c)
    bad = err > 0.1
    fringe = bad & (gt_c < 0.5) & (out > 0.5)   # bg truth, fg color
    cut = bad & (gt_c > 0.5) & (out < 0.5)      # fg truth, bg color
    holes = bad & (out == 0.0) & (gt_c > 0.0)
    return bad.sum(), fringe.sum(), cut.sum(), holes.sum(), err.sum()


print('t      | old: bad fringe cut holes mass  | final: bad fringe cut holes mass')
for t in (0.008, 0.023, 0.05, 0.057, 0.13, 0.15):
    src_c, src_z = render_scene(0.0)
    gt_c, gt_z = render_scene(t)
    wd = splat(src_z, t)
    old = reproject_old(src_c, src_z, wd, t)
    fin = reproject_final(src_c, src_z, wd, t)
    bo, fo, co, ho, mo = metrics(old, gt_c, gt_z)
    bf, ff, cf, hf, mf = metrics(fin, gt_c, gt_z)
    print(f'{t * 1000:5.1f}mm | old: {bo:4d} {fo:4d} {co:3d} {ho:4d} {mo:6.1f} '
          f'| fin: {bf:4d} {ff:4d} {cf:3d} {hf:4d} {mf:6.1f}')
