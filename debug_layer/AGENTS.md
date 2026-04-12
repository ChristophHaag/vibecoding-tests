# AI Agent Guide — debug_layer

## Scope

- This file is only for `debug_layer` work.
- Repo-wide workflow such as Monado startup, demo apps, workspace structure, and multi-project VS Code usage lives in `../AGENTS.md`.

## Build

Configure and build `debug_layer` from this folder as its own project root:

```sh
cmake -G Ninja -S . -B build
cmake --build build -j$(nproc)
```

- Prefer an optimized build type such as `RelWithDebInfo`; ImGui rendering is too slow at `-O0`.
- Main outputs: `build/libXrApiLayer_debug_gui.so` and `build/xr-with-debug-gui.sh`.

## Architecture

- The only exported symbol is `xrNegotiateLoaderApiLayerInterface`.
- `dispatch.h` owns the intercepted-function X-macro list and forward declarations.
- `dispatch.cpp` populates the next-layer dispatch table and the interceptor map.
- `layer_main.cpp` negotiates with the loader, advances the API-layer create chain, and exposes `xrGetInstanceProcAddr`.
- `instance_data.*` and `tracked_state.h` hold all tracked runtime state.
- The GUI thread is started from `xrCreateSession` unless `XR_DEBUG_GUI_DISABLE=1`.

## Common edits

Add an intercepted function:

1. Add it to `LIST_INTERCEPTED_FUNCTIONS` in `src/dispatch.h`.
2. Add the `Layer_xr...` declaration in `src/dispatch.h`.
3. Implement it under `src/interceptors/`.
4. If you add a new source file, add it to `add_library()` in `CMakeLists.txt`.

Add tracked state:

1. Define the struct in `src/tracked_state.h`.
2. Store it in `InstanceData` in `src/instance_data.h`.
3. Add global handle-map helpers in `src/instance_data.cpp` if a new handle type is involved.
4. Write under `std::unique_lock(data->state_mutex)` and read under `std::shared_lock(data->state_mutex)`.

Add a GUI panel:

1. Add `src/gui/gui_*.h/.cpp`.
2. Render it from `gui_main.cpp`.
3. Add it to the dock layout.
4. Set a default dock target with `ImGui::SetNextWindowDockID(..., ImGuiCond_Appearing)` so it docks even when an old `imgui.ini` exists.

## Constraints

- SDL3 must stay statically linked with symbol-hiding linker flags. Do not switch to shared SDL3.
- SDL init, window creation, and GL context ownership stay on the dedicated GUI thread.
- `XR_NO_PROTOTYPES` is intentional; use `PFN_xr*` types.
- If Vulkan bindings are involved, include `vulkan/vulkan.h` before `openxr/openxr_platform.h`.
- The version script must keep `xrNegotiateLoaderApiLayerInterface` as the only exported symbol.
- OpenGL preview capture happens on the app thread before `xrReleaseSwapchainImage`; do not move it to the GUI thread.
- Do not use `SDL_GL_GetProcAddress` on app threads for preview capture.

## Testing

Use the generated wrapper so `XR_API_LAYER_PATH` points at the build manifest:

```sh
build/xr-with-debug-gui.sh <openxr-app> [args...]
build/xr-with-debug-gui.sh --no-gui <openxr-app> [args...]
```

Typical smoke test:

```sh
P_OVERRIDE_ACTIVE_CONFIG=remote \
XRT_COMPOSITOR_FORCE_XCB=1 \
XRT_NO_STDIN=1 \
stdbuf -oL -eL ../monado/build/src/xrt/targets/service/monado-service \
    > /tmp/monado-debug-layer.log 2>&1 &
MONADO_PID=$!
until grep -q "Listening on port '4242'" /tmp/monado-debug-layer.log 2>/dev/null; do sleep 0.2; done

sleep infinity | env \
    VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
    XR_DEBUG_GUI_PREVIEW_LOG=1 \
    XR_DEBUG_GUI_PREVIEW_INTERVAL=1 \
    stdbuf -oL -eL \
    build/xr-with-debug-gui.sh --no-gui /usr/bin/hello_xr -G Vulkan2 \
    2>&1 | tee /tmp/hello_xr_vulkan_validation.log

kill $MONADO_PID
wait $MONADO_PID 2>/dev/null || true
```

- Start Monado first using the repo-wide instructions in `../AGENTS.md`.
- Prefer running this smoke test automatically after runtime-facing `debug_layer` changes, not just a compile-only check.
- Success signals include `XR_SESSION_STATE_VISIBLE->XR_SESSION_STATE_FOCUSED` and `Captured Vulkan preview`.
- Failure signals include `VUID`, `Validation Error`, or `ERROR:`.
- If this workflow breaks or needs extra setup to run cleanly, update this file or `../AGENTS.md` with the corrected commands before finishing.

## Style

- Run `git clang-format` before committing C/C++ changes.
- If `git clang-format` refuses because files are unstaged, use `clang-format -i` on the touched project-owned files.
- Do not format fetched dependencies.
