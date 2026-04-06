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
- **Frame info overlay** — frame count, predicted display time, refresh rate, should-render flag, session state
- **Persistent layout** — ImGui docking layout saved to `~/.config/openxr_debug_gui/imgui.ini`

## Intercepted Functions

`xrCreateSession`, `xrDestroySession`, `xrBeginSession`, `xrEndSession`, `xrCreateActionSet`, `xrCreateAction`, `xrSuggestInteractionProfileBindings`, `xrAttachSessionActionSets`, `xrSyncActions`, `xrGetActionState*`, `xrGetCurrentInteractionProfile`, `xrCreateReferenceSpace`, `xrCreateActionSpace`, `xrDestroySpace`, `xrLocateSpace`, `xrLocateViews`, `xrWaitFrame`, `xrBeginFrame`, `xrEndFrame`, `xrStringToPath`, `xrPathToString`, `xrDestroyInstance`, `xrDestroyActionSet`, `xrDestroyAction`

## Building

```sh
cmake -B build
cmake --build build -j$(nproc)
```

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
gui/                    SDL3 + ImGui on dedicated thread (gui_main, gui_actions, gui_spaces)
gui/gui_common.h        Math (Vec3, Mat4, ArcballCamera, project_to_screen)
```

## Notes

- The GUI runs on a dedicated thread with its own SDL3/GL context — does not interfere with the app's rendering
- SDL3 is linked statically with all symbols hidden (`-Wl,--exclude-libs,ALL`) to avoid conflicts with apps using SDL2
- Only `xrNegotiateLoaderApiLayerInterface` is exported from the .so
- Layout resets: delete `~/.config/openxr_debug_gui/imgui.ini`
