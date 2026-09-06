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

### 2. Low-res depth experiment — TRIAGED 2026-09-06, live A/B pending capture repair

Sim triage in `owsim-quad-cover.py` (`item2` block: splat + reproject
from downsampled depth, all shapes x 8/23/57 mm): box-filter
downsampling is catastrophic everywhere (fringe/cut/holes explode —
averaging across steps fabricates neither-surfaces; killed as a
variant). Min-downsample full-pipeline keeps hard counters but shifts
mass both ways and regresses thin geometry (bar 8 mm: bad 0→60, mass
13→24) — Meta's no-loss claim does not transfer directly, because our
agreement/occlusion edge machinery reads the same buffer. The
splat-only split (2x-min splat, full-res reproject taps) is the standout:
fringe+mass down at larger shifts on every shape (bar 57 mm: fringe
52→0, mass 76→28; diamond 57 mm: mass 77→59; disc 57 mm: mass 69→58),
cut never moves, small-shift bad-count churn at ~flat mass. Mechanism
for the large-shift fringe wins is not fully understood — no shader
change until a live same-pose A/B (`analyze-warp.py`) confirms. If
pursued live, the shape is fixed: keep full-res depth for the
reproject taps, downsample only the splat input (splat atomics are the
bandwidth to win). Constraint from the MV track stands: build the
downsampler as a reusable stage, not warp-inlined.

### 3. Depth-convention test matrix — DONE 2026-09-06

`scripts/owdepth-conventions.py` ports all three linearization copies
(`openwarp_splat.comp` + `openwarp_mesh.vert`
`convert_app_depth_to_warp_ndc`, `openwarp.comp` `app_depth_to_eye_z`)
in float32 and checks 400 near/far/min/max/depthIsGL combos x 10 stored
values: finite everywhere, mirrors cross-consistent, analytically exact
on valid standard AND finite-reversed combos, monotonic with the
orientation-correct sign. Reversed-Z fix 2026-09-06: the old
`!(f > n)` test routed finite reversed (`0 < far < near`) into the
infinite-far branch, exploding near geometry to infinity — the finite
perspective inverse is correct for both orderings, so the branch is now
infinite iff far is <= 0 / infinite / NaN (all three shader copies).
Reported-swapped (`nearZ>farZ`, e.g. live 4000/0.05) linearizes exactly;
a reversed buffer reported in normal order contradicts its own depths
and stays unfixable by definition. The warn-once guards added earlier
at both warp UBO fill sites are removed — reversed is a supported case
now, not a hazard.

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

### 7. Settle accounting + FLIP migration — FLIP half DONE 2026-09-06

`analyze-warp.py` gained `--flip`: each FFT-aligned eye pair is scored
with the FLIP evaluator (reference fresh, test warped, LDR, default 67
PPD recorded in the JSON) as `flip_mean`/`flip_p99`/`flip_pct01` plus a
magma `<out>_<eye>_flip.png` map — additive, all existing abs-err
outputs bit-identical without the flag. Verified on the
`repro_double_*` same-pose pair: deterministic, 0.0 on identical inputs
(no dark-scene floor), nonzero on artifact crops. Remaining: settle
accounting (frames-to-detail metric for the burst tooling).

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
