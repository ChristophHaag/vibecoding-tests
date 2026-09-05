#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Closed-loop stale-reprojection hunter for the openwarp warp path.

Alternates two absolute head poses over ONE persistent remote-client
connection while capturing the Monado mirror densely via xwd-on-stdout
(~7 ms/shot) with in-process numpy scoring. Stops on the first
hand-blackout hit: green-hand pixels collapse while the pink hand stays
in view (stale source reprojected at the new warp pose).

Requires a live session (service-up.sh + playground-up.sh, default
50 ms pacing) settled at some pose with both hands in view; baselines
there, then jumps between the two given poses. Each send is itself a
fresh teleport, so every round re-arms the stale window (~50 ms at
default pacing; pose delivery itself is ~0.15 s, not seconds).

Usage:
    python3 scripts/capture-stale-hunt.py \
        Ax Ay Az Aw Bx By Bz Bw [--rounds N] [--out DIR]

Poses are absolute "head" quaternions (position fixed at 0 1.7 0).
Prints `HIT <png> dt=<s> elapsed=<s>` plus a settled fresh frame at the
same pose (`FRESH <png>`), or `MISS ...` with the best near-miss.
Exit 0 on hit, 1 otherwise.
"""
import argparse
import os
import select
import struct
import subprocess
import sys
import time

import numpy as np
from PIL import Image

GEO = None


def window_size():
    """Query the Monado mirror geometry once (raw rgb:- has no header)."""
    global GEO
    if GEO is None:
        out = subprocess.run(["xwininfo", "-name", "Monado"],
                             capture_output=True, text=True).stdout
        w = int([l for l in out.splitlines() if "Width:" in l][0].split()[-1])
        h = int([l for l in out.splitlines() if "Height:" in l][0].split()[-1])
        GEO = (w, h)
    return GEO


def xwd_rgb():
    """Grab the Monado mirror window, return (R, G, B) int arrays."""
    # NOTE: plain XGetImage (xwd, scrot, xwd -root) fails server-wide here
    # (BadMatch); MIT-SHM based `import ... rgb:-` works at ~25 ms/shot.
    w, h = window_size()
    p = subprocess.run(["import", "-window", "Monado", "-depth", "8", "rgb:-"],
                       capture_output=True)
    d = p.stdout
    exp = w * h * 3
    if len(d) < exp:
        raise RuntimeError("import grab failed (is the Monado window up?)")
    px = np.frombuffer(d, dtype=np.uint8, count=exp).reshape(h, w, 3)
    return px[:, :, 0].astype(int), px[:, :, 1].astype(int), px[:, :, 2].astype(int)


def score(rgb):
    r, g, b = rgb
    green = int(((g > 200) & (r < 180) & (b < 180)).sum())
    pink = int(((r > 200) & (g > 80) & (g < 180) & (b > 80) & (b < 180)).sum())
    return green, pink


def save(rgb, path):
    arr = np.stack([rgb[0], rgb[1], rgb[2]], axis=-1).astype(np.uint8)
    Image.fromarray(arr).save(path)


class RC:
    """One persistent remote-client connection (avoids per-send reconnects)."""

    def __init__(self):
        self.p = subprocess.Popen(
            ["remote-driver-client/build/monado-remote-client"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, bufsize=1)

    def cmd(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        ready, _, _ = select.select([self.p.stdout], [], [], 5.0)
        if not ready:
            raise RuntimeError(f"remote client did not answer '{line}' "
                               f"(is monado-service up with remote config?)")
        return self.p.stdout.readline().strip()

    def head(self, q):
        if self.cmd("head 0 1.7 0 %f %f %f %f" % q) != "ok":
            raise RuntimeError("remote client rejected head pose")
        if self.cmd("send") != "ok":
            raise RuntimeError("remote client failed send")

    def close(self):
        try:
            self.p.stdin.write("quit\n")
            self.p.stdin.flush()
        except BrokenPipeError:
            pass
        self.p.wait(timeout=5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("quats", nargs=8, type=float,
                    help="Ax Ay Az Aw Bx By Bz Bw (absolute head quats)")
    ap.add_argument("--rounds", type=int, default=12)
    ap.add_argument("--out", default="/tmp",
                    help="directory for hit/fresh/best PNGs")
    args = ap.parse_args()
    a = tuple(args.quats[0:4])
    b = tuple(args.quats[4:8])
    os.makedirs(args.out, exist_ok=True)

    # Precondition: mirror must be grabbable (service + app + window).
    try:
        rgb0 = xwd_rgb()
    except RuntimeError as e:
        print(f"error: {e}", flush=True)
        return 1
    g0 = np.median([score(xwd_rgb())[0] for _ in range(10)])
    p0 = np.median([score(xwd_rgb())[1] for _ in range(10)])
    print(f"baseline green={g0:.0f} pink={p0:.0f}", flush=True)

    rc = RC()
    best = None  # (green, path, info)
    t_start = time.time()
    try:
        for rnd in range(args.rounds):
            q, name = (a, "A") if rnd % 2 == 0 else (b, "B")
            rc.head(q)
            t_send = time.time()
            while time.time() - t_send < 1.5:
                rgb = xwd_rgb()
                g, p = score(rgb)
                dt = time.time() - t_send
                if best is None or g < best[0]:
                    path = os.path.join(args.out, "stale_best.png")
                    save(rgb, path)
                    best = (g, path, f"rnd={rnd}{name} dt={dt:.2f}s g={g} p={p}")
                if g < 0.5 * g0 and (g + p) > 0.25 * (g0 + p0):
                    hit = os.path.join(args.out, f"stale_hit_r{rnd}{name}.png")
                    save(rgb, hit)
                    print(f"HIT {hit} dt={dt:.2f}s g={g} p={p} "
                          f"elapsed={time.time() - t_start:.1f}s", flush=True)
                    # Settle, then capture the fresh frame at the same pose.
                    time.sleep(5)
                    save(xwd_rgb(), os.path.join(args.out, "stale_fresh.png"))
                    print(f"FRESH {os.path.join(args.out, 'stale_fresh.png')}",
                          flush=True)
                    return 0
            print(f"round {rnd}{name} done, best-g so far: {best[0]}", flush=True)
    finally:
        rc.close()
    print(f"MISS elapsed={time.time() - t_start:.1f}s best: {best[2]} {best[1]}",
          flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
