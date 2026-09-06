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

### 1. Agreement-weighted warp taps — days

Replace the binary accept in the 3x3 warp neighborhood with
depth-agreement weights (ExtraSS bilateral warp in miniature). Best
near-term chance of reducing nicks without reintroducing halo.
Sim-first in `owsim-quad-cover.py`, then live A/B on identical
teleports. Gate: bad/fringe/cut counts vs the current zero-fringe
baseline; any halo regression kills it (cf. far-side-nearest revert).
Files: `openwarp.comp` (`sample_source_sharp`), sim script.

### 2. Low-res depth experiment — days

Downsample app depth 2x/4x before the splat; A/B with
`analyze-warp.py`. Meta's no-loss claim (PTW runs on 368x400 depth)
predicts pure bandwidth win on splat + warp. If quality holds, the
freed budget funds items 3–4. Gate: zero metric regression on edge
crops. Files: `openwarp_splat.comp`, warp dispatch in
`monado/src/xrt/auxiliary/render/shaders/`,
`compositor/main/comp_renderer.c`.

### 3. Depth-convention test matrix — days

`app_depth_to_eye_z` (`openwarp.comp:158`, mirrored in the splat pass)
handles reversed / infinite-far / GL-clip / subrange / NaN by reading,
with no dedicated coverage. Extend the sim over
near/far/min/max/depthIsGL combinations. Wrong linearization is an
edge factory (misplaced silhouettes); this closes the class cheaply.
Gate: all combinations produce finite, monotonic eye-Z.

### 4. Coverage-hole debug view — days

Split "no data" from "filled" in one glance: visualize invalid
warpdepth distinctly from stretched pixels (`DEBUG_SHOW_STRETCH`
shows the latter; holes are currently only visible as magenta under
`DEBUG_DEPTH_AS_COLOR`). Needed to judge items 5–6 honestly. Files:
`openwarp.comp:350-671`, `render_interface.h`
(`RENDER_OPENWARP_DEBUG_*`), debug GUI panel.

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
