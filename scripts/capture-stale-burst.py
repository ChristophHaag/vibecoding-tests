#!/usr/bin/env python3
"""Capture stale burst + settled fresh for openwarp edge-fray analysis.

For each A->B jump: send pose B over a persistent remote-client connection,
grab the mirror densely (~30ms) for 2s, then sleep 4s (cube spins 0.25rot/s
-> 360 degrees, same orientation) and grab the settled fresh frame.
Saves burst_*.rgb + fresh.rgb per jump dir. Poses are absolute
(X + quaternion at height 1.7 — teleports, exactly repeatable across
runs/builds), so both rotation and translation jumps work.
Usage: python3 scripts/capture-stale-burst.py OUTDIR Ax AQx AQy AQz AQw Bx BQx BQy BQz BQw
"""
import os
import select
import subprocess
import sys
import time


def grab():
    p = subprocess.run(["import", "-window", "Monado", "-depth", "8", "rgb:-"],
                       capture_output=True)
    return p.stdout


class RC:
    def __init__(self):
        self.p = subprocess.Popen(
            ["remote-driver-client/build/monado-remote-client"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, bufsize=1)

    def cmd(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        ready, _, _ = select.select([self.p.stdout], [], [], 10.0)
        if not ready:
            raise RuntimeError(f"no answer to '{line}'")
        return self.p.stdout.readline().strip()

    def close(self):
        try:
            self.p.stdin.write("quit\n")
            self.p.stdin.flush()
        except BrokenPipeError:
            pass
        self.p.wait(timeout=5)


def main():
    outdir = sys.argv[1]
    ax, aqx, aqy, aqz, aqw = map(float, sys.argv[2:7])
    bx, bqx, bqy, bqz, bqw = map(float, sys.argv[7:12])
    a = (ax, aqx, aqy, aqz, aqw)
    b = (bx, bqx, bqy, bqz, bqw)
    os.makedirs(outdir, exist_ok=True)
    rc = RC()
    try:
        for name, qfrom, qto in [("AtoB", a, b), ("BtoA", b, a)]:
            rc.cmd("head %f 1.7 0 %f %f %f %f" % qfrom)
            assert rc.cmd("send") == "ok"
            time.sleep(6)  # settle at start pose
            rc.cmd("head %f 1.7 0 %f %f %f %f" % qto)
            assert rc.cmd("send") == "ok"
            t0 = time.time()
            n = 0
            while time.time() - t0 < 2.0:
                with open(f"{outdir}/{name}_burst{n:02d}.rgb", "wb") as f:
                    f.write(grab())
                n += 1
            time.sleep(4.0)  # settle: cube back to same orientation
            with open(f"{outdir}/{name}_fresh.rgb", "wb") as f:
                f.write(grab())
            print(f"{name}: {n} burst frames + fresh", flush=True)
    finally:
        rc.close()


main()
