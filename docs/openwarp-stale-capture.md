# Capturing stale openwarp frames (methodology)

How to capture the interesting frame — stale source reprojected to the
new warp pose — and how to tell it apart from the three lookalikes.
Prerequisite workflow (service/app startup, readiness signals) stays in
`AGENTS.md`; this file is only the capture/analysis method.

## The window is 1–2 frames — identify, don't guess

A live app re-renders within ~1–3 frames of pose delivery, so per burst
frame after a teleport, diffed against the settled fresh frame **masked
to hand-colored pixels** (pink: R>200, G/B 80–180; green: G>200, R/B
80–180 — the cube spins and confounds every global metric):

- thousands of px differing → pre-delivery (warp still at old pose,
  full parallax vs fresh),
- hundreds of px → **stale** (warp new, source old — the frame you want),
- ~0 → settled identity warp, measures nothing.

Verified: 15 cm lateral jump at default pacing → stale = burst01–02 only
(297 hand px), burst03+ hand-identical to fresh. Analyzing an arbitrary
burst index (e.g. burst10) almost certainly measures a fresh identity
frame — this exact mistake invalidated a full round of "edge" readings
(see `docs/openwarp-edge-sharpness.md`).

Poses must be teleports (step changes); the warp pose is then either A
or B, never in between, so edge-scale diff unambiguously means stale.

## Scripts

- `scripts/capture-stale-hunt.py` — stop-on-hit hunter for blackout-type
  events (green collapse + pink present), saves hit + same-pose fresh.
  See `AGENTS.md` for the recipe.
- `scripts/capture-stale-burst.py` — full-burst variant for offline edge
  analysis: arbitrary absolute poses **including translation**
  (`OUTDIR Ax AQx AQy AQz AQw Bx BQx BQy BQz BQw`), one persistent
  remote-client connection (no per-send TCP churn), dense
  `import -window Monado -depth 8 rgb:-` grabs (~21 ms/shot) for 2 s
  per jump direction, then a 4 s settle (cube spins 0.25 rot/s → 360°,
  same orientation) plus the settled fresh frame. Pick stale frames
  offline with the hand-diff timeline above.
  ```sh
  python3 scripts/capture-stale-burst.py /tmp/frayA 0 0 0 0 1 0.15 0 0 0 1
  ```
- `scripts/analyze-warp.py` — FFT-aligned halo/missing/smear metrics for
  a (warped, fresh) pair; `--crop x,y,w,h` restricts to static geometry.

Masks that work: hand-color (above) for diff timelines; plateau-motion
mask (pixels changing across fixed-pose burst frames = spinning cube)
for excluding animation from stale-vs-fresh diffs.

## Analysis patterns

- Overlay: red = stale-bright/fresh-dark (halo), green =
  stale-dark/fresh-bright (missing). Classify stale colors at missing
  pixels (black vs dim mix) — black = hole/occlusion verdict, dim =
  bilinear band.
- Edge profiles: scanline across a silhouette per channel (R/G/B
  tracking each other = achromatic, sharp; separated R/B ramps =
  lens-CA, not warp).
- Ghost/double numeric test (no eyeballing): count pink/green
  hand-joint pixels per eye at the same pose — a double view shows
  counts far above the settled baseline (verified: 3281 vs 1768
  green, ~1.8x). A chamfer histogram of warped-only mask pixels to the
  fresh mask then tells rim from copy: a thin 1 px peak is filtering
  residue, while a second mass ≥20 px away at the old head-pose
  position (verified: 125 px offset for 0.3 m on close joints) is a
  stale second copy. Gate on hands-present first (zero color pixels
  usually means legitimate out-of-view clipping, not blackout), and
  only trust count deltas on static geometry over short windows —
  spinning occlusion swings joint counts by ±1400 at identical poses,
  and 1 s capture intervals land on 90°-symmetric cube orientations
  that hide rotation entirely.
- Transition width (10–90% count) is only meaningful on settled or
  same-pose pairs — during motion the edge legitimately sweeps.

## Attribution (which stage dropped a black region)

Re-capture the same stale pose with `MONADO_OPENWARP_*` env on
`monado-service` (parsed in `comp_compositor.c:84-97`):

- `SHOW_OCCLUSION=1` tints occlusion rejects (magenta),
- `SHOW_STRETCH=1` tints stretch fills (green),
- `SHOW_HOLES=1` paints true coverage holes solid red (no valid depth
  even after the stretch search) — splits "no data" from "filled" in
  one glance; combine with `DISABLE_STRETCH=1` for raw splat coverage,
- `DEPTH_AS_COLOR=1` shows warp depth (magenta holes = no splat arrived),
- `DISABLE_OCCLUSION=1` / `DISABLE_STRETCH=1` bisect the fix,
- `DEPTH_AS_COLOR` doubles as a dispatch probe: colormap output proves
  `openwarp.comp` runs.

Decision tree: magenta → nearer source texel covers it (check the
occluder is real at the *source* pose against same-pose fresh); green →
stretch ran (no valid source sample); valid background-looking depth →
background splat legitimately won; magenta hole → nothing splatted
(out of frustum). A black patch that is also black in fresh is scene
truth or honest out-of-data, not a warp bug.

Caveat on `FREEZE_SOURCE=1`: it is supposed to hold one source frame
while the warp tracks, but a 2026-09 attempt with the playground
produced frames identical to live captures — whether the snapshot
re-captured every frame (cycling swapchain indices) or poses didn't
apply was not isolated. Verify before relying on it: after a head move
the frozen frame MUST differ from a live frame at the new pose.

## Pitfalls (all verified the hard way)

- Evenly spaced screenshots are fresh by construction (the app
  resubmits ~100x between 5 s shots at 50 ms pacing).
- `PLAYGROUND_FRAME_MS >= 100` wedges the app in this sandbox (0% CPU,
  stuck in `do_wait`, zero submits). Hunt at default 50 ms pacing.
- One `monado-remote-client` per `send` churns TCP and appears able to
  wedge the session; use one persistent connection (stdin pipe, read
  the `ok`s — the `RC` class in the hunt scripts).
- `kill -STOP` on the app freezes the mirror too (compositor presents
  only on client commits) and risks the frame-pacing stall — restart
  fresh instead of freeze/thaw cycles.
- Static mirror ≠ wedged app (static scene is supposed to be
  bit-identical); liveness = one head move + md5 change.
- Different-pose comparisons measure parallax, not warp error — only
  same-pose stale-vs-fresh pairs count.

## Environment status (2026-09-06)

Mirror capture is **unavailable**: a ~02:05 UTC system update broke
ImageMagick (`import` dies at startup with `missing an image filename`,
even `import logo:`), `xwd`/`scrot` still fail BadMatch as before, and
`ffmpeg x11grab` reports `screen size 0x0`. Do not burn sessions
retrying capture commands. Related: the same update bumped the system
SDL2 SONAME (.70 → .72); re-run `cmake -G Ninja -S . -B build` in
`monado/` if the service target fails to link with a missing
`libSDL2-2.0.so.0.3200.70` error.
