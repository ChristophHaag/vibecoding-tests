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
    │   └── frame.cpp              xrWaitFrame, xrBeginFrame, xrEndFrame, swapchain ops (CLOCK_MONOTONIC timing)
    └── gui/
        ├── gui_main.h/.cpp        SDL3 init + ImGui setup on dedicated thread, DockBuilder layout
        ├── gui_actions.h/.cpp      Actions/bindings/live-state panels
        ├── gui_spaces.h/.cpp       3D FBO scene: grid, axes, space frames, view frustums, labels
        ├── gui_perf.h/.cpp         Performance observatory: stacked bars (budget/percentage/max-frame scale),
        │                           zoomable real-time timeline, 8 timing graphs with frame-number tooltips
        └── gui_common.h           Math: Vec3, Vec4, Mat4, ArcballCamera, project_to_screen
```

## Building

```sh
cmake -G Ninja -B build-ninja
cmake --build build-ninja -j$(nproc)
```

The build defaults to **RelWithDebInfo**. ImGui's draw routines (AddPolyline, etc.) are
unusably slow at `-O0` — always build with optimizations enabled.

Binary: `build-ninja/libXrApiLayer_debug_gui.so`
Wrapper: `build-ninja/xr-with-debug-gui.sh`

Prefer Ninja for agent-created builds.  Avoid generating fresh build output in
tracked `build/` directories when an untracked `build-ninja/` tree will do.

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

Tracked composition-layer previews currently follow two important rules:
- Preview images are cached per `(swapchain, image index, array index)` instead of one global "latest" preview for a swapchain. Keep that model if you expand previewing to more APIs.
- The GUI should tolerate temporarily missing `xrEndFrame` layers by retaining recent layer submissions for a few frames and marking them stale instead of dropping them immediately.

## Adding a GUI panel

1. Create `gui/gui_newpanel.h` and `gui/gui_newpanel.cpp`.
2. Implement `void gui_render_newpanel(InstanceData *data)` — acquire `std::shared_lock` on `data->state_mutex`.
3. Call it from `gui_main.cpp`'s render loop.
4. Add the window name to the `DockBuilder` layout in `gui_main.cpp` (under `want_initial_layout`).
5. Add both files to `add_library()` in `CMakeLists.txt`.

If the user already has an existing `imgui.ini`, the first-run dock builder will
not run.  New panels should also set a default dock target with
`ImGui::SetNextWindowDockID(..., ImGuiCond_Appearing)` so they land inside the
main dockspace instead of floating.

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
- OpenGL preview capture happens on the application thread before `xrReleaseSwapchainImage`, not on the GUI thread. This is deliberate to avoid cross-context ownership issues.
- Do not use `SDL_GL_GetProcAddress` on application threads for swapchain preview capture. Use direct GL/EGL/GLX proc lookup because SDL may not be initialized there.
- `XR_TYPE_GRAPHICS_BINDING_EGL_MNDX` should be treated as desktop OpenGL for preview eligibility and UI labeling.
- Current preview support is intentionally limited to OpenGL color swapchains; non-color swapchains, cube faces, and non-OpenGL sessions remain metadata-only.

## Composition Layers Panel

- The Composition Layers panel renders a retained live set built from recent `xrEndFrame` submissions instead of only `composition_frames.back()`.
- Keep the non-inspect preview layout height stable even when a preview is missing, otherwise the details below will visibly jump during runtime.
- Preview debug logging is controlled by `XR_DEBUG_GUI_GL_PREVIEW_LOG`:
    - `0`: silent
    - `1`: first success and distinct failure/skip reasons per swapchain
    - `2`: every attempt
- When extending previews to array swapchains, keep array index in both the preview cache key and the GUI texture cache key.

## 3D Composition Rendering

- The existing 3D Spaces panel already owns a dedicated GL FBO, camera, and line renderer; composition-layer world rendering should build on that instead of opening a second scene panel.
- Projection views should be visualized using the tracked `view.pose` + `view.fov` geometry, with textured surfaces placed on the far plane of the displayed frustum.
- Quad layers should be visualized as textured rectangles transformed by the layer pose and size in the layer's tracked space.
- Expect multiple composition layers to reuse the same swapchain image across frames; do not assume every frame produces a new preview.

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

If `git clang-format` refuses to run because the file is unstaged, use
`clang-format -i` on the touched project-owned files instead and rebuild.
