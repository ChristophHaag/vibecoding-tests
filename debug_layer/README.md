# OpenXR Debug GUI Layer

An OpenXR API layer that opens a standalone Dear ImGui window showing live debugging information for any OpenXR application. Runtime-agnostic — works with any conformant OpenXR runtime.

## Features

- **Action & ActionSet inspector** — lists all created action sets, actions, types, subaction paths, and attachment state
- **Suggested bindings viewer** — shows all interaction profile bindings the app registered, grouped by profile
- **Live action state** — real-time display of all action values (boolean, float, vector2f, pose) with active/changed indicators, updated every `xrSyncActions`
- **Active interaction profiles** — shows which controller profile is bound per subaction path
- **3D space visualization** — interactive arcball scene rendering all reference spaces and action spaces as labeled coordinate frames with RGB axes
- **HMD view frustums** — `xrLocateViews` results shown as orange wireframe FOV frustums with near-plane rectangles
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

`xrCreateSession`, `xrDestroySession`, `xrBeginSession`, `xrEndSession`, `xrPollEvent`, `xrCreateActionSet`, `xrCreateAction`, `xrSuggestInteractionProfileBindings`, `xrAttachSessionActionSets`, `xrSyncActions`, `xrGetActionState*`, `xrGetCurrentInteractionProfile`, `xrCreateReferenceSpace`, `xrCreateActionSpace`, `xrDestroySpace`, `xrLocateSpace`, `xrLocateViews`, `xrWaitFrame`, `xrBeginFrame`, `xrEndFrame`, `xrCreateSwapchain`, `xrDestroySwapchain`, `xrAcquireSwapchainImage`, `xrWaitSwapchainImage`, `xrReleaseSwapchainImage`, `xrStringToPath`, `xrPathToString`, `xrDestroyInstance`, `xrDestroyActionSet`, `xrDestroyAction`

## Building

```sh
cmake -B build
cmake --build build -j$(nproc)
```

The default build type is **RelWithDebInfo** — this is important because ImGui's draw routines are unusably slow at `-O0`.

Requires: CMake ≥ 3.20, C++17 compiler, OpenGL, X11 dev headers.  
SDL3, Dear ImGui, and OpenXR headers are fetched automatically via FetchContent.

Output:
- `build/libXrApiLayer_debug_gui.so`
- `build/XrApiLayer_debug_gui.json`
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
export XR_API_LAYER_PATH=/path/to/debug_layer/build
my_xr_app
```

### Environment variables

| Variable | Effect |
|---|---|
| `XR_DEBUG_GUI_DISABLE=1` | Load layer but skip GUI window (interceptors still log to stderr) |
| `XR_DEBUG_GUI_FPS=N` | GUI render rate, 1–240 (default: 30) |

## Architecture

```
layer_main.cpp          Entry point: negotiate, xrCreateApiLayerInstance, xrGetInstanceProcAddr
dispatch.h/.cpp         X-macro interceptor registry + NextDispatch struct
instance_data.h/.cpp    Per-instance state + global handle→InstanceData maps
tracked_state.h         All tracked object structs (actions, spaces, sessions, views)
interceptors/           One file per domain (instance, session, actions, spaces, frame)
gui/                    SDL3 + ImGui on dedicated thread (gui_main, gui_actions, gui_spaces, gui_perf)
gui/gui_common.h        Math (Vec3, Mat4, ArcballCamera, project_to_screen)
gui/gui_perf.h/.cpp     Performance observatory: stacked bars, real-time timeline, timing graphs
```

## Notes

- The GUI runs on a dedicated thread with its own SDL3/GL context — does not interfere with the app's rendering
- SDL3 is linked statically with all symbols hidden (`-Wl,--exclude-libs,ALL`) to avoid conflicts with apps using SDL2
- Only `xrNegotiateLoaderApiLayerInterface` is exported from the .so
- Layout resets: delete `~/.config/openxr_debug_gui/imgui.ini`
