# Openwarp depth-reprojection port — progress tracking

## Goal
Port "openwarp" depth-based reprojection onto current monado `upstream/main` as a
clean re-implementation in the modern compositor.

## Guidance (from user)
- Do NOT preserve old code; it is an example of a previous attempt. Rewrite freely,
  focus on clean code.
- Verify depth actually arrives inside the integrated openwarp code.
- Use openxr-simple-playground (it submits depth) + debug code; use Vulkan
  validation layers.

## What the old code did (for reference only)
- 11 commits on `bones/bones/openwarp` (2023), hooked into the OLD compositor
  renderer (`renderer_get_view_projection` + `comp_layer_renderer_draw`,
  replaced by `comp_reproject`). Linked to `comp_warp.c/h` + `openwarp_mesh.{vert,frag}`.
- Algorithm: a uniform screen-space grid mesh whose vertices push the depth of each
  point back into world space, then re-project at the *current* (timewarped) pose.
  Replaces the flat plane-approximation timewarp with per-pixel (per-vertex) depth
  reprojection -> correct parallax under translation.

## Modern architecture (current main) — key facts
- Compositor gfx path fully rewritten: two-stage
  (1) layer *squash* into per-view scratch color image,
  (2) *distortion* via a lens-distortion mesh that samples the scratch and does a
      plane-approx timewarp.
  Entry: `chl_frame_state_gfx_default_pipeline` -> `comp_render_gfx_dispatch`
  (`src/xrt/compositor/util/comp_render_gfx.c`) -> `render_gfx`
  (`src/xrt/auxiliary/render/render_gfx.c`).
- Fast path: when the single layer is a projection (or projection-depth) layer,
  `crg_distortion_fast_path` skips squash and samples the swapchain color directly.
- Depth layers (`XRT_LAYER_PROJECTION_DEPTH`) have per-view color + depth swapchains;
  depth currently DISCARDED in the gfx path (`do_projection_layer` ignores `dvd`).
- Shaders in `src/xrt/auxiliary/render/shaders/` (compiled by glslang to .h arrays).
- `render_gfx_render_pass` (render_interface.h:697) owns all pipelines per pass.
  Mesh pipelines created in `render_gfx_render_pass_init` via `create_mesh_pipeline`.
- Mesh geometry: per-view lens VBO/IBO in `render_resources.mesh` (based on `xdev`).
- Projection-depth data: `xrt_layer_projection_depth_data { v[views], d[views], chroma_key }`;
  depth swapchain per view at `sc_array[XRT_MAX_VIEWS + i]`
  (`comp_layer_get_depth_swapchain`).
- Depth-layer fields: `sub`, `min_depth`, `max_depth`, `near_z`, `far_z`.

## DESIGN DECISION (Design A) — Fast-path depth-warp mesh
Add a clean, self-contained **warp pass** that renders a uniform screen grid deformed
by depth reprojection, used when a single `XRT_LAYER_PROJECTION_DEPTH` layer is the
whole frame (fast path). This is the natural place where depth + color are both
available, and it implements the openwarp algorithm cleanly within render_gfx style.

New module `render_warp` (in `src/xrt/auxiliary/render/`) that owns: warp UBO +
descriptor layout + pipeline layout + pipelines + uniform grid VBO/IBO + a small
descriptor pool (2 preallocated sets, one per view, updated per draw). Provides:
- `render_warp_init/warp_fini`
- `render_warp_render(w, vk, cmd, view_index, warp_data)` — called inside the active
  distortion render pass; writes the warp descriptor set and draws the grid.

Warp data per view:
- color+depth image views + samplers (from the projection-depth layer swapchains)
- `u_renderInverseP` = inverse(render projection) [layer fov]
- `u_renderInverseV` = world-from-pose(render pose) [layer pose]
- `u_warpVP` = warp_projection * warp_view [display/scanout pose]
- `bleedRadius`, `edgeTolerance` (outlier rejection)

### Files
NEW:
- `src/xrt/auxiliary/render/shaders/openwarp_mesh.vert`
- `src/xrt/auxiliary/render/shaders/openwarp_mesh.frag`
- `src/xrt/auxiliary/render/render_warp.h`
- `src/xrt/auxiliary/render/render_warp.c`

EDIT:
- `.../shaders/render_shaders_interface.h` (add modules
  `openwarp_mesh_vert/frag`, expose SHADER count if needed)
- `.../shaders/render_shaders.c` (LOAD/FINI)
- `.../shaders/CMakeLists.txt` (add to SHADERS)
- `src/xrt/auxiliary/render/CMakeLists.txt` (add render_warp.c)
- `src/xrt/auxiliary/render/render_interface.h` (add `render_warp` include-point / decls,
  or keep render_warp self-contained with its own header)
- `src/xrt/compositor/util/comp_render_gfx.c` (`crg_distortion_depth_warp` fast path)
- `openxr-simple-playground/main.cpp` (debug/test hook to submit only the projection
  depth layer so the fast path + warp is exercised; verify depth reaches warp)

## Verification plan
- Build monado, run monado-service (remote cfg) + validation layers, run playground.
- Confirm depth reaches the warp: add a temporary debug assertion/log or a shader/UBO
  debug output that reports depth is non-trivial. Simplest: log in the warp CPU path
  when a depth layer is used, and/or visualize via a debug color based on depth.

## Current step
Deep investigation complete. Begin implementation (shaders first, then render_warp,
then integration, then playground hook).

## IMPLEMENTATION - DONE (verified)
The warp pass was integrated directly into the existing `render_gfx`/`comp_render_gfx`
path (rather than a separate `render_warp` module) as a fast-path branch:

- Shaders `openwarp_mesh.vert/frag` (UBO binding 0, color 1, depth 2). No Y-flip:
  `gl_Position` stays in Vulkan clip space.
- `render_interface.h`: `render_gfx_warp_ubo_data`
  (`u_renderInverseP/V`, `u_warpVP`, `post_transform`, `bleedRadius`, `edgeTolerance`),
  `render_resources.warp`, `render_gfx_render_pass.warp`, decls for
  `render_gfx_warp_alloc_and_write` + `render_gfx_warp_draw`.
- `render_resources.c`: warp descriptor-set layout (3 bindings), 64x64 grid, per-view
  UBOs, persistent per-view descriptor sets.
- `render_gfx.c`: `create_warp_pipeline`, `update_warp_descriptor_set`,
  `render_gfx_warp_alloc_and_write`, `render_gfx_warp_draw`; pass init/fini wiring.
- `comp_render_gfx.c`: `get_layer_depth_image` + `crg_distortion_depth_warp`
  (per-view color+depth image views, inverse/render/warp matrices,
  `render_gfx_begin_target`/`begin_view`/`warp_draw`/`end_view`/`end_target`);
  dispatch branch `fast_path && XRT_LAYER_PROJECTION_DEPTH` -> depth warp.
- Playground: `PLAYGROUND_NO_QUAD=1` (submit only the projection-depth layer) and
  `PLAYGROUND_NO_QUIT=1` (ignore SDL_QUIT so frames keep committing) env test hooks.

### Verified live e2e
`crg_distortion_depth_warp` is reached and a non-null depth image view from the layer's
depth swapchain is in use. Signal (once per run, `U_LOG_I`, needs XRT_LOG=i):

```
INFO [crg_distortion_depth_warp] openwarp: depth-based warp active, view=0 color=0x... depth=0x...
```

E2E runner: `/tmp/ow-start.sh` (starts monado + playground detached via setsid, then you
poll the logs). Canonical repro:

```sh
export XR_RUNTIME_JSON=monado/build/openxr_monado-dev.json
# monado:
env P_OVERRIDE_ACTIVE_CONFIG=remote XRT_COMPOSITOR_FORCE_XCB=1 XRT_NO_STDIN=1 \
    XRT_COMPOSITOR_COMPUTE=false XRT_LOG=i \
    monado/build/src/xrt/targets/service/monado-service
# app:
env PLAYGROUND_NO_QUAD=1 PLAYGROUND_NO_QUIT=1 \
    openxr-simple-playground/build/openxr-playground
```

### Gotchas (record these - easy to misread!)
1. **Monado uses the COMPUTE shader renderer by default on Linux desktop**
   (`USE_COMPUTE_DEFAULT = true`, `comp_settings.c:17`). The openwarp warp lives in the
   **GFX** path, so `comp_render_gfx_dispatch` is never called unless you force
   `XRT_COMPOSITOR_COMPUTE=false` when launching monado-service. Without it, "frames are
   committed but dispatch never fires".
2. **`U_LOG_I`/`U_LOG_D` are filtered at the default `XRT_LOG` (WARN) level**
   (`u_logging.c:66`). The warp confirmation log is `U_LOG_I`; set `XRT_LOG=i` (or put a
   `U_LOG_W`) or it silently never shows even though the code runs.
3. The fast-path projection-depth branch needs the app to actually submit a
   projection-depth layer. `PLAYGROUND_NO_QUAD=1` drops the quad so only the depth layer
   is submitted (`fast_path=1 layer_count=1 type=1`, where type 1 ==
   `XRT_LAYER_PROJECTION_DEPTH`).
4. The playground reads no stdin; the Old AGENTS.md `(sleep infinity) |` pipe is
   unnecessary for it. Start it detached with `< /dev/null`.
