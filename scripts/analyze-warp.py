#!/usr/bin/env python3
"""analyze-warp.py — numeric warp-artifact metrics for a (warped, fresh) pair.

No image understanding involved: both frames are plain arrays. The warped
frame (stale source reprojected to a new head pose) is integer-aligned to
the fresh reference (same head pose, settled) per eye with FFT
cross-correlation, then three artifact classes are counted:

  halo    warped-bright but fresh-dark pixels near fresh silhouettes
          (foreground smeared into background: the screen-space outline)
  missing fresh-bright but warped-dark pixels (honest-black disocclusions
          and background-cut errors on foreground)
  resid   overall mean abs diff + pct of pixels > threshold after alignment

Bright/dark is a max-channel threshold (--hi, default 40: suits the
black-background playground; pass a higher value for bright scenes).

Usage:
  python3 scripts/analyze-warp.py warped.png fresh.png [--out PREFIX]
      [--maxshift 60] [--hi 40] [--band 4] [--crop x,y,w,h]
  Prints one JSON object to stdout; writes <PREFIX>_<eye>.png overlays
  (red=halo, green=missing) and <PREFIX>_diff.png when --out is given.
"""
import argparse
import json
import sys

import numpy as np
from PIL import Image


def load_gray(path):
    a = np.asarray(Image.open(path).convert("RGB")).astype(np.int32)
    return a


def split_eyes(a):
    h, w, _ = a.shape
    return a[:, : w // 2], a[:, w // 2 :]


def bright_mask(a, hi):
    return a.max(axis=-1) > hi


def dilate(mask, iters):
    out = mask.copy()
    ys, xs = np.where(mask)
    h, w = mask.shape
    for j in range(-iters, iters + 1):
        for i in range(-iters, iters + 1):
            if i == 0 and j == 0:
                continue
            out[np.clip(ys + j, 0, h - 1), np.clip(xs + i, 0, w - 1)] = True
    return out


def best_shift(ref, mov, maxshift):
    """Integer (dx, dy) to apply to mov so it best matches ref (sum-abs)."""
    rg = ref.sum(axis=-1).astype(np.float64)
    mg = mov.sum(axis=-1).astype(np.float64)
    fr = np.fft.rfft2(rg)
    fm = np.fft.rfft2(mg)
    xc = np.fft.irfft2(fr * np.conj(fm), s=rg.shape)
    h, w = rg.shape
    best = (1e300, 0, 0)
    for dy in range(-maxshift, maxshift + 1):
        for dx in range(-maxshift, maxshift + 1):
            v = xc[dy % h, dx % w]
            if v > best[0]:
                best = (v, dx, dy)
    return best[1], best[2]


def roll(a, dx, dy):
    return np.roll(a, (dy, dx), axis=(0, 1))


def analyze_eye(w, f, hi, band, maxshift):
    dx, dy = best_shift(f, w, maxshift)
    wa = roll(w, dx, dy)
    d = np.abs(wa.astype(np.int32) - f.astype(np.int32)).sum(axis=-1)
    wb = bright_mask(wa, hi)
    fb = bright_mask(f, hi)
    near = dilate(fb, band) & ~fb
    halo_m = wb & ~fb & near
    smear_m = wb & ~fb & ~near
    missing_m = fb & ~wb
    n = float(f.shape[0] * f.shape[1])
    return {
        "shift": [dx, dy],
        "resid_mean": round(float(d.mean()), 3),
        "resid_pct30": round(float((d > 30).mean() * 100), 3),
        "halo_px": int(halo_m.sum()),
        "halo_pct": round(float(halo_m.sum() / n * 100), 4),
        "halo_mass": round(float(d[halo_m].sum() / n), 4),
        "smear_px": int(smear_m.sum()),
        "missing_px": int(missing_m.sum()),
        "missing_pct": round(float(missing_m.sum() / n * 100), 4),
        "missing_mass": round(float(d[missing_m].sum() / n), 4),
    }, wa, halo_m, missing_m, d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("warped")
    ap.add_argument("fresh")
    ap.add_argument("--out", default=None)
    ap.add_argument("--maxshift", type=int, default=60)
    ap.add_argument("--hi", type=int, default=40)
    ap.add_argument("--band", type=int, default=4)
    ap.add_argument(
        "--crop",
        default=None,
        help="x,y,w,h crop applied to both images first "
        "(e.g. a static controller region, avoiding animated objects)",
    )
    args = ap.parse_args()

    w = load_gray(args.warped)
    f = load_gray(args.fresh)
    if args.crop:
        x, y, cw, ch = (int(v) for v in args.crop.split(","))
        w = w[y : y + ch, x : x + cw]
        f = f[y : y + ch, x : x + cw]
    if w.shape != f.shape:
        sys.exit(f"size mismatch: {w.shape} vs {f.shape}")
    out = {}
    for tag, (we, fe) in zip(("left", "right"), zip(split_eyes(w), split_eyes(f))):
        m, wa, halo_m, missing_m, d = analyze_eye(
            we, fe, args.hi, args.band, args.maxshift
        )
        out[tag] = m
        if args.out:
            ov = np.zeros_like(we)
            ov[halo_m] = (255, 0, 0)
            ov[missing_m] = (0, 255, 0)
            base = np.clip(fe, 0, 255).astype(np.uint8)
            blend = np.clip(
                base.astype(np.int32) // 2 + ov.astype(np.int32) // 2, 0, 255
            ).astype(np.uint8)
            Image.fromarray(blend).save(f"{args.out}_{tag}.png")
    if args.out:
        dall = np.abs(w.astype(np.int32) - f.astype(np.int32)).sum(axis=-1)
        Image.fromarray(np.clip(dall, 0, 255).astype(np.uint8)).save(
            f"{args.out}_diff.png"
        )
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
