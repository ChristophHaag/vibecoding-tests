# OpenXR Debug GUI Layer

An OpenXR API layer that opens a standalone Dear ImGui window showing live debugging information for any OpenXR application. Runtime-agnostic — works with any conformant OpenXR runtime.

## Features

- **Action & ActionSet inspector** — lists all created action sets, actions, types, subaction paths, and attachment state
- **Suggested bindings viewer** — shows all interaction profile bindings the app registered, grouped by profile
- **Live action state** — real-time display of all action values (boolean, float, vector2f, pose) with active/changed indicators, updated every `xrSyncActions`
- **Active interaction profiles** — shows which controller profile is bound per subaction path
- **3D space visualization** — interactive arcball scene rendering all reference spaces and action spaces as labeled coordinate frames with RGB axes
- **HMD view frustums** — `xrLocateViews` results shown as orange wireframe FOV frustums with near-plane rectangles
- **Composition layer inspector** — shows recently submitted composition layers, per-layer metadata, sub-image rects, swapchain/image indices, and depth-chain info
- **OpenGL composition previews** — captures throttled previews of OpenGL/EGL color swapchain images on release, caches them per swapchain image, and displays them inside the composition layer inspector
- **Vulkan composition previews** — submits throttled preview captures of Vulkan color swapchain images on release using the application's Vulkan device and queue, then harvests the staged thumbnails asynchronously for display in the composition layer inspector
- **Preview inspect mode** — click a preview to toggle from fit view into actual-pixel inspection with wheel zoom, reset controls, and RGBA pixel hover readout
- **Retained live layer view** — the inspector keeps recently missing layers around for a few frames and marks them stale, reducing panel flicker when apps omit a layer from one `xrEndFrame`
- **Label anti-overlap** — overlapping space labels are automatically nudged apart
- **Performance observatory** — frame lifecycle timing with CLOCK_MONOTONIC instrumentation:
  - **Stacked-bar breakdown** — per-frame phase breakdown (xrWaitFrame / xrBeginFrame / app work / swapchain ops / xrEndFrame) with Budget or Percentage scale toggle and always-visible frame number labels
  - **Budget scale modes** — "Period" reference (1×budget = full width) or "Max frame" reference (longest frame in window fills width, better for spotting relative differences when frames vary wildly)
  - **Extreme outlier indicators** — frames exceeding 2× budget show "Nx" multiplier markers (e.g., `8x`)
  - **Real-time timeline** — zoomable swim-lane view with wall-clock time axis showing pipeline overlap between frames, VSync grid, predicted display time markers, and per-row frame number labels. Adjustable zoom (1–20×) and row height (8–32 px) with horizontal scrolling
  - **CPU headroom** — percentage of frame budget remaining after all non-wait work
  - **Over-budget indicators** — red `!` marker on frames that exceed the display period
  - **8 individual timing graphs** with min/max/avg overlays and frame-number hover tooltips showing current sample and frame-to-frame transition (total, wait, app work, end frame, swapchain, begin frame, sync actions, locate views)
  - **Pause/resume** with frozen snapshot, configurable history window (1–30 s)
- **Frame info overlay** — frame count, predicted display time (with wall-clock HH:MM:SS.mmm), refresh rate, should-render flag, session state, display time delta graph
- **Persistent layout** — ImGui docking layout saved to `~/.config/openxr_debug_gui/imgui.ini`

## Intercepted Functions

`xrCreateSession`, `xrDestroySession`, `xrBeginSession`, `xrEndSession`, `xrPollEvent`, `xrCreateActionSet`, `xrCreateAction`, `xrSuggestInteractionProfileBindings`, `xrAttachSessionActionSets`, `xrSyncActions`, `xrGetActionState*`, `xrGetCurrentInteractionProfile`, `xrCreateReferenceSpace`, `xrCreateActionSpace`, `xrDestroySpace`, `xrLocateSpace`, `xrLocateViews`, `xrWaitFrame`, `xrBeginFrame`, `xrEndFrame`, `xrCreateSwapchain`, `xrEnumerateSwapchainImages`, `xrDestroySwapchain`, `xrAcquireSwapchainImage`, `xrWaitSwapchainImage`, `xrReleaseSwapchainImage`, `xrStringToPath`, `xrPathToString`, `xrDestroyInstance`, `xrDestroyActionSet`, `xrDestroyAction`

## Building

```sh
cmake -G Ninja -B build
cmake --build build -j$(nproc)
```

The default build type is **RelWithDebInfo** — this is important because ImGui's draw routines are unusably slow at `-O0`.

Requires: CMake ≥ 3.20, C++17 compiler, OpenGL, Vulkan, X11 dev headers.  
SDL3, Dear ImGui, and OpenXR headers are fetched automatically via FetchContent.

Output:
- `build/libXrApiLayer_debug_gui.so`
- `build/openxr-api-layer/XrApiLayer_debug_gui.json`
- `build/xr-with-debug-gui.sh`

## Usage

### Wrapper script (recommended)

```sh
# Basic usage
build/xr-with-debug-gui.sh my_xr_app --app-args

# With options
build/xr-with-debug-gui.sh --fps 60 my_xr_app
build/xr-with-debug-gui.sh --no-gui my_xr_app   # interceptors only, no window
```

The script is callable from any directory — it has the build path baked in.

### Manual environment setup

```sh
export XR_ENABLE_API_LAYERS=XR_APILAYER_DEBUG_gui
export XR_API_LAYER_PATH=/path/to/debug_layer/build/openxr-api-layer
my_xr_app
```

### Environment variables

| Variable | Effect |
|---|---|
| `XR_DEBUG_GUI_DISABLE=1` | Load layer but skip GUI window (interceptors still log to stderr) |
| `XR_DEBUG_GUI_FPS=N` | GUI render rate, 1–240 (default: 30) |
| `XR_DEBUG_GUI_PREVIEW_INTERVAL=N` | Capture every `N`th eligible previewable swapchain release; `0` disables preview capture |
| `XR_DEBUG_GUI_PREVIEW_MAX_EDGE=N` | Maximum thumbnail edge length before downscaling (default: 320) |
| `XR_DEBUG_GUI_PREVIEW_OPENGL_INFLIGHT=N` | Number of OpenGL preview capture slots kept in flight before new submits are skipped (default: 4) |
| `XR_DEBUG_GUI_PREVIEW_VULKAN_INFLIGHT=N` | Number of Vulkan preview capture slots kept in flight before new submits are skipped (default: 4) |
| `XR_DEBUG_GUI_PREVIEW_LOG=1` | Log first successful preview capture and distinct preview skip/failure reasons per swapchain |
| `XR_DEBUG_GUI_PREVIEW_LOG=2` | Log every preview attempt and skip; useful only for short debugging runs |

Legacy `XR_DEBUG_GUI_GL_PREVIEW_*` variable names are still accepted for compatibility.

## Composition Layer Preview Design

- The layer deep-copies `xrEndFrame` layer submissions into tracked state so the GUI never depends on application-owned pointers after the call returns.
- OpenGL preview capture now performs only the source-image blit and PBO readback submission on the application thread before `xrReleaseSwapchainImage`. Fence polling, PBO mapping, and CPU-side thumbnail copies are harvested asynchronously on later releases.
- Vulkan preview capture still records work on the application thread before `xrReleaseSwapchainImage`, but the steady-state path now only submits GPU work there. Fence polling, staging invalidation, and CPU-side thumbnail copies are harvested asynchronously on later releases.
- The preview path resolves GL helper entry points directly from EGL or GLX instead of SDL, because the app thread is not required to initialize SDL.
- Captured previews are cached per `(swapchain, image index, array index)` instead of only keeping the newest preview for a swapchain. This avoids flicker when apps rotate through swapchain images.
- The Composition Layers panel renders a short retained live set of recent layers instead of only the last frame. Layers missing for a few frames are marked stale before being dropped.
- The Composition Layers panel now shows preview app-thread time, async finalize time, ready latency, and in-flight depth so capture overhead can be compared directly against the frame timing graphs.
- Current preview support includes desktop OpenGL, `XR_MNDX_egl_enable`, `XR_KHR_vulkan_enable`, and `XR_KHR_vulkan_enable2` sessions. Other graphics APIs still show metadata without image content.
- Current preview limitations:
  - Cube and other multi-face swapchains are metadata-only.
  - Non-color swapchains are metadata-only.
  - Vulkan previews currently require sampled, single-sample images.
  - Array swapchains currently preview layer 0 only.
  - Preview capture is intended for debugging, not zero-copy production display.

## Preview Benchmarking

- Use the Composition Layers panel to compare preview `attempts`, `success`, `skipped`, app-thread time, finalize time, and ready latency for a live swapchain.
- Use the Performance panel to compare `xrReleaseSwapchainImage`, swapchain, app-work, and `xrEndFrame` timing with preview capture disabled vs. `XR_DEBUG_GUI_PREVIEW_INTERVAL=1`.
- For OpenGL, the key steady-state numbers are preview app-thread time and skipped submissions. If the app-thread time is low but skips rise, increase `XR_DEBUG_GUI_PREVIEW_OPENGL_INFLIGHT` before assuming the path itself is too expensive.
- For Vulkan, the key steady-state number is preview app-thread time. In a healthy async path it should stay well below the deferred finalize and ready-latency numbers.

## Vulkan Validation Smoke Test

The Vulkan preview path is smoke-tested with the system `hello_xr` binary and the
Khronos validation layer enabled. Start Monado first, then run:

```sh
P_OVERRIDE_ACTIVE_CONFIG=remote XRT_COMPOSITOR_FORCE_XCB=1 XRT_NO_STDIN=1 \
    /home/haagch-demo/projects/vibecoding-tests/monado/build/src/xrt/targets/service/monado-service \
    >/tmp/monado.log 2>&1 &
MONADO_PID=$!
until grep -q "Listening on port '4242'" /tmp/monado.log 2>/dev/null; do sleep 0.2; done

sleep infinity | env \
    VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
    XR_DEBUG_GUI_PREVIEW_LOG=1 \
    XR_DEBUG_GUI_PREVIEW_INTERVAL=1 \
    stdbuf -oL -eL \
    build/xr-with-debug-gui.sh --no-gui /usr/bin/hello_xr -G Vulkan2 \
    2>&1 | tee /tmp/hello_xr_vulkan_validation.log

rg -n "Captured Vulkan preview|VUID|Validation Error|ERROR:" /tmp/hello_xr_vulkan_validation.log
kill $MONADO_PID
```

Successful runs reach `XR_SESSION_STATE_FOCUSED`, log `Captured Vulkan preview`
for both eye swapchains, and do not emit any `VUID`, `Validation Error`, or
`ERROR:` lines. The same recipe also works with `hello_xr -G Vulkan`.

## World-Space Layer Rendering Notes

- The 3D Spaces panel already has the math and GL infrastructure needed to draw extra world-space geometry.
- Composition layer content should be treated as visualization geometry derived from tracked layer submissions, not as part of the application render path.
- The scene should pick one consistent root space, then resolve tracked spaces, view poses, and layer spaces into that root before drawing. Mixing raw `xrLocateSpace` results from different base spaces will misplace geometry.
- Projection layers can be visualized by rendering their captured sub-images onto geometry placed at the far end of each tracked frustum.
- Quad layers can be visualized by rendering their captured sub-image onto a world-space rectangle transformed by the layer pose and size.
- Because layers may be omitted from some `xrEndFrame` calls, the 3D visualization should use the same retained-live-layer model as the Composition Layers panel rather than dropping geometry immediately.
- The 2D and 3D panels should reuse the same GUI-side preview texture cache so the same captured thumbnail is not uploaded twice when both panels are open.
- The 3D scene needs a depth attachment once textured layer quads are added, otherwise world-space previews sort incorrectly against each other and against the line overlays.

## Architecture

```
layer_main.cpp          Entry point: negotiate, xrCreateApiLayerInstance, xrGetInstanceProcAddr
dispatch.h/.cpp         X-macro interceptor registry + NextDispatch struct
instance_data.h/.cpp    Per-instance state + global handle→InstanceData maps
tracked_state.h         All tracked object structs (actions, spaces, sessions, views, swapchains, composition layers)
interceptors/           One file per domain (instance, session, actions, spaces, frame)
gui/                    SDL3 + ImGui on dedicated thread (gui_main, gui_actions, gui_layers, gui_spaces, gui_perf)
gui/gui_common.h        Math (Vec3, Mat4, ArcballCamera, project_to_screen)
gui/gui_perf.h/.cpp     Performance observatory: stacked bars, real-time timeline, timing graphs
```

## Notes

- The GUI runs on a dedicated thread with its own SDL3/GL context — does not interfere with the app's rendering
- SDL3 is linked statically with all symbols hidden (`-Wl,--exclude-libs,ALL`) to avoid conflicts with apps using SDL2
- Only `xrNegotiateLoaderApiLayerInterface` is exported from the .so
- Newly added panels should define a fallback dock target with `ImGui::SetNextWindowDockID(..., ImGuiCond_Appearing)` because persisted `imgui.ini` layouts bypass the first-run dock builder.
- The Composition Layers panel uses retained previews and retained recent-layer state to reduce flicker from rotating swapchain indices and temporarily omitted layers.
- Use the `build/xr-with-debug-gui.sh` wrapper for smoke tests; it points `XR_API_LAYER_PATH` at the manifest-only Ninja build directory and avoids loader warnings from unrelated files.
- Layout resets: delete `~/.config/openxr_debug_gui/imgui.ini`

## Licensing

This repository is presented as 100% AI-written. That also means the provenance
of any given line is fundamentally uncertain: an LLM cannot reliably prove that
it did not reproduce material derived from copyleft, proprietary, or otherwise
incompatible sources.

For that reason, any MIT notices in individual files should not be treated as a
trustworthy provenance guarantee. In plain terms: they claim an MIT licensing
state that cannot actually be verified from the way this repository was
produced.

The intended dedication for this repository is public domain, but with an
explicit caveat that the true upstream licensing status of generated content is
unknown. If you need clean provenance or a defensible license chain, do not rely
on this repository without performing your own review and replacement work.
