# AI Agent Guide — debug_layer

## Repository layout

```
debug_layer/
├── CMakeLists.txt                 Build system (FetchContent for SDL3, ImGui, OpenXR headers)
├── XrApiLayer_debug_gui.json      Layer manifest (loader reads this)
├── XrApiLayer_debug_gui.map       Linker version script (exports only negotiate symbol)
├── xr-with-debug-gui.sh.in        CMake template → build/xr-with-debug-gui.sh
├── README.md
├── DECISIONS.md                   Architecture rationale
├── PLAN.md                        Original feature plan
└── src/
    ├── layer_main.cpp             Entry: negotiate + xrCreateApiLayerInstance + xrGetInstanceProcAddr
    ├── dispatch.h                 X-macro LIST_INTERCEPTED_FUNCTIONS + NextDispatch struct
    ├── dispatch.cpp               PopulateNextDispatch, RegisterAllInterceptors, GetInterceptor
    ├── instance_data.h            InstanceData struct + global handle→data maps
    ├── instance_data.cpp          Map operations (Register/Unregister/Get for each handle type)
    ├── tracked_state.h            All tracked structs: TrackedActionSet, TrackedAction, TrackedSpace,
    │                              TrackedSession, TrackedViewPose, TrackedFrameState, etc.
    ├── interceptors/
    │   ├── instance.cpp           xrDestroyInstance, xrStringToPath, xrPathToString
    │   ├── session.cpp            xrCreate/Destroy/Begin/EndSession — launches GUI from xrCreateSession
    │   ├── actions.cpp            Action sets, actions, bindings, sync, state queries, profiles
    │   ├── spaces.cpp             Reference/action spaces, xrLocateSpace, xrLocateViews
    │   └── frame.cpp              xrWaitFrame, xrBeginFrame, xrEndFrame
    └── gui/
        ├── gui_main.h/.cpp        SDL3 init + ImGui setup on dedicated thread, DockBuilder layout
        ├── gui_actions.h/.cpp      Actions/bindings/live-state panels
        ├── gui_spaces.h/.cpp       3D FBO scene: grid, axes, space frames, view frustums, labels
        └── gui_common.h           Math: Vec3, Vec4, Mat4, ArcballCamera, project_to_screen
```

## Building

```sh
cmake -B build
cmake --build build -j$(nproc)
```

Binary: `build/libXrApiLayer_debug_gui.so`
Wrapper: `build/xr-with-debug-gui.sh`

No external dependencies beyond system OpenGL + X11 dev headers. SDL3, Dear ImGui (docking branch), and OpenXR-SDK headers are fetched via CMake FetchContent.

## How the layer works

1. The OpenXR loader calls `xrNegotiateLoaderApiLayerInterface` (the only exported symbol).
2. `RegisterAllInterceptors()` populates a `name → PFN` map from the `LIST_INTERCEPTED_FUNCTIONS` X-macro.
3. `xrCreateApiLayerInstance` advances the `XrApiLayerCreateInfo` chain and creates the instance via the next layer/runtime.
4. `PopulateNextDispatch()` queries all intercepted function pointers from the next layer.
5. `Layer_xrGetInstanceProcAddr` returns our interceptor if we have one, otherwise forwards to the next layer.
6. On `xrCreateSession`, the GUI thread is spawned (unless `XR_DEBUG_GUI_DISABLE=1`).

## Adding a new intercepted function

1. **dispatch.h**: Add `X(xrFunctionName)` to `LIST_INTERCEPTED_FUNCTIONS`.
2. **dispatch.h**: Add the forward declaration `XrResult XRAPI_CALL Layer_xrFunctionName(...)`.
3. **interceptors/**: Implement `Layer_xrFunctionName` in the appropriate file (or a new one).
4. If creating a new interceptor file, add it to `add_library()` in `CMakeLists.txt`.

The X-macro automatically generates:
- The `NextDispatch` struct member (`PFN_xrFunctionName`)
- The `PopulateNextDispatch` query
- The `RegisterAllInterceptors` map entry
- There is **no code generator** — everything is manual C++ with macros.

## Adding tracked state

1. Add a struct to `tracked_state.h`.
2. Add the collection (map/vector) to `InstanceData` in `instance_data.h`.
3. If the state is keyed by a new handle type, add a global map + Register/Unregister/Get helpers in `instance_data.h/.cpp`.
4. Populate from the interceptor under `std::unique_lock lock(data->state_mutex)`.
5. Read from GUI under `std::shared_lock lock(data->state_mutex)`.

## Adding a GUI panel

1. Create `gui/gui_newpanel.h` and `gui/gui_newpanel.cpp`.
2. Implement `void gui_render_newpanel(InstanceData *data)` — acquire `std::shared_lock` on `data->state_mutex`.
3. Call it from `gui_main.cpp`'s render loop.
4. Add the window name to the `DockBuilder` layout in `gui_main.cpp` (under `want_initial_layout`).
5. Add both files to `add_library()` in `CMakeLists.txt`.

## Threading model

- **App threads** call interceptors → take `std::unique_lock(state_mutex)` for writes.
- **GUI thread** renders at target FPS → takes `std::shared_lock(state_mutex)` for reads.
- SDL3 init + window + GL context are all created on the GUI thread (not the app's thread) to avoid GL context conflicts.

## Key design constraints

- **SDL3 is statically linked** with `-Wl,-Bsymbolic -Wl,--exclude-libs,ALL` so no SDL symbols leak. This prevents conflicts with apps using SDL2. Never switch to shared SDL3.
- **`XR_NO_PROTOTYPES`** is defined — use `PFN_xr*` function pointer types, not direct prototypes.
- The version script `XrApiLayer_debug_gui.map` ensures only `xrNegotiateLoaderApiLayerInterface` is exported. Verify with `nm -D build/libXrApiLayer_debug_gui.so | grep -w T`.
- ImGui docking branch is required (tag pattern: `v*-docking`). The `DockBuilder` API is from `imgui_internal.h`.
- Layout persistence: `~/.config/openxr_debug_gui/imgui.ini`. Delete to reset.

## Testing

Use the wrapper script against any OpenXR app:

```sh
build/xr-with-debug-gui.sh <openxr-app> [args...]
```

Or with interceptors only (no window):

```sh
build/xr-with-debug-gui.sh --no-gui <openxr-app> [args...]
```

Layer log messages go to stderr with prefix `[XR_APILAYER_DEBUG_gui]`.

## Style

Run `git clang-format` before committing. Only format project-owned code, not fetched dependencies.
