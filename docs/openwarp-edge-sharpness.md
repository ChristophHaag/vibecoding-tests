# OpenWarp edge sharpness: optimization work log

Reprojection fray at depth silhouettes, in the compute openwarp path
(`openwarp.comp`, second pass). Covers the shipped fix, every alternative
that was tried and rejected (with numbers), the corrected live
measurement, and what is left. For *how to capture* stale frames, see
`docs/openwarp-stale-capture.md`. For the pipeline itself, see
`monado/doc/openwarp_integration.md` §6.

## Problem

Per warp pixel the depth is exact (nearest-wins splat), but the source
color was fetched with bilinear `textureLod`, whose 2x2 kernel straddles
silhouettes within ~0.5 texel of every source depth edge: bright
foreground bleed onto background, dark background nicks on foreground —
a soft 1–2 px frayed band that slides with the head. The occlusion depth
read had the same flaw (filtered depth is neither surface at an edge),
wobbling the verdict within 1 px of silhouettes.

## What shipped: agreement-gated sharp sampling

`sample_source_sharp()` in
`monado/src/xrt/auxiliary/render/shaders/openwarp.comp` (shader-only
change — no UBO, descriptor, or pipeline-layout changes):

- Fetch exact source depths (`texelFetch`) for the 2x2 taps around the UV
  plus the tap nearest the UV; compare against the point's own
  render-view eye distance (through `renderV`, never the warp depth —
  the render and warp eyes sit centimetres apart and decompose the same
  point into different planar depths).
- **Agreement** (UV-side tap within a two-sided 5% band of expected):
  the UV addresses the right surface — sample sharp (bilinear stays
  where the whole 2x2 agrees, so interiors keep full filtering including
  minification; exact nearest texel at a step). The agreement is the
  visibility proof, so no occlusion test runs there — this also removes
  false black nicks from filtered depth at true edges.
- **Otherwise the legacy path runs bit-identically**: filtered-depth
  occlusion test, bilinear color (nearest texel for stretched fill).

Perf cost: 5 exact depth fetches + conversions per pixel (4 taps + ref),
then the same single color tap as before. No extra images, atomics, or
passes. Only disagreement-visible pixels (a few % at edges) do any extra
work beyond the fetches.

## Simulation results

`scripts/owsim-edge-sharp.py` (bg wall z=5 Minh Tran 0.1, fg quad z=1 Minh Tran 1.0,
lateral head steps, ground truth = scene rasterized at the warp pose):

```
t      | old: bad fringe cut holes mass  | final: bad fringe cut holes mass
  8.0mm | old:   30    0   0    0   18.4 | fin:    0    0   0    0   13.0
 23.0mm | old:   60    0   0   0   32.2 | fin:   30    0   0    0   24.1
 50.0mm | old:    0    0   0    0   22.0 | fin:    0    0    0    0   22.0
 57.0mm | old:   30    0   0    0   43.1 | fin:    0    0   0    0   35.0
130.0mm | old:    0    0   0    0   63.0 | fin:    0    0    0    0   60.0
150.0mm | old:    0    0   0    0   66.0 | fin:    0    0    0    0   66.0
```

`bad` = abs err > 0.1, `fringe` = bg-truth painted fg, `cut` =
fg-truth painted bg. Note the old code's errors are soft partial mixes
(under the fringe threshold); the fix removes them rather than hardening
them. Remaining mass at large shifts is honest-black disocclusion,
identical in both.

## Rejected alternatives (all sim-tested, all regressed)

`scripts/owsim-erode.py` (same scene + 13x13 closest-valid stretch fill):

| Variant | Result | Verdict |
|---|---|---|
| Always sample depth-matching tap | Hardened the splat's half-texel dilation ring into a full-brightness halo; worse mass at every shift | rejected |
| Always sample nearest tap | Broke disoccluded-bg pixels whose UV lands on the occluder (needs the filtered-depth verdict there) | rejected |
| Disagreement 50/50 blend (nearest + matching) | Worse mass at every shift on dark bg — position-weighted bilinear is already the best static estimate without coverage data | rejected |
| Directional splat erosion (inset edge-side quad corners) | inset 0.25 changes nothing (floor() quantization); inset 0.5–1.0 punches cut holes that stretch's farthest-tie fills with background. Erosion is a halo↔cut slider, not a fix | rejected |
| Near+far warpdepth buffers + blend | Same 50/50 coverage guess as the blend above, plus an image, atomics, and layout churn | rejected without implementing |

Lesson: without coverage data, position-weighted bilinear is the optimal
static estimate; the only winnable band is the true-edge agreement case.

## Live measurement (corrected)

An earlier round of live "edge" readings turned out to be fresh identity
frames (burst10+, app re-renders in ~40–60 ms) — retracted. Genuine stale
frames are only burst01–02 post-transition; see
`docs/openwarp-stale-capture.md` for identification.

One 15 cm lateral stale pair (playground hands, cube masked,
`tint` run for attribution), current shader:

- 297/8947 hand px differ — **all darker** (42 black + dim mixes),
  **zero bright halo**, zero occlusion highlights (not the occlusion
  check), zero cut-through copies.
- Settled frames: hand regions bit-identical across 5 s-spaced shots
  (no shimmer); settled silhouette edges achromatic (no lens-CA fringe —
  R/G/B track each other).
- Validation-clean (0 errors, 0 warnings); `DEPTH_AS_COLOR` proves the
  pass dispatches; head moves reproject with the scene intact.

Pending: old-vs-new live A/B on identical teleports (mirror capture
broke sandbox-wide on 2026-09-06 — see stale-capture doc).

## Residual and limits

What remains is binary coverage flips at silhouettes: the splat
quantizes each texel's warp footprint to whole pixels, so boundary pixels
flip fg↔bg between the stale and fresh sampling grids. Irreducible at 1x
depth resolution (pixels whose UV itself sits on foreground are
self-consistent and indistinguishable from magnified foreground). On the
playground's black background these read as dark edge bites (~3% of hand
px for one frame after a violent 15 cm teleport; far less per frame in
smooth motion). Supersampled warpdepth would fix it at ~4x splat cost —
rejected per the no-huge-perf-impact constraint.

## 2026-09-06 follow-up: cube-edge protrusions, attribution, one tried fix

Prompt: frayed edges still visible on the playground cube (bottom-left edge
tabs); fix must generalize past straight edges (curved/thin geometry too).

### Live attribution (identical 15 cm lateral teleports, hands, cube masked)

Three headless runs on absolute poses (A=0, B=0.15 m), stale = burst01-02
(hand-diff hundreds), settled = ~0 — same regime as before:

- Occlusion rejects: **zero** in both directions (`SHOW_OCCLUSION` tints
  nothing on hands). No false positives; nothing to tune there.
- Stretch fills: modest (`SHOW_STRETCH`: 23–114 hand px per pair).
- Dominant mass = black nicks (AtoB 97–122, BtoA 546 hand px, 345 of them
  ≥2 px deep). `DEPTH_AS_COLOR` at the nick coords: 0/546 invalid-magenta
  (all valid depth), mean color (42, 77, 175) vs hand interior (60, 128,
  238) vs bg (38, 64, 159) — i.e. background won the splat there.
  Foreground coverage gaps, not holes, not occlusion verdicts.
- Edge profiles are achromatic sharp 1 px steps (R/G/B track; no lens-CA
  component). Full-bright tabs are rare (hands ≥2 px out: 0–2 px);
  the visible tabs are dim grayish mixes next to saturated faces.

### Simulation (`scripts/owsim-quad-cover.py`, new)

Diamond (diagonal edges) + disc (curved) + thin-bar (finger-like) fg on a
bg wall, bbox vs center-in-quad test vs node-minimum corner depths vs
wider clamp, all with sharp sampling + stretch replica:

- Every coverage variant trades fringe↔cut (e.g. diamond 23 mm: fringe
  12→8 but cut 11→18; 130 mm: fringe 49→58). Node corners and wider
  clamp do not dominate either. Splat stays as is.
- Far-side-nearest (disagreement path, UV-side tap behind the warp
  surface → exact nearest texel instead of bilinear, zero extra taps):
  strictly +EV in sim on all three shapes — bad down (disc 23 mm 42→12,
  bar 23 mm 30→0), fringe/cut bit-identical everywhere.

### Live A/B of far-side-nearest: regressed, reverted

Shipped it, rebuilt, re-ran identical teleports: changed 74/104 hand px,
**regressed 69/98 vs improved 2/2**, all regressed truth-foreground
(old dim hand color → new black), error mass +23%/+17%. In real
multi-layer close geometry those pixels are magnified truth-fg edges,
where the dim bilinear mix beats black — the 2-layer sim does not cover
this population. Reverted (one-line functional change + a comment noting
the attempt); post-revert stale frames are bit-identical to baseline
outside the cube mask (0 px AtoB, 9 px BtoA).

### Standing residual (unchanged)

~1 px attached ring + phase-dependent tabs/bites; smooth-motion stale
(5 mm step ≈ 0.45 m/s head speed): 117–139/4600 hand px (2.5%), rim-only.
What would move it: 2x warpdepth supersampling (~4x splat atomics plus
resolve) — rejected per the no-major-perf-cost constraint.

## Open questions (for the reporter of the remaining fray)

1. Bright rim or dark bites? The measured residue is dark-on-bright.
   Bright bleed outward is a different, unreproduced mechanism.
2. Fast motion only, or slow drift too? Edges clean settled but crawling
   *during* motion points at temporal flip-popping (binary decisions
   chattering as sub-pixel phase drifts), which needs a temporal answer,
   not a sharper kernel.
3. Animated content (spinning cube)? Object motion is invisible to any
   timewarp — needs motion vectors, out of scope here.

## Prior art survey: how others handle depth-reprojection edges

Researched 2026-09-06 (vendor docs, Khronos registry, papers, GPUOpen).
Our pipeline is a single-layer forward-splat + backward-sample depth
reprojector; below is what everyone else does about the same silhouettes,
and which of their answers transfer. The shared vocabulary lives in the
Glossary at the end of this file.

### Vendors: VR runtimes

- **Oculus ATW/OTW/PTW/ASW.** Rotation-only async timewarp (ATW) leaves
  *positional judder* (Meta's term) on near-field translation. Positional
  TimeWarp (PTW) adds a depth mesh warp; ASW 1.0 synthesizes half-rate
  frames by *extrapolation* from video-encoder (NVENC) motion vectors with
  no depth, so layered/repeating patterns break (their example: zebra
  behind a picket fence). ASW 2.0 = PTW for ego-motion + ASW motion
  vectors for the residue (object motion, disocclusion, view-dependent
  shading) — depth is used to *separate objects before extrapolating*.
  PTW itself is implemented as a sparse-parallax-mapping-style
  approximation (full parallax-occlusion mapping was judged too
  expensive; overhead vs rotation-only is "lost in frame timing noise").
  Two hard rules they document: the warp kind must match everywhere in
  the chain (OTW vs PTW in both ASW steps, else double-correction
  artifacts), and without app depth everything falls back to ASW 1.0.
  Tooling precedent: OculusWorldDemo's Tab menu (frame-time slider,
  depth on/off toggle) is the same A/B-toggles methodology as our
  `MONADO_OPENWARP_*` flags.
  (Sources: Meta "Developer guide to ASW 2.0", "Introducing ASW 2.0",
  "Asynchronous Timewarp examined".)
- **Quest Application SpaceWarp (AppSW).** The app renders at half rate
  and submits a **motion-vector buffer + depth buffer at ~1/16 res**
  (Quest 2 default: 368x400 MV vs 1440x1584 eye!) through OpenXR
  `XR_FB_space_warp` (`XrCompositionLayerSpaceWarpInfoFB`:
  `motionVectorSubImage`, `appSpaceDeltaPose`, `depthSubImage`,
  `minDepth/maxDepth`, `nearZ/farZ`; MV = current-minus-previous NDC,
  xyz *including depth motion*, R16G16B16A16_SFLOAT recommended).
  Compositor does frame *extrapolation* (move pixels to predicted spots)
  plus PTW for head motion. Their documented artifact taxonomy matches
  ours almost 1:1: *disocclusion shimmer/halo* on moving silhouettes,
  thin-edge artifacts from the low-res MV buffer (their "Railings"
  scene), single-MV-per-pixel ambiguity on transparency (recommends
  proxy meshes, alpha-clip MV for text/UI, "Output depth and velocity"),
  UI ghosting → put UI on separate compositor layers that bypass the
  warp (exactly our quad-overlay practice). Depth doubles as the edge
  detector ("AppSW uses the depth buffer to identify edges"). MV pass is
  "almost free" (no lighting/textures unless discard); static objects
  may use camera-motion-only vectors reconstructed from depth. Debug
  views (`MVOverlay` modes) and RenderDoc tracing are the recommended
  workflow. (Sources: Meta AppSW native/Unity/Unreal guides, Khronos
  `XrCompositionLayerSpaceWarpInfoFB` reference.)
- **SteamVR Motion Smoothing** (Valve, Alex Vlachos). Same high-level
  tech as ASW by Vlachos's own account ("different implementations of
  the same high-level tech"): last two frames → GPU video-encoder motion
  vectors → filter the vector field → apply to the newest frame; a depth
  buffer resolves overlaps ("closer depth wins" — our atomicMin rule).
  Can synthesize 2–3 frames per real one. Valve explicitly prefers
  *generalized, no-depth* solutions and ships repeating-pattern fixes
  for all apps. Their patent (US10733783) adds detail: downsampled
  frames to the encoder, macroblock matching, vector-driven *mesh
  distortion* (moved vertices stretch triangles — the mesh-path analog
  of our tabs), gradient blending between moved/unmoved verts, depth
  buffer arbitration of multi-mapped pixels. (Sources: SteamVR blog,
  RoadToVR interview, US10733783.)
- **Windows Mixed Reality / HoloLens LSR.** Microsoft's Late-Stage
  Reprojection runs *per-pixel with depth* on immersive headsets but
  falls back to a *stabilization plane* (single quad at a focus point)
  where no accurate depth exists — i.e. our plane-timewarp fallback is
  industry-standard practice, and `HolographicSettings.SetFocusPointForFrame`
  is its API. HoloLens guidance also recommends 16-bit depth + pulled-in
  far plane as the cheap precision lever. (Sources: Microsoft
  "Hologram stabilization", Unity WMR manual.)
- **PlayStation VR2.** Sony documents hardware *positional reprojection*
  with "novel hole-filling", plus 60→120 frame interpolation mode —
  whose edge ghosting/double-imaging blur reviewers called out as worse
  than ASW/Motion Smoothing, with Sony reportedly iterating on it since.
  Lesson: interpolation (two real frames) still ghosts edges; our
  extrapolation-from-one-frame problem is strictly harder, so parity
  with ASW-class output is already a good result. (Sources: Sony STEF
  2022, UploadVR technical analysis, Jun 2023 dev reports.)
- **OpenXR depth submission.** `XR_KHR_composition_layer_depth`
  (`XrCompositionLayerDepthInfoKHR`: `subImage`, `minDepth/maxDepth`,
  `nearZ/farZ`; nearZ>farZ signals reversed depth) is the standard hook
  our path consumes; XR_FB_space_warp adds motion vectors. Any
  "vectors" future for openwarp plugs in here. (Source: Khronos
  registry.)
- **Latency reducers (complementary, cheap).** Meta's Late Latching
  (re-emit pose-derived uniforms at end of render thread; saves ~10 ms
  motion-to-photon, measured via `Prd`) and Phase Sync (schedule render
  to finish just before the compositor needs it) both *shrink the warp
  delta*, which shrinks every edge artifact proportionally. Same family:
  pose prediction tuning, higher refresh rate, freshest-source-first.
  These attack artifact *size*; our shader attacks artifact *shape*.

### Foundations: depth-image-based rendering (DIBR)

Our compute path is a textbook DIBR pipeline (McMillan's "3D warp" +
splatting + hole fill); the 1990s literature already named our
artifacts and triaged the same fixes:

- **McMillan 3D warp + occlusion-compatible order** (PhD 1997):
  unproject-per-pixel-depth + reproject; visibility resolved by
  traversal order (painter's algorithm over 4 sheets around the
  epipole), no z-buffer/atomics needed. GPU-hostile (serial order) and
  the discrete-image version breaks strict compatibility (Murk &
  Bishop 1997) — which is *why* we pay for atomicMin instead. Terms to
  reuse: *epipole*, *occlusion-compatible order*, *exposure errors*
  (McMillan's word for disocclusion), *reconstruction problem*.
- **Mark post-rendering 3D warping** (I3D 1997 + PhD 1999): two
  reconstruction modes — splatting with disparity/normal-dependent
  kernel size vs mesh reconstruction — and the mesh mode's failure is
  our graphics-path failure verbatim: *"rubber sheets stretching from
  the edge of the foreground object to the background"* at silhouettes,
  fixed by discontinuity detection + special compositing. Multi-
  reference compositing with *binary* keep/overwrite decisions (our
  occlusion verdict is the single-reference degenerate case).
  Hole-filling by epipolar "wiping" across the hole. And the
  methodology match: an off-line algorithm test-bed driven by captured
  user motion — exactly what `scripts/owsim-*.py` are.
- **Carmack's latency-mitigation notes** (danluu.com transcription):
  translation warp gives *"smears or gaps along edges"* with
  *"first person view hands and weapons [as] a prominent case"* —
  our playground hands, predicted a decade early. His mitigations:
  limit translation-warp magnitude, *compress the scene depth range*
  ("making constant the depth range … to limit the dynamic
  separation"), render near-field occluders as a *separate plane
  composited after the warp* (our quad overlay, independently
  invented). Forward warp "offers the best accuracy"; a triangle grid
  at a fraction of depth resolution is the cheap version; the single
  quad "avoids all silhouette artifacts at the expense of incorrect
  pixel positions" (our plane fallback, same tradeoff stated).
  Reverse warp "produce[s] identical results for simple direction
  changes, but additional artifacts near geometric boundaries …
  unless considerable effort is expended to search a neighborhood for
  the best source pixel" — the charter for our agreement test.
  Plus scanout-row-interpolated warp against "waggle" (racing the
  beam) and freshest-source-first.
- **Layered Depth Images** (Shade et al., SIGGRAPH 98): multiple
  depth+color pixels per line of sight; average depth complexity only
  ~1.24 in their scenes; back-to-front *over* compositing, no z-buffer
  (which is what makes splatting + alpha cheap). The complete answer
  to disocclusion when you can afford the layers. Follow-up with our
  exact vocabulary: Muddala et al. 2016 builds the LDI in the
  *original* view (inpaint there, not in the warped view — "data in
  the virtual view consists of other artifacts"), classifies FG/BG
  with depth-discontinuity pixel pairs (DDPP, cf. our 5% band),
  inpaints occluded layers with neighboring background, and names
  *translucent disocclusions* (fg-behind-fg) plus temporal consistency
  as the open problems.
- **Multi-Plane Images** (Zhou et al., SIGGRAPH 2018): LDI with fixed
  fronto-parallel RGBA planes + *over* compositing; the alpha channel
  is what gives soft edges, reflections, and transparency — i.e. alpha
  is the missing coverage channel our pipeline lacks (cf. the
  premultiplied-alpha analogy below). Rendering is homography +
  composite: real-time-capable once the planes exist; the cost is all
  in *building* them (there, a neural net).
- **MPEG MIV** (immersive-video standard): industrial DIBR at scale.
  Pruning (drop pixels visible elsewhere; keep test = 10% depth +
  3x3-luma difference, then erosion/dilation cleanup), patch atlases,
  background synthesis by rendering with *negated depth* (background
  over foreground!), `PushPullInpainter` for the rest, server-side
  inpainting, and the honest benchmark line: *"the handling of true
  contours … constitutes the true benchmark"* — silhouettes are where
  every synthesizer is judged. (Sources: MIV overview + TMIV docs.)
- **Pull-push / push-pull** (Gortler et al., Lumigraph 1996; GPU
  revisits: Kraus/Strengert, GRAPP 2009): splat → pull (mip pyramid
  with weights) → push (fill gaps from coarse levels without blurring
  what's valid). The cheap, GPU-friendly (0.6 ms/Mpx-class), proven
  hole-filler — used for DOF disocclusion (Kraus & Strengert 2007) and
  inside TMIV. Strictly better-behaved than nearest-fill for *wide*
  holes; orientation-free. Candidate if our 13x13 window ever proves
  too small — not for the 1 px ring.

### Splatting theory: footprints done right

- **EWA splatting** (Zwicker et al., SIGGRAPH 2001 / TVCG 2002):
  reconstruction kernel + low-pass filter unified; elliptical
  footprints from the local projective mapping; A-buffer with a
  coverage threshold (τ≈0.4) for edge antialiasing. Our quads are the
  degenerate case (box footprint, hard nearest-wins). Takeaway: the
  literature's correct footprint is oriented/elliptical and
  *coverage-weighted*, never binary — our binary whole-pixel
  quantization is exactly the documented residual.
- **Softmax splatting** (Niklaus & Liu, CVPR 2020): the soft version
  of our atomicMin — overlapping splats weighted by softmax over an
  importance metric (depth, or brightness-constancy), with a
  temperature α sliding between averaging and z-buffering. A
  fixed-function approximation (few depth-ranked taps, hand-tuned
  α) is the shape any "soft z" future for the splat pass would take;
  needs extra taps/images, so on the shelf, not on the roadmap.
- **3D Gaussian Splatting + Mip-Splatting** (Kerbl et al. 2023; Yu et
  al., CVPR 2024 Best Student Paper): screen-space *dilation* of
  splats causes **both dilation artifacts (zoom out: structures too
  thick) and erosion artifacts (zoom in: structures too thin)** —
  the same two-sided slider we measured, now with a name and a Best
  Paper. Their fix (3D smoothing + mip box-filter ≈ 1 px) and the
  caution that EWA-style wide filters   oversmooth map directly onto
  our "wider kernel = softer but wrong" results.

### Temporal/history family: TAA, upscalers, frame generation

Same silhouettes, same words — *ghosting* (their name for our double
view), *disocclusion mask*, *history validation* — with one structural
advantage we lack: a fresh current frame to fall back on.

- **TAA survey** (Yang/Liu/Salvi, EG 2020): history validation =
  rejection (accept/reject) vs rectification (clamp/clip toward the
  current frame). Practical forms: convex-hull → color-AABB
  clip/clamp (Karis, in YCoCg for tightness) → *variance clipping*
  (Salvi 2016: mean±γσ, γ≈1; robust to outliers). Ghosting concentrates
  *near high-contrast boundaries* because big AABBs pass invalid
  history — our exact failure geography. UE4's "responsive AA" stencil
  flag for risky materials is our SHOW_* flags' spiritual sibling.
- **Salvi 2016 (GDC) specifics worth stealing:** longest-motion-vector
  tracking so small fast features aren't missed by center sampling;
  per-layer TAA for multi-layer images; large (7x7) variance windows
  via pre-filtered moments (same moments trick as VSM below); jitter
  sequence kept shortest-possible so bad locks die fast.
- **FSR2** (open source, GPUOpen): the closest public analog of our
  two passes. *Reconstructed previous depth* = scatter current-depth
  samples with motion vectors, keep nearest by atomics — our splat,
  mirrored in time. *Depth clip* = current vs reconstructed depth with
  a minimum-separation threshold → disocclusion mask (our occlusion
  verdict, two-sided). On disocclusion: drop most history, keep a
  little (smoother transition), blur the incoming sample. *Thin-feature
  locking*: detect pixel-wide ridges, exempt them from clipping,
  lifetime = jitter-sequence length, killed on disocclusion or shading
  change — a temporal answer to exactly our flip-popping. *Reactive*
  and *transparency & composition* masks: app-provided alpha/coverage
  hints for pixels with no depth/MV footprint (particles, blended) —
  i.e. an explicit coverage channel, which is what we are missing.
  "Only use input information to fix history" (never accumulated data)
  is a portable debugging rule.
- **FSR3 frame interpolation** (open source, GPUOpen): the full
  disocclusion machine. Interpolated-frame depth via reproject+dilate;
  motion-vector fields built with *atomic priority packing* (primary
  bit + 10-bit camera-distance priority + 5-bit color-similarity);
  vector-field *inpainting pyramid* (closest-in-2D, farthest-in-depth
  selection — cf. our stretch tie-break); dual disocclusion masks;
  color-similarity blending of game-vector vs optical-flow results;
  final *inpainting pyramid* (mip chain ignoring holes ≈ pull-push on
  color) for leftover holes; debug views per stage. This is the
  reference architecture if openwarp ever grows vectors: depth-dilate,
  priority atomics, pyramid fill, staged debug views.
  (DLSS frame generation is the same public family — game vectors +
  optical flow + UI separation — but NVIDIA's internals were not
  individually verified here; FSR3 above is the citable one.)

### Adjacent domains that already solved our sub-problems

- **Shadow-map filtering (the formal analog).** PCF (Reeves 1987):
  filter the *comparisons*, never the depths — our agreement test is a
  2x2 PCF-style vote for the same reason (filtered depth is "neither
  surface at an edge"). VSM (Donnelly & Lauritzen 2006): moments +
  Chebyshev → fully filterable visibility, whose *light bleeding*
  (overlapping occluders) is our fringe with equations: a step
  visibility function cannot be reconstructed without sampling all N
  pieces. Transferable maxims: minimum filter width hides
  magnification artifacts (why our bilinear interiors are fine);
  bias/Peter-Panning is their erosion/dilation slider by another name.
- **Post-process motion blur** (velocity buffer family: Rosado,
  McGuire 2012, UE4, Blender EEVEE): tile-dilate (MAX velocity over
  tiles so fast features aren't missed — cf. Salvi's longest vector),
  depth-tested gather, *inner vs outer blur* distinguished (inner =
  disocclusion, needs background the rasterizer never saw). The
  complete answer there is ray-reveal of the true background (2022
  hybrid-MBlur paper) — exact, costs rays. Our honest-black holes are
  the no-rays version of the same call.
- **Screen-space raymarching** (SSR/SSGI): Hi-Z traversal, *thickness*
  heuristic for behind-surface continuation, screen-edge fade for
  out-of-frustum rays (= our `source_uv_valid` guard, same job).
- **DOF composite** (McGuire half-res near/far; EEVEE): separate
  foreground/background layers with dilated tiles; Blender's notes
  admit foreground *inflation* over background as the accepted
  tradeoff — production precedent for shipping our dilation ring.
- **Premultiplied alpha** (Porter–Duff): filter *associated* (coverage
  × color) data, never raw color across edges. Our bilinear-across-
  the-edge mixes are unassociated filtering; the principled fix is a
  coverage channel (MSAA depth + alpha-to-coverage content warps
  better), not a cleverer kernel.
- **Joint bilateral upsampling** (Kopf et al. 2007): the theory of
  depth-aware interpolation — range-weighted taps that stop at steps.
  Our 5%-band tests are its degenerate 2x2 form.
- **Soft particles / depth fade**: fade by depth delta at
  intersections instead of hard clipping — the vocabulary for any
  future "soften the ring by agreement distance" experiment.

### Metrics and methodology (how the field judges this)

- **FLIP** (Andersson et al. 2020, NVlabs/flip, `pip install
  flip-evaluator`): difference evaluator for *alternating* images —
  the workflow we already use by eye (stale-vs-fresh flipping), with a
  color pipeline + an explicit edge/point *feature* pipeline that
  up-weights silhouette differences, pooled by mean. Our abs-err>0.1
  counts should migrate to FLIP maps + mean FLIP when judging future
  fixes; HDR-FLIP handles all-black backgrounds (median-luminance 0).
- **Synthetic ground-truth scenes** are the standard rig (Mark's
  test-bed with captured motion; the UNC IBR group's synthetic
  scenes): our `owsim-*.py` + same-pose stale/fresh pairs are conformant
  practice, not a hack.
- **Staged debug views + A/B toggles** are universal: AppSW
  MVOverlay modes, FSR3 per-stage debug views, OculusWorldDemo depth
  toggle — our SHOW_*/DISABLE_* flags and `analyze-warp.py` belong to
  the same family; keep them.

### Frame generation at scale: how the interpolators handle edges (2022–2026)

All modern frame generators take depth + motion and still fight the
same silhouettes. Their edge answers, newest first:

- **DLSS 3 Frame Generation (2022).** Convolutional autoencoder over 4
  inputs: current + prior frame, Ada Optical Flow Accelerator field,
  engine motion vectors + depth. Per-pixel network choice between MV
  (geometry) and flow (shadows/reflections/particles/UI). De-occlusion
  stays the documented weak spot (Digital Foundry/HotHardware: ghosted
  post, distorted feet around Spider-Man). Integration rules that
  transfer: provide a **Hudless** (pre-UI) buffer + UI alpha so FG can
  recompose UI undistorted, and reduce motion blur while FG is active.
  (Sources: NVIDIA DLSS3 announcement + Ada whitepaper, Streamline
  `ProgrammingGuideDLSS_G`.)
- **DLSS 4 incl. Multi Frame Generation (Jan 2025, RTX 50).** Split
  architecture: half the network runs once per frame pair and is
  reused, a much smaller half runs per generated frame (3 frames in
  ~1 ms avg on 5090 vs 3.25 ms for 1 frame on 4090). Transformer
  backbones for Super Resolution + Ray Reconstruction (2x params,
  self-attention over space *and* time: Alan Wake 2 fence/fan/power
  lines as the ghosting regression tests). The research page states
  our problem verbatim: MVs are wrong for specular/reflections/UI,
  depth is wrong for lasers and post-raster UI — "teaching the network
  to effectively use these inputs where reliable and supplement them
  where they are not … required an AI-based approach." Large-motion
  baselines cited: FILM (2022), MoMo (2025). Disocclusions in RR are
  filled by attention over spatial context. (Sources: NVIDIA ADLR
  DLSS4 page, GeForce News, Catanzaro DF interview.)
- **DLSS 4.5 (CES 2026).** 6x Dynamic MFG (up to 5 generated frames,
  multiplier flexes with headroom) + 2nd-gen transformer SR on *all*
  RTX back to 20-series, advertised as holding "fine detail and clean
  disocclusions" in Performance modes. Direction of travel: generation
  count scales, edge quality is the differentiator. (Source: press +
  BottleneckPC explainer; second-hand, treat specs as approximate.)
- **FSR Frame Generation ML 4.0.1 ("Redstone", SDK 2.3, Jun 2026).**
  "Predict per-pixel motion *and appearance*, then blend with
  motion-vector reprojection", trained on Instinct GPUs; optical flow
  + MV inputs; analytical 3.1.6 kept as fallback for older GPUs.
  Requirements confirm the floor: MV + depth in supported formats even
  with third-party upscalers; ≥60 fps pre-interp, <30 fps forbidden
  (artifacts dominate). FSR 4 upscaling's headline win is particles
  without reactive masks — the ML answer to our mask-everything
  instinct. (Sources: GPUOpen FSR SDK manuals, Redstone announcements.)
- **XeSS-FG (Intel, dev guide 2025).** Compute-shader sequence behind
  a proxy swapchain; inputs MV (current→previous) + depth at equal
  size. Two vendor-blessed rules: XeSS **uses the depth texture to
  dilate motion vectors** internally, and user-supplied high-res MVs
  must be pre-dilated as the **foremost surface in a 3x3 neighborhood**
  — the atomicMin rule, stated as API contract. Camera vs object
  velocity flattening passes and UI-texture flags included.
- **MetalFX (Apple, Metal 4 / WWDC25).** Temporal scaler takes
  jittered color + MV + depth; depth "prioritizes foreground edge AA
  and gives clues on newly exposed objects" (vendor quote of our
  problem). Reactive mask for particles/transparency (FSR-reactive
  analog), dynamic input resolution, exposure debugger. New
  **Frame Interpolator**: two frames + MV + depth → in-between frame,
  reusing the upscaler inputs, and it *adjusts MVs to simulation
  length*. New **denoised upscaler** additionally consumes
  albedo/specular/normal/roughness G-buffers. (Source: WWDC25 session
  211 transcript.)
- **AFMF 2 / 2.1 (AMD driver-level: no engine data at all).** TV-style
  interpolation from screen space only; DF: "can never match"
  integrated solutions. Failure policy is the lesson: AFMF 1 disabled
  itself in fast motion (jarring on/off), AFMF 2 keeps blending, and
  2.1's **Fast Motion Response** lets the user pick **Repeat Frame**
  (honest judder) vs **Blended Frame** (smooth garbage). Rumored AFMF
  3 moves to Redstone ML. Takeaway: without depth, the optimal policy
  is surrender — which is exactly what our depth input buys us out of.

### Extrapolation for latency: openwarp's direct siblings

Interpolation has two frames; extrapolation has one plus a deadline —
our situation, and an active research branch:

- **Reflex 2 Frame Warp (CES 2025).** Warps the just-rendered frame to
  a re-sampled camera position *just before scanout*; a
  latency-optimized predictive renderer inpaints the resulting holes
  from camera + color + depth priors. THE FINALS 56→27→14 ms;
  VALORANT <3 ms at 800+ fps. This is depth reprojection as a shipping
  latency product — our compute path's closest commercial cousin.
  (Source: NVIDIA Reflex 2 announcement.)
- **Post-render late-warp study (Kim/McGuire et al., HPG 2020).** The
  research Reflex 2 cites: rotation-naive (black beyond the guardband),
  rotation-oracle, translation-rotation-oracle conditions. Rules:
  translation needs depth/velocity; render HUD and world separately;
  warp-aware hit detection (invasive). Even naive warp erased 80–90%
  of an 80 ms latency penalty for aiming.
- **ExtraNet (Guo et al., SIGGRAPH Asia 2021).** Real-time
  *extrapolated* rendering: G-buffers (MV, stencil, world position,
  NoV, world normal) for warping + hole marking; depth/normal/
  roughness/metallic as network input. Two explicit tasks:
  **irradiance inpainting** (no correspondence) vs shading prediction
  (correspondence). Uses motion vectors, *not* optical flow ("no
  distortion"); **splats backward MVs to get forward MVs** (our splat
  in reverse); occlusion MVs from Zeng et al. 2021; 1.5–2x fps; TAA
  hides residual blur. Code: `fuxihao66/ExtraNet`.
- **ExtraSS (Wu et al., SIGGRAPH Asia 2023, Intel+UCSB).**
  Extrapolation + spatial supersampling jointly. **G-buffer guided
  warping**: à-trous kernels weighted by G-buffer similarity — a
  bilateral warp filter, i.e. the principled big brother of our 3x3
  agreement test (and it notes occlusion-MVs still fail on complex
  backgrounds). Plus a shading-refinement net (flow + residual for
  shadows/reflections). UE-evaluated.
- **Temporally reliable MVs (Zeng et al. 2021).** Occlusion-aware
  motion vectors from G-buffer reasoning (dynamic-object stencil,
  normals for self-occlusion, positions for statics) — the paper to
  hand any engine team asked to emit better MVs.

### Depth-aware interpolation without any game data (DAIN and friends)

- **DAIN (Bao et al., CVPR 2019).** Depth-aware flow projection: when
  several flows collide at one output position, aggregate with weights
  **w = 1/depth** so near wins. Ablation: **soft blending beats hard
  minimum selection** — softness absorbs depth uncertainty (supports
  our agreement-gating instinct over hard cuts). Holes filled
  outside-in; frames warped with learned 4x4 kernels. Limitation to
  memorize: estimated-depth boundaries blur (their shoe/skateboard) —
  a caution against any "just estimate depth" shortcut. Code:
  `baowenbo/DAIN`. (Predecessor MEMC-Net; modern large-motion
  baselines FILM 2022 / MoMo 2025, per NVIDIA's DLSS4 comparisons.)

### Shipping temporal upscalers' edge playbooks

- **Unreal TSR.** Parallax **disocclusion heuristic from depth +
  velocity**, deliberately placed on **async compute** (0.5 ms
  recovered in Fortnite) — the pass-placement trick for any future
  openwarp-side heuristic. **TSR 1spp** convergence metric
  (ScreenPercentage² × fps → time-to-detail for fresh disocclusions,
  e.g. 66 ms at 50%/60 fps). **Nyquist-Shannon 2x history** kills
  reprojection blur at 4x history-update cost. **History Resurrection
  (5.4)**: keep persistent frames, reuse when closer than previous
  (depth-reprojected compare; 0.24 ms in Fortnite). **Has Pixel
  Animation** flag: don't trust velocity on procedural surfaces.
  Spatial-AA fallback whenever history is rejected; `VisualizeTSR`
  rejection mask as first diagnostic. (Sources: Epic TSR docs +
  tuning PDFs.)
- **NSRR (Xiao et al., Facebook Reality Labs, SIGGRAPH 2020).** Learned
  4x4 supersampling from color + depth + MVs; warps at **target
  resolution via zero-upsampling** so the net sees valid/invalid
  explicitly; trained on a VR head-motion dataset. VESPCN+ ablation:
  extra inputs +1.1–1.3 dB, architecture the rest — inputs necessary,
  not sufficient. Follow-up (Zhong et al. 2023): full G-buffer features
  + cached history features.
- **Insomniac temporal injection.** Render >half the pixels, jitter,
  inject into 4K over a handful of frames (Ratchet & Clank →
  Spider-Man); known cost is slight softness; paired with DRS. The
  console proof that sparse-jitter + history converges.

### Denoising and resampling: visibility reuse with receipts

- **SVGF (Schied et al. 2017).** Geometry-gated temporal accumulation:
  per-tap depth/normal/meshID consistency tests, redistribute
  discarded weight, 2x2→3x3 escalation for foliage, else declare
  disocclusion and drop history; variance estimate falls back to
  spatial for <4 frames after disocclusion; depth/normal/luminance
  edge-stopping (local-linear depth model for landscapes). The
  conservative all-analytical reference for any consistency-gated
  filter — note it escalates *before* surrendering, like our
  13x13-after-3x3.
- **ReSTIR family (Bitterli 2020 → GRIS Lin 2022 → conditional/Wyman).**
  Spatiotemporal reservoir reuse matched across frames by motion
  vectors — temporal reuse *is* reprojection with MIS receipts. Shift
  mappings: reconnection, delayed, roughness-gated hybrid, sequential
  (try several, let resampling pick). Hard rules: retest visibility
  after shifting ("reconnecting through occlusions" is a named
  pitfall), cap temporal confidence (~20x) against correlation.
  **Suffix ReSTIR**: match in world space, not screen space, and
  disocclusion variance-lag disappears — object-space reuse beats
  screen-space exactly where our screen-space warp frays.

### What this means for openwarp (ranked, honest costs)

1. **Motion vectors are the industry's answer** for object motion and
   for disambiguating silhouette taps (AppSW proves low-res MV+depth
   suffices: 368x400). Needs app submission (`XR_FB_space_warp`-style
   path + engine MV passes). Strategic, out of shader scope — but any
   future vectors work starts from FSR3's layout (priority atomics,
   inpainting pyramid, dual masks).
2. **A background layer is the complete single-frame answer**
   (LDI/MPI/MIV-background-synthesis). Real cost: second depth/color
   pair + fill pass. The only proposal that *removes* disocclusion
   instead of hiding it.
3. **Pull-push inpainting pyramid** for holes wider than our 13x13
   window: cheap, GPU-proven, orientation-free. Drop-in upgrade path
   for stretch if wide disocclusions ever dominate (they don't today).
4. **Shrink the delta, not the kernel**: prediction tuning,
   late-latch-style fresher poses, higher refresh — every edge
   artifact scales with warp distance, at zero shader cost.
5. **Judge with FLIP**, keep the A/B flags, keep sim-first discipline
   (this log's sliders all reproduced in `owsim-quad-cover.py` before
   touching GLSL — the far-side-nearest revert is why).
6. **Coverage channel, not cleverer kernels**: MSAA-depth-sourced
   coverage (premultiplied analogy) is the principled end of the
   halo↔cut slider. Everything else tested so far slides it.
7. **Content-side levers exist**: depth-range compression, separate
   near-field/UI layers (Carmack + every vendor), alpha-clip over
   alpha-blend for text. Already partially practiced (quad overlay).
8. **Depth-dilated MVs are vendor canon** (XeSS foremost-in-3x3,
   our atomicMin; TSR's parallax heuristic; ExtraNet's occlusion
   MVs): any future vector consumer must dilate by depth, never
   average. Our splat already emits the right quantity.
9. **Soft beats hard at uncertain boundaries** (DAIN ablation):
   keep agreement-gated soft handling; the far-side-nearest revert is
   consistent with the literature, not just our live test.
10. **Cheap experiments this round licenses**: G-buffer-similarity
    warp weights (ExtraSS bilateral warp in miniature — weight the
    3x3 taps by agreement instead of binary accept); 1spp-style
    frames-to-settle accounting for stale windows; AFMF-style honesty
    (prefer source over synthesis when agreement collapses).
11. **Neural inpainting is the known-good hole answer** (ExtraNet
    irradiance inpaint, Reflex predictive inpaint, DLSS attention
    fill) — every instance consumes G-buffers/MVs/extra frames we
    don't submit. Strategic, like vectors; the analytical ceiling
    remains pull-push + background layer.

### Vendor contract notes: Meta AppSW/PTW depth specifics

What Meta's Horizon docs (AppSW technical guide, native `mobile-asw`
guide, Unity guide, compositor doc, ASW 2.0 dev guide, `Introducing
AppSW` blog) actually specify about the depth side. No shader code is
published — this is the contract and the policy, which is exactly the
part we'd otherwise have to invent:

- **PTW disocclusion = neighbor fill.** Compositor doc, verbatim in
  spirit: previously-hidden pixels are unknowable, "fills in best
  guesses based on neighboring pixel data." Our 13x13 stretch is the
  same family — the reference implementation does nothing fancier at
  the PTW stage. (Sources: `os-compositor`.)
- **Depth is their edge detector.** MVOverlay mode 2: "Render the
  depth buffer. AppSW uses the depth buffer to identify edges." Our
  agreement/ring logic is the same idea, independently arrived at.
- **PTW consumes low-res depth.** The MV-pass depth (368x400 on Quest
  2) feeds the compositor, and "higher than the recommended resolution
  will not give additional quality benefits." Licenses a downsampled
  depth path on our side; the EXT recommended-resolution query is the
  conformant knob. (Sources: `os-app-spacewarp`, native guide.)
- **MV-pass rules** (for future engine integrators + our test
  client): opaque only, no MSAA ("no benefits found"), trivial
  shaders (prev/curr NDC, skip lighting unless discard), depth =
  plain resolve. Static objects may use camera-motion-only vectors
  with depth reuse (Unity 2022.3+) — i.e. **depth-only is the complete
  answer for statics**, confirming our fallback is not degraded for
  that case. (Sources: native + Unity guides.)
- **Matrix-consistency contract** (Unity guide): MV pass must share
  the *exact* camera matrices with the eye pass, including late-latch
  state, or vectors are silently wrong. Runtime-side consequence: we
  can and should distrust (see plan item 7).
- **Alpha-clip, never blend, for text/UI MVs.** Echo in any future
  extension-support docs. (Source: Unity guide.)
- **Linearization metadata**: projection Z/W rows + world scale on PC
  (`ovrTimewarpProjectionDesc`/`ovrViewScaleDesc`); in OpenXR-land
  that's `nearZ/farZ` + `minDepth/maxDepth` incl. reversed depth.
  Our `app_depth_to_eye_z` (`openwarp.comp:158`, mirrored in the
  splat pass) already handles reversed / infinite-far / GL-clip /
  subrange / NaN — remaining work is an explicit test matrix, not a
  fix (see plan item 1). (Sources: ASW 2.0 guide, XR registry.)
- **Debug views to copy**: MVOverlay 1–4 (MV direct, depth/edges,
  contrast MV, zero-centered MV). Mode 4 (zero-motion gray) is the
  ideal MV-trust diagnostic when MVs land; our `DEBUG_SHOW_EDGES` is
  already the mode-2 analog.
- **`XrSpaceWarp*` native sample** (Oculus mobile SDK): app-side only,
  but a known-good MV/depth *generator* — test-vector source for
  validating a future implementation.
- **Policy, not physics**: Quest gates PTW on AppSW (needs MVs too).
  Monado need not follow — depth-only PTW is architecturally complete,
  and we already ship it.

### Improvement plan for our openwarp (ranked, file-grounded)

> The fidelity subset of this plan (edges, no MVs) now lives as a
> standalone document: `docs/openwarp-fidelity-plan.md`. This section
> keeps the full ranked context including the MV track.

Definition of done everywhere: same-pose stale-vs-fresh pairs
(`capture-stale-burst.py` + hand masks + `analyze-warp.py`), plus a
ms-budget check at 90 Hz stereo on the sandbox GPU (AMD Radeon AI
PRO R9700, RDNA4 discrete) for anything in
the per-frame path. Sim-first discipline holds (`owsim-quad-cover.py`
before GLSL).

**P0 — audits and cheap wins (days).**

1. **Depth-convention test matrix.** `app_depth_to_eye_z` already
   handles reversed / infinite-far / GL / subrange / NaN by reading,
   but has no dedicated coverage: extend the ow sim (or a small unit
   harness) over near/far/min/max/depthIsGL combinations incl.
   `far <= near` and `dn` clamping edges. Files:
   `monado/.../shaders/openwarp.comp:158`,
   `openwarp_splat.comp` (mirror fn).
2. **Low-res depth experiment.** Downsample app depth 2x/4x before
   the splat, A/B with `analyze-warp.py`; Meta predicts no quality
   loss, pure win on splat + warp bandwidth. Files:
   `openwarp_splat.comp`, warp dispatch in
   `compositor/main/comp_renderer.c`.
3. **(done)** Neighbor-fill policy is documented in the shader header
   (`openwarp.comp:19-23`); cited here for the record.

**P1 — quality/perf work (weeks).**

4. **Debug views**: keep `DEBUG_SHOW_EDGES` (Meta mode-2 analog);
   add a coverage-hole view (invalid warpdepth, distinct from stretch)
   to separate "no data" from "filled" in one glance. Spec the
   zero-centered MV view now (Meta mode 4) so it lands with item 7.
   Files: `openwarp.comp:350-671`, `render_interface.h`
   (`RENDER_OPENWARP_DEBUG_*`), debug GUI panel.
5. **Agreement-weighted warp taps** (ExtraSS bilateral warp in
   miniature, ranked #10): weight the 3x3 taps by depth agreement
   instead of binary accept. Sim first; live A/B on identical
   teleports.
6. **Settle accounting**: 1spp-style frames-to-detail metric for the
   stale-window tooling, so future fixes report "converges in N
   frames" instead of ad-hoc sleeps. Files: `scripts/`.

**P2 — strategic (months).**

7. **`XR_EXT_frame_synthesis` runtime support**, in order: (a)
   advertise the extension + accept per-view MV/depth swapchains
   (state-trackers → IPC → compositor plumbing); (b) **MV trust
   check**: camera-induced MV component must agree with the tracked
   pose delta we already know, else fall back to depth-only — the
   runtime answer to Meta's matrix-consistency contract, and the
   single most important artifact gate (bad MVs >> no MVs); (c)
   synthesis pass = MV-advance dynamic pixels + existing depth warp
   for ego-motion, holes into the current stretch/black policy;
   (d) fallback chain MV → depth → plane; (e) relaxed-interval pacing
   (frame-timing contract work). First client: Godot 4.6+ frame
   synthesis; reference vectors: `XrSpaceWarp` sample. Per-Meta
   costing the MV path stays cheap (low-res buffers, trivial app
   shaders).
8. **FSR 3.1 MIT as reference, not dependency** (verified hostile as
   a compositor citizen: proxy swapchain, exclusive Vulkan queues,
   frame-ID continuity assumption, interpolation-only): lift
   analytical disocclusion/inpaint *techniques* into our hole-fill,
   never the SDK.
9. **FLIP migration** for judging (ranked #5, unchanged).

## Glossary: depth-reprojection research vocabulary

One-line definitions for every term worth knowing when reading papers,
vendor docs, or GPUOpen/GDC material about this technology. Our local
equivalents noted where they exist.

### Core reprojection family

- **Reprojection** — synthesizing a novel view from rendered image(s)
  + auxiliary data instead of re-rendering.
- **Timewarp / TimeWarp (TW)** — Oculus term for display-time
  reprojection of a rendered frame to the latest pose.
- **Orientation-only / rotation-only warp (OTW)** — assumes all content
  at infinity; corrects rotation only. Our plane fallback's ancestor.
- **Positional TimeWarp (PTW)** — warp using per-pixel depth: correct
  parallax under translation. Our compute path *is* PTW.
- **Spacewarp / SpaceWarp (ASW / AppSW)** — frame *synthesis* (new
  frames from old), not just pose correction. ASW: runtime-side,
  video-encoder/optical-flow vectors. AppSW: app-side motion vectors +
  depth at half rate via `XR_FB_space_warp`.
- **Motion Smoothing** (SteamVR) / **Motion Reprojection** (WMR) /
  **reprojection mode** (PSVR2 60→120) — other vendors' ASW-class
  frame synthesizers.
- **Late-Stage Reprojection (LSR)** — Microsoft's compositor-side warp
  (per-pixel with depth, else stabilization plane).
- **Stabilization plane / focus point** — single-quad LSR approximation
  (`SetFocusPointForFrame`); maximum correctness at one depth.
- **Extrapolation vs interpolation** — predict beyond the last frame
  (timewarp/ASW/Motion Smoothing: no added latency) vs synthesize
  between two frames (frame-gen: needs buffering, adds latency).
- **Forward warp (splatting)** — push source pixels to destinations;
  exact for arbitrary deltas, needs scatter + visibility resolution.
  Our splat pass.
- **Reverse / backward warp** — pull: for each destination, look up the
  source; GPU-friendly gather, needs neighborhood search at boundaries
  (Carmack). Our reproject pass.
- **Asynchronous (ATW/ASW)** — runs decoupled from app frame rate, on
  the freshest pose/frame available.
- **Pothole insurance** (Meta) — framing of warp as occasional-drop
  cover, not a substitute for frame rate.
- **Scanout racing / racing the beam** — warping per display row with
  row-interpolated poses (fixes "waggle" on progressive scanout).

### Artifact vocabulary (what you see)

- **Disocclusion** — newly visible region with no source data (the hole
  problem). McMillan: *exposure error*; MIV: *newly-exposed areas*.
- **Occlusion (stale-foreground)** — warp surface hidden behind
  foreground in the source view; sampling it paints a second copy.
- **Ghosting / double view / double imaging / echoes** — stale second
  copy (TAA's *ghosting* is the same word for history residue).
- **Halo / fringe / bleed** — foreground color leaking onto background
  (bright tabs outward).
- **Nicks / bites / cut / cut-through** — background (or black) eating
  into foreground.
- **Rubber sheets** (Mark) — mesh-warp triangles stretching across
  silhouettes; **smearing / melting / swimming** — its perceptual
  names; **inner vs outer blur** (motion-blur literature).
- **Judder (positional / multiple-image)** — Meta's term for
  translation uncorrected by rotation-only warp; distinct from
  frame-rate judder.
- **Shimmer / crawling / flicker / popping / temporal aliasing** —
  time-varying edge decisions as sub-pixel phase drifts.
- **Black smear** (OLED display lag) vs motion smear — different
  mechanisms, same word; disambiguate in reports.
- **Pupil swim** — distortion (not warp) error under eye motion.
- **Fizzing** (FSR2.2) — sparkling during disocclusions.
- **View-dependent shading residue** — speculars/highlights the warp
  moved correctly but shaded stale (ASW's leftover corrections).

### Depth and image representation

- **Depth buffer / Z-buffer** — per-pixel closest-surface depth from
  rasterization; warp input. Ours arrives via
  `XR_KHR_composition_layer_depth` (`nearZ/farZ/minDepth/maxDepth`).
- **Reversed-Z / reverse-infinite projection** — near→1, far→0 in float:
  near-magical precision distribution (Reed/Upchurch-Desbrun); what our
  warp math assumes. Near-plane distance still dominates precision.
- **EyeZ / view depth / planar depth** — metric distance along view
  axis (what our 5% band compares) vs NDC/window depth (nonlinear).
- **Sprites with Depth / depth sprites** — planar impostors carrying
  depth; warpable without per-pixel cost.
- **Layered Depth Images (LDI)** — multiple depth+color samples per
  sight ray (~1.24 avg complexity); back-to-front *over*, no z-buffer.
- **Multi-Plane Images (MPI)** — fixed fronto-parallel RGBA planes +
  over compositing; alpha = soft edges/transparency/occluded content.
- **Atlases / patches / pruning / occupancy** (MPEG MIV) — multi-view
  redundancy removal for transport; pruning tests (10% depth, 3x3
  luma) resemble our agreement band.
- **DDPP** (depth-discontinuity pixel pairs) — Muddala's FG/BG
  classifier at occlusions; our 2x2 step test is the degenerate form.
- **Translucent disocclusion** — fg-behind-fg reveal; needs >2 layers.
- **Out-of-frustum / out-of-field** — warp target with no source
  coverage ever (distinct from disocclusion); honest black.
- **Coverage / occupancy / alpha** — the per-pixel "how much of which
  surface" we lack (the missing channel behind the halo↔cut slider).
- **MSAA depth / alpha-to-coverage (A2C)** — hardware coverage sources;
  A2C content warps better than alpha-tested.

### Sampling, filtering, coverage

- **Splat / splatting / footprint** — forward-deposited reconstruction
  kernel (ours: projected texel quad, box filter, atomicMin).
- **EWA (elliptical weighted average)** — reconstruction + low-pass in
  one oriented filter; the correct-footprint theory (ours degenerate).
- **Softmax splatting** — soft z-resolution over depth-ranked taps
  (temperature α: averaging ↔ z-buffering).
- **Dilation / erosion (splat)** — footprint too big (halo) vs too
  small (cut holes); Mip-Splatting documents both from one filter.
- **Pull-push / push-pull pyramid** — splat → pull (weighted mip) →
  push (gap-fill without blurring valid); the cheap wide-hole filler.
- **Inpainting pyramid** (FSR3) — mip chain ignoring holes ≈ pull-push
  on color.
- **PCF (percentage-closer filtering)** — filter *comparisons*, not
  depths; our 2x2 agreement vote belongs here.
- **VSM/ESM/MSM + light bleeding + Chebyshev bound** — filterable
  visibility via moments; bleeding = fringe with equations.
- **Bilateral / joint-bilateral / cross-bilateral / guided filter** —
  edge-stopping (range-weighted) smoothing; our band test is a 2x2 JBU.
- **Minification / magnification / LOD / mip bias / anisotropic** —
  resampling regimes; our explicit Lod-0 has no true minification
  filtering (aliasing source under minify).
- **Centroid sampling** — MSAA rule keeping taps on the covered
  primitive; the hardware cousin of our agreement test.
- **Premultiplied (associated) vs unassociated color** — filter
  coverage×color, never raw color across edges (Porter–Duff).
- **Soft particles / depth fade** — fade by depth delta at
  intersections instead of hard clipping.
- **Conservative rasterization** — over-estimating coverage (opposite
  knob to erosion).

### Motion representation

- **Motion / velocity buffer** — per-pixel screen-space displacement
  (AppSW: NDC delta incl. depth; FSR: render-res velocity). *The*
  industry answer for object motion.
- **Camera-motion-only vectors** — ego part reconstructible from depth
  alone (AppSW static-object fast path).
- **Optical flow** (NVENC motion estimation → NVIDIA Optical Flow SDK
  → FfxOpticalFlow) — image-derived vectors; block matching optimizes
  compression, flow optimizes plausibility (Meta's warning).
- **Forward-backward / cycle consistency** — occlusion test via
  depth/flow disagreement both ways; our occlusion check is one.
- **Scene flow** — full 3D motion field (flow + depth change).
- **Ego-motion vs object motion** — depth fixes the former, vectors
  the latter; animated content is invisible to pure timewarp.
- **Motion-vector dilation / longest vector / tile-dilate (MAX)** —
  propagate fast/thin features to neighborhoods (McGuire, Salvi).
- **Reactive / transparency&composition masks** (FSR2) — app hints for
  pixels with no depth/MV footprint (particles, blended).
- **Proxy meshes** — render simplified occluders into the MV pass
  (AppSW transparency workaround).

### Temporal / history

- **History buffer / accumulation / EMA** — reprojected previous
  output blended with current (TAA/upscalers/denoisers).
- **History validation / rejection vs rectification** — drop stale
  history vs clamp it toward current data (convex hull → AABB →
  variance clipping, YCoCg).
- **Variance clipping (γ)** — Salvi's mean±γσ extents; γ trades
  stability for ghosting.
- **Thin-feature locking** (FSR2) — exempt ridges from clipping;
  lifetime = jitter length; killed on disocclusion/shading change.
- **Jitter sequence (sub-pixel)** — TAA's sampling pattern; shortest
  viable kills bad locks fastest.
- **Temporal lag** — stale shading carried forward (blur across
  frames), distinct from ghosting.
- **History rectification debt** — "only use input information to fix
  history" (FSR2 rule; ghosted accumulations poison rectification).

### Visibility and occlusion reasoning

- **Occlusion-compatible order / painter's algorithm / back-to-front
  over** — McMillan/LDI/MPI visibility without depth tests.
- **atomicMin / nearest-wins / z-buffering** — our discrete replacement
  for the above (GPU-friendly, binary).
- **Depth-clip / depth-separation (h) test** — FSR2's two-sided
  disocclusion verdict (ours is one-sided + agreement shortcut).
- **Source-visible test** — our render-view-depth agreement check
  (never compare across eye positions).
- **Depth dilation for disocclusion search** (FSR3 interpolated depth)
  — dilate-then-reproject to estimate hidden surfaces.
- **Priority packing** (FSR3: primary bit + 10-bit camera-distance +
  5-bit color-similarity atomics) — how to resolve multi-mapped
  vectors deterministically.
- **Thickness heuristic** (SSR) — assume surfaces continue behind
  silhouettes; candidate model for hole depth.
- **Focus/stabilization shortcuts** — single-plane or compressed-range
  approximations (cheap, bounded error).

### Pipeline, runtime, display timing

- **Compositor / layers** (projection, quad, cylinder…) — late
  composition after warp; UI/text belong on warp-bypassing layers.
- **Swapchain / acquire / wait / present / predicted display time** —
  the frame plumbing; prediction horizon sets warp distance.
- **Late latch / late-latching** — re-emit pose uniforms at the last
  moment (≈−10 ms motion-to-photon).
- **Phase Sync / frame pacing** — schedule render to finish just
  before composition (beats fixed-latency; measured via Prd).
- **Pose prediction (horizon)** — shorter horizon = smaller deltas =
  smaller artifacts; tuned per runtime.
- **Refresh rate / half-rate / frame doubling** — 60→120 or 45→90
  synthesis modes; higher rate shrinks every delta.
- **Async compute / queues / preemption** — where warp passes live;
  subgroup ops as future acceleration vocabulary.
- **Storage image / coherent / barriers / atomic contention** —
  Vulkan realities of the splat pass.
- **Distortion / CAC / mesh** — lens correction after warp
  (per-channel LUT, achromatic-verified here); mesh path = vertex
  warp, per-pixel path = ours.

### Metrics, tooling, methodology

- **FLIP** — alternating-image perceptual difference (color + edge
  feature pipelines; mean pooling; HDR variant black-safe). Our
  planned successor to abs-err counts. `pip install flip-evaluator`.
- **PSNR / SSIM / LPIPS / VMAF / JND** — older/single-number metrics;
  error maps rarely validated against perception (FLIP's critique).
- **Same-pose stale-vs-fresh pairs** — our ground-truth protocol
  (teleport steps; hand masks; plateau-motion masks for animation).
- **A/B toggles + staged debug views** — SHOW_*/DISABLE_* flags,
  MVOverlay modes, FSR3 stage views, OculusWorldDemo depth toggle.
- **RenderDoc / Nsight / RGP / apitrace** — frame-capture debugging
  (AppSW guide prescribes RenderDoc for MV-pass inspection).
- **Synthetic scenes + captured motion** (Mark's test-bed) —
  `owsim-*.py` conform to standard practice.

### Frame generation, extrapolation, reuse (2022–2026 additions)

- **Optical flow / OFA** — per-pixel apparent motion from frame pairs
  (Ada/Blackwell hardware units); catches shadows/reflections/UI that
  engine MVs miss; fails differently (distortion) than MVs.
- **Hudless + UI recomposition** — DLSS-FG inputs: pre-UI scene +
  UI alpha, composited after generation so text never warps.
- **MFG / Dynamic MFG** — DLSS 4/4.5: 3–5 generated frames per real
  one via split once-per-pair + per-frame halves; multiplier flexes
  with headroom. Latency still tracks the real rate.
- **Transformer SR/RR** — vision-transformer upscaler/denoiser
  (self-attention over space+time; 2x params over CNN); less ghosting,
  better thin geometry; failure-driven retraining as methodology.
- **AFMF + Fast Motion Response** — driver-level interpolation with no
  engine data; Repeat Frame (honest judder) vs Blended Frame (smooth
  garbage) on fast motion. Proof of what depth buys us.
- **MetalFX interpolator / denoised scaler / reactive mask** —
  Apple's two-frames+MV+depth interpolator; upscaler variant consuming
  albedo/specular/normal/roughness; mask channel for MV-less
  particles (FSR-reactive analog).
- **Late-warp / Frame Warp / guardband** — post-render warp to a
  re-sampled camera just before scanout; render wider than displayed
  (guardband) so rotation has source; HUD composited separately.
- **Occlusion motion vectors** (Zeng et al. 2021) — MVs repaired with
  stencil/normal/position reasoning; ExtraSS notes they still fail on
  complex backgrounds.
- **G-buffer guided warping** — ExtraSS: à-trous warp taps weighted by
  G-buffer similarity; the bilateral form of our agreement test.
- **Irradiance inpainting vs shading prediction** — ExtraNet's two
  tasks: synthesize where nothing corresponds, refine where something
  does. Our stretch-vs-black split, learned.
- **Zero-upsampling** — NSRR: project sparse samples to target res
  leaving zeros, so the net sees validity explicitly.
- **1spp (TSR)** — frames for a disocclusion to reach one sample per
  pixel: 1000/(ScreenPercentage² × fps). Stale-window accounting.
- **History resurrection** — TSR 5.4: persistent older frames reused
  when closer than previous (depth-reprojected compare).
- **Parallax heuristic** — TSR's depth+velocity disocclusion mask on
  async compute; placement trick for future warp-side heuristics.
- **Temporal injection** — Insomniac: jittered >half-pixel renders
  accumulated into 4K over frames; slight softness as known cost.
- **Flow projection (DAIN)** — colliding flows aggregated with
  w = 1/depth; soft blend beats hard min under depth uncertainty.
- **Reconnection / delayed / hybrid / sequential shift** — ReSTIR
  path-reuse mappings; visibility must be retested ("reconnecting
  through occlusions" = bias); sequential tries several, resampling
  picks. Our stale-foreground check is the same bias guard.
- **Reservoir / ReSTIR / GRIS / suffix ReSTIR** — spatiotemporal
  sample reuse with MIS receipts; temporal match by MVs; world-space
  (suffix) matching cures screen-space disocclusion lag.
- **SVGF (moments, edge-stopping)** — geometry-gated accumulation
  (depth/normal/meshID per-tap tests, escalate 2x2→3x3, then drop);
  spatial variance fallback post-disocclusion; the analytical
  reference filter.
- **FILM / MoMo** — learned large-motion interpolators NVIDIA uses as
  MFG baselines; no game data, offline-quality direction.
- **MV trust check (proposed)** — runtime validation that submitted
  MVs' camera-induced component agrees with the tracked pose delta;
  mismatch → depth-only fallback. Our answer to Meta's
  matrix-consistency contract.
- **Recommended MV/depth resolution** — EXT query
  (`XrFrameSynthesisConfigViewEXT`); Meta: above recommended buys
  nothing (368x400 on Quest 2). Our future low-res depth path knob.
- **Camera-motion-only MVs** — statics reuse depth instead of an MV
  pass (Unity 2022.3+); depth-only fallback is complete for statics.
- **`XrSpaceWarp` sample** — Oculus native sample app; known-good
  MV/depth generator for validating a future implementation.

## 2026-09-06 follow-up: fidelity-plan item 1 (agreement-weighted warp taps) — tested, rejected

Candidate: on the disagreement path, when the occlusion test does not
fire, replace the plain bilinear with a blend of the 2x2 taps weighted
by spatial bilinear weight x gaussian agreement
`exp(-(log(zt/z_exp)/sigma)^2)` per tap (ExtraSS bilateral warp in
miniature, fidelity-plan item 1). Sigmas 0.025/0.05/0.10, all three
shapes, six shifts, plus a close-range variant (Z_BG 5.0 -> 1.3, the
population the far-side-nearest live revert showed the 5x rig misses).
Matrix in `scripts/owsim-quad-cover.py` (`agree_sigma` path in
`reproject_final`, `item1` / `item1-close` blocks).

Result: converts cut to fringe and raises mass almost everywhere it
changes anything; all three sigmas saturate identically.

```
diamond  8mm: base bad16 fringe7 cut7 mass24.9 -> wtd bad10 fringe9 cut1 mass22.0
diamond 23mm: base bad50 fringe12 cut11 mass41.7 -> wtd bad39 fringe39 cut0 mass51.8
disc    23mm: base bad42 fringe10 cut2 mass33.7 -> wtd bad40 fringe40 cut0 mass51.3
bar     23mm: base bad30 fringe0 cut0 mass24.0 -> wtd bad30 fringe30 cut0 mass42.9
close-range diamond 23mm (Z_BG=1.3): base bad30 fringe0 mass8.1 -> wtd bad30 fringe30 mass27.0
50/130/150mm: bit-identical (agreement/occlusion/stretch paths untouched)
```

Mechanism (same family as the always-match rejection): `z_exp` inherits
the splat's half-texel foreground dilation, so agreement weights vote
foreground on pixels whose truth is background. With 5x fg/bg separation
any surface-distinguishing sigma saturates to a hard switch (all three
sigmas bit-identical); with close ratios it hardens sub-threshold soft
mixes into over-threshold fringe (close-range 23mm: mass 8.1 -> 27.0,
fringe 0 -> 30). The sigma axis has no useful middle: sigmas small
enough to distinguish surfaces saturate, sigmas large enough to stay
soft stop distinguishing and degrade to bilinear. Position-weighted
bilinear remains the best static estimate without coverage data. No
shader change; the plan gate ("any halo regression kills it") fires.
