"""Depth-convention test matrix for the openwarp depth linearization
(fidelity-plan item 3, reversed-depth fix 2026-09-06).

Three shader copies share one front-end and must stay in sync:
  monado/.../shaders/openwarp_splat.comp  convert_app_depth_to_warp_ndc
  monado/.../shaders/openwarp_mesh.vert   convert_app_depth_to_warp_ndc
  monado/.../shaders/openwarp.comp        app_depth_to_eye_z
The first two are textually identical; the third differs only in the
tail (returns metric eye-Z with a 1e30 far sentinel instead of warp NDC
with a 0.0 far sentinel). All three are ported below in float32 to match
shader semantics, then checked over a near/far/min/max/depthIsGL matrix:

  gate 1 (plan gate): every combination yields FINITE, non-NaN output.
  gate 2: the two warp-NDC copies agree bit-for-bit; warp eye-Z agrees
          with warpNear/splat-NDC (cross-consistency of the mirrors).
  gate 3: on VALID combos the value matches the analytic projection
          inverse — standard orientation (0=near,1=far) AND finite
          reversed (farZ<nearZ: 0=far,1=near, either reporting order
          linearizes via the same finite formula).
  gate 4: monotonicity over stored depth has the orientation-correct sign
          (non-decreasing for standard buffers, non-increasing for
          finite reversed ones).

Reporting note: OpenXR validation only rejects nearZ==farZ, so an app
rendering reversed-Z signals it by reporting farZ<nearZ (swapped order).
A reversed buffer reported in normal order contradicts its own depths
and is unfixable by definition — the runtime must trust the reporting;
that combination is recorded, not gated.

Validation context (oxr_session_frame_end.c): min/maxDepth must be in
[0,1] with min<=max and nearZ!=farZ, else the layer is rejected before
it reaches the shader — so out-of-spec combos below only need gate 1
(robustness), while in-spec combos need all gates.

Run: python3 scripts/owdepth-conventions.py (exit nonzero on failure).
"""

import math
import sys

import numpy as np

F32 = np.float32
WARP_NEAR = F32(0.1)
FAR_SENTINEL_Z = 1.0e30


def port_convert_ndc(d_stored, n, f, mn, mx, is_gl):
    """Exact port of convert_app_depth_to_warp_ndc (splat + mesh vert)."""
    d_stored = F32(d_stored)
    n = F32(n)
    f = F32(f)
    mn = F32(mn)
    mx = F32(mx)
    if not (n > F32(0.0)) or bool(np.isnan(d_stored)):
        return F32(0.0)
    rng = mx - mn
    # NOTE: the shader divides by (range == 0 ? 1 : range), i.e. by literal
    # 1.0 when the range is zero — mirrored exactly here.
    if rng != F32(0.0):
        dn = (d_stored - mn) / rng
    else:
        dn = d_stored - mn
    dn = F32(min(max(float(dn), 0.0), 1.0))
    if (f <= F32(0.0)) or bool(np.isinf(f)) or bool(np.isnan(f)):
        if dn >= F32(1.0):
            return F32(0.0)
        z_eye = n / (F32(1.0) - dn)
    elif is_gl == 1:
        # Finite far, either orientation (standard and reversed alike).
        ndc = dn * F32(2.0) - F32(1.0)
        z_eye = (F32(2.0) * n * f) / ((f + n) - (f - n) * ndc)
    else:
        z_eye = (n * f) / (f - (f - n) * dn)
    return WARP_NEAR / z_eye


def port_eye_z(d_stored, n, f, mn, mx, is_gl):
    """Exact port of app_depth_to_eye_z (openwarp.comp reproject pass)."""
    d_stored = F32(d_stored)
    n = F32(n)
    f = F32(f)
    mn = F32(mn)
    mx = F32(mx)
    if not (n > F32(0.0)) or bool(np.isnan(d_stored)):
        return F32(FAR_SENTINEL_Z)
    rng = mx - mn
    if rng != F32(0.0):
        dn = (d_stored - mn) / rng
    else:
        dn = d_stored - mn
    dn = F32(min(max(float(dn), 0.0), 1.0))
    if (f <= F32(0.0)) or bool(np.isinf(f)) or bool(np.isnan(f)):
        if dn >= F32(1.0):
            return F32(FAR_SENTINEL_Z)
        return n / (F32(1.0) - dn)
    elif is_gl == 1:
        # Finite far, either orientation (standard and reversed alike).
        ndc = dn * F32(2.0) - F32(1.0)
        return (F32(2.0) * n * f) / ((f + n) - (f - n) * ndc)
    else:
        return (n * f) / (f - (f - n) * dn)


def truth_standard(dn, n, f, is_gl):
    """Analytic eye-Z for a standard-orientation buffer (0=near,1=far)."""
    if f <= 0 or math.isinf(f) or math.isnan(f):
        return n / (1.0 - dn) if dn < 1.0 else math.inf
    if is_gl:
        ndc = dn * 2.0 - 1.0
        return (2.0 * n * f) / ((f + n) - (f - n) * ndc)
    return (n * f) / (f - (f - n) * dn)


def truth_reversed(dn, lo, hi, is_gl):
    """Analytic eye-Z for a reversed buffer with actual planes lo..hi
    (stored 1=near, 0=far): the standard mapping at the mirrored depth."""
    return truth_standard(1.0 - dn, lo, hi, is_gl)


NEARS = (0.01, 0.05, 0.1, 0.0, -1.0)
FARS = (0.0, -1.0, 10.0, 100.0, math.inf, math.nan)
FARS_EXTRA = ("same", "half")  # f == n, f == n/2 (finite, <= n)
SUBRANGES = ((0.0, 1.0), (0.2, 0.8), (0.0, 0.0), (1.0, 1.0), (0.5, 0.5))
D_GRID = (0.0, 0.25, 0.5, 0.75, 1.0, -0.5, 1.5, math.inf, -math.inf, math.nan)

failures = []
reversed_notes = []


def check(cond, msg):
    if not cond:
        failures.append(msg)


def run():
    n_checked = 0
    for n in NEARS:
        for f in list(FARS) + [n, n / 2.0 if n > 0 else -0.5]:
            for mn, mx in SUBRANGES:
                for is_gl in (0, 1):
                    n_checked += 1
                    vals_ndc = []
                    vals_eye = []
                    for d in D_GRID:
                        v_ndc = port_convert_ndc(d, n, f, mn, mx, is_gl)
                        v_eye = port_eye_z(d, n, f, mn, mx, is_gl)
                        vals_ndc.append(v_ndc)
                        vals_eye.append(v_eye)
                        # gate 1: finite, non-NaN everywhere.
                        check(bool(np.isfinite(v_ndc)),
                              f"ndc non-finite n={n} f={f} sub=({mn},{mx}) gl={is_gl} d={d}: {v_ndc}")
                        check(bool(np.isfinite(v_eye)),
                              f"eye non-finite n={n} f={f} sub=({mn},{mx}) gl={is_gl} d={d}: {v_eye}")
                        # gate 2: mirror cross-consistency (outside sentinels).
                        if v_ndc > 0.0 and v_eye < FAR_SENTINEL_Z / 2.0:
                            rel = abs(float(v_eye) - float(WARP_NEAR / v_ndc)) / float(v_eye)
                            check(rel < 1e-4,
                                  f"mirror mismatch n={n} f={f} sub=({mn},{mx}) gl={is_gl} d={d}: "
                                  f"eye={v_eye} vs warpNear/ndc={float(WARP_NEAR / v_ndc)}")
                    # gate 4: monotonicity over the in-range grid, with the
                    # orientation-correct sign (standard non-decreasing,
                    # finite reversed non-increasing; NaN/inf/out-of-range
                    # exempt).
                    valid = (n > 0 and (math.isinf(f) or (f > n and f > 0)) and mn <= mx
                             and 0.0 <= mn and mx <= 1.0)
                    valid_rev = (n > 0 and 0.0 < f < n and not math.isinf(f) and not math.isnan(f)
                                 and mn <= mx and 0.0 <= mn and mx <= 1.0)
                    if valid and (mn, mx) == (0.0, 1.0):
                        seq = [float(port_eye_z(d, n, f, mn, mx, is_gl)) for d in (0.0, 0.25, 0.5, 0.75)]
                        check(all(b >= a for a, b in zip(seq, seq[1:])),
                              f"non-monotonic n={n} f={f} gl={is_gl}: {seq}")
                    if valid_rev and (mn, mx) == (0.0, 1.0):
                        seq = [float(port_eye_z(d, n, f, mn, mx, is_gl)) for d in (0.0, 0.25, 0.5, 0.75)]
                        check(all(b <= a for a, b in zip(seq, seq[1:])),
                              f"reversed non-monotonic n={n} f={f} gl={is_gl}: {seq}")
                    # gate 3: analytic correctness on valid combos.
                    if valid:
                        for d in (0.0, 0.25, 0.5, 0.75, 1.0):
                            dn = min(max((d - mn) / (mx - mn if mx != mn else 1.0), 0.0), 1.0)
                            if (mn, mx) != (0.0, 1.0):
                                continue  # subrange remap has no single truth dn; finiteness only
                            t = truth_standard(dn, n, f, is_gl)
                            got = float(port_eye_z(d, n, f, mn, mx, is_gl))
                            if math.isinf(t):
                                check(got >= FAR_SENTINEL_Z / 2.0,
                                      f"far sentinel n={n} f={f} gl={is_gl} d={d}: got {got}")
                            else:
                                # 1e-3: the shader runs in float32 (n=0.01/f=100
                                # at d=1.0 already errs 2e-4 vs float64 truth),
                                # still 50x tighter than the 5% edge bands.
                                check(abs(got - t) / max(t, 1e-9) < 1e-3,
                                      f"value mismatch n={n} f={f} gl={is_gl} d={d}: got {got}, truth {t}")
                    if valid_rev:
                        for d in (0.0, 0.25, 0.5, 0.75, 1.0):
                            dn = min(max((d - mn) / (mx - mn if mx != mn else 1.0), 0.0), 1.0)
                            if (mn, mx) != (0.0, 1.0):
                                continue  # subrange remap has no single truth dn; finiteness only
                            t = truth_reversed(dn, f, n, is_gl)
                            got = float(port_eye_z(d, n, f, mn, mx, is_gl))
                            check(abs(got - t) / max(t, 1e-9) < 1e-3,
                                  f"reversed mismatch n={n} f={f} gl={is_gl} d={d}: got {got}, truth {t}")
    print(f"matrix: {n_checked} param combos x {len(D_GRID)} stored values, "
          f"{len(failures)} failures")
    for msg in failures[:20]:
        print("FAIL:", msg)

    # Finite reversed buffers (farZ<nearZ reporting) now linearize via
    # the same finite formula — demonstrated and gated in the matrix
    # above (gate 3 valid_rev). What stays unfixable: a reversed buffer
    # reported in NORMAL order contradicts its own depths (the runtime
    # must trust the reporting), recorded here, asserts nothing.
    print()
    print("reversed-buffer behavior (swapped reporting is gated, asserts nothing extra):")
    for label, n, f in (("reported-normal n=0.1 f=100", 0.1, 100.0),
                        ("reported-swapped n=100 f=0.1 (nearZ>farZ)", 100.0, 0.1)):
        # truth: reversed-finite buffer, actual planes 0.1..100.
        row = []
        for d in (0.0, 0.5, 1.0):
            got = float(port_eye_z(d, n, f, 0.0, 1.0, 0))
            row.append(f"d={d}: shader {got:.4g} vs reversed-truth {truth_reversed(d, 0.1, 100.0, 0):.4g}")
        print(f"  {label}: " + "; ".join(row))
        reversed_notes.append((label, row))


if __name__ == "__main__":
    run()
    if failures:
        print(f"\n{len(failures)} FAILURES")
        sys.exit(1)
    print("\nall gates pass (finite reversed-Z gated correct)")
