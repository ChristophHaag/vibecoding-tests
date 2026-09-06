# OpenWarp visual-fidelity plan: edges first

Goal: make depth-reprojected edges artifact-free within the data we
already have (color + depth layers, no motion vectors). Success is
measured, not eyeballed: same-pose stale-vs-fresh pairs
(`scripts/capture-stale-burst.py` + hand masks +
`scripts/analyze-warp.py`), sim counts from
`scripts/owsim-quad-cover.py` (bad/fringe/cut/hole), and a ms-budget
check at 90 Hz stereo on the sandbox GPU (AMD Radeon AI PRO R9700,
RDNA4 discrete) for per-frame work.

Scope: this plan covers **fidelity only**. Motion vectors are
deliberately out of scope (see "Motion vectors later" below) — every
item here must stand on its own without them.

Background (not repeated here): the full work log lives in
`docs/openwarp-edge-sharpness.md` (what shipped, sim results, rejected
alternatives, prior-art survey, vendor contract notes); capture
methodology in `docs/openwarp-stale-capture.md`. This document is the
actionable subset: what to do, in what order, with exit gates.

## Where we stand

Shipped: agreement-gated sharp sampling (filtered interiors, exact
texels at depth steps; `sample_source_sharp` in
`monado/src/xrt/auxiliary/render/shaders/openwarp.comp`), 13x13
stretch fill with far-biased ties, one-sided stale-foreground
occlusion reject. Measured residual: a thin (~1 px), sharp, attached
ring from splat footprint quantization — no soft fray, no detached
copies. That ring plus revealed-background holes in motion are the two
edge defect classes this plan attacks.

## Work items (in order)

### 1. Agreement-weighted warp taps — TESTED 2026-09-06, REJECTED

Tried: gaussian agreement weights (`exp(-(log(zt/z_exp)/sigma)^2)`,
sigmas 0.025/0.05/0.10) on the non-occluded disagreement path, all
shapes x six shifts plus a close-range (Z_BG=1.3) variant — matrix in
`owsim-quad-cover.py`, numbers in `openwarp-edge-sharpness.md`. Result:
cut converts to fringe and mass rises wherever it changes anything
(e.g. bar 23 mm: fringe 0→30, mass 24→42.9; close-range diamond 23 mm:
mass 8.1→27.0, fringe 0→30); all sigmas saturate identically. Same
failure family as always-match: `z_exp` inherits the splat's dilation,
so agreement votes foreground on truth-background pixels, and the sigma
axis has no useful middle. No shader change. Do not retry without
coverage data (item 5).

### 2. Low-res depth experiment — days

Downsample app depth 2x/4x before the splat; A/B with
`analyze-warp.py`. Meta's no-loss claim (PTW runs on 368x400 depth)
predicts pure bandwidth win on splat + warp. If quality holds, the
freed budget funds items 3–4. Gate: zero metric regression on edge
crops. Files: `openwarp_splat.comp`, warp dispatch in
`monado/src/xrt/auxiliary/render/shaders/`,
`compositor/main/comp_renderer.c`.

### 3. Depth-convention test matrix — DONE 2026-09-06

`scripts/owdepth-conventions.py` ports all three linearization copies
(`openwarp_splat.comp` + `openwarp_mesh.vert`
`convert_app_depth_to_warp_ndc`, `openwarp.comp` `app_depth_to_eye_z`)
in float32 and checks 400 near/far/min/max/depthIsGL combos x 10 stored
values: finite everywhere, mirrors cross-consistent, analytically exact
on valid standard combos, monotonic with the correct sign. One real
finding: reversed-Z buffers are NOT handled (prior "handles reversed"
note was wrong — `f<=n` only avoids a crash). Reported-normal reversed
inverts end to end; reported-swapped (`nearZ>farZ`) explodes
(`owdepth-conventions.py` pins both). No shader-behavior change without
a live reversed source to validate against; instead both warp UBO fill
sites (`comp_renderer.c`, `comp_render_gfx.c`) now warn once on the
reversed signal (`0 < far_z < near_z`) so a future submission fails
loudly instead of as mystery warp artifacts.

### 4. Coverage-hole debug view — DONE 2026-09-06

New `RENDER_OPENWARP_DEBUG_SHOW_HOLES` bit (1u<<8) + `DEBUG_SHOW_HOLES`
in `openwarp.comp`: pixels with no valid warp depth — even after the
stretch search — paint solid red, distinct from stretch-filled (green)
and occlusion rejects (magenta); `DEPTH_AS_COLOR` behavior unchanged.
Plumbed end to end: `render_interface.h` define, `comp_compositor.h`
`show_holes`, `MONADO_OPENWARP_SHOW_HOLES` env default, GUI checkbox
("Highlight coverage holes (red)"), `comp_renderer.c` flag packing.
Verified: full build clean, shader glslangValidator-clean, live
service + playground FOCUSED with `SHOW_HOLES=1` (+`DISABLE_STRETCH=1`
to force the new branch widely taken) under validation layers — 0
validation errors. Pixel-level confirmation pending sandbox capture
repair; until then judge holes-vs-filled with this view + the
`DEPTH_AS_COLOR` magenta cross-check.

### 5. Coverage channel from MSAA depth — weeks

The principled fix for the residual ring: the ring is foreground
winning up to half a pixel it only partly covers (splat quantizes
footprints to whole pixels + atomicMin). Per-pixel coverage from the
app's MSAA depth resolves what no local rule can (magnified
foreground vs true background are indistinguishable at 1x). Requires
app MSAA depth submission + splat changes; biggest payoff, biggest
cost. Gate: ring mass → ~0 with cut staying 0.

### 6. Background-layer fill for wide disocclusions — weeks

Second depth/color pair + fill pass (LDI/MPI-style, background-only).
The only proposal that *removes* revealed-background holes instead of
guessing them; in live motion these are the most visible edge defect
after the ring. Defer unless captures show holes wider than the 13x13
window dominating (they don't today) — keep as the standing answer,
not active work. Pull-push pyramid stays parked behind it for the same
reason.

### 7. Settle accounting + FLIP migration — ongoing tooling

1spp-style frames-to-detail metric for the stale-window tooling
("converges in N frames" instead of ad-hoc sleeps); migrate
`analyze-warp.py` abs-err counts to FLIP maps + mean. Makes every
item above judgeable. Files: `scripts/`.

## Explicit non-goals (tested or scoped out)

- Far-side-nearest sampling: live-regressed, reverted; literature
  agrees (soft beats hard at uncertain boundaries).
- Shrinking splat footprints: sim-traded ring for cut holes.
- Neural inpainting: needs G-buffers/MVs/extra frames we don't get.
- Frame-generation SDKs (FSR/DLSS/XeSS): app-side interpolation,
  wrong temporal direction and wrong pipeline stage for a compositor.

## Motion vectors later (do not block)

MV integration (`XR_EXT_frame_synthesis`: MV-advance dynamic pixels +
existing depth warp, MV → depth → plane fallback) is the planned
answer for *moving-object* edges, which nothing above can fix. To keep
that path open while doing this plan:

- Keep submitted depth buffers available to the compositor at a
  synthesis-friendly resolution (item 2's downsampler should be a
  reusable stage, not warp-inlined).
- Keep the pass structure composable: warp output must remain a valid
  *input* to a future synthesis pass (linear intermediate, per-view,
  undistorted rectilinear — as today), never baked with distortion.
- Keep debug infrastructure extensible: item 4's views plus a spec'd
  (not yet built) zero-centered MV view, so MV trust has somewhere to
  surface on day one.
- When touching IPC/UBO layouts for items above, prefer additive
  changes; don't bake in "depth is the only auxiliary input."
- The MV trust check (camera-component of submitted MVs vs tracked
  pose delta → depth-only fallback) is already designed; it executes
  against whatever warp items 1–6 produce.

Suggested sequencing: 1 → 3 → 4 → 2 (each gates the next; 2 funds
5/6) → 7 alongside → 5 when MSAA-depth submission is realistic → 6 on
evidence → MV track when an engine client (e.g. Godot frame
synthesis) is in hand to validate against.
