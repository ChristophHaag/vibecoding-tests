# Design Decisions — OpenXR Debug GUI API Layer

This document records the key architectural and technology choices made for the
`XR_APILAYER_DEBUG_gui` layer, with rationale for each.

---

## 1. UI Framework: Dear ImGui

### Candidates Considered

| Framework | Language | GPU backends | Windowing | License |
|-----------|----------|-------------|-----------|---------|
| **Dear ImGui** | C++ | OpenGL, Vulkan, DX9-12, Metal, WebGPU | SDL2/3, GLFW, Win32 | MIT |
| Qt | C++ | Own renderer | Own | LGPL/Commercial |
| Nuklear | C | OpenGL, SDL, GDI | SDL, GLFW, X11 | MIT/Public Domain |
| egui | Rust | wgpu, glow | winit | MIT/Apache-2.0 |
| nanogui | C++ | OpenGL, Metal | GLFW | BSD |

### Decision: **Dear ImGui (docking branch, v1.92+)**

**Reasons:**

1. **Graphics API breadth.** Dear ImGui has official, maintained backends for
   OpenGL 2/3, Vulkan, DirectX 9/10/11/12, Metal, and WebGPU.  This is
   critical because feature 2 (swapchain texture inspection) will need to
   display textures from the application's graphics API.  No other candidate
   covers this range with first-party support.

2. **Immediate-mode paradigm.** The debug GUI needs to display rapidly changing
   state (action values, space poses) every frame.  Immediate-mode rendering
   is a natural fit — no widget state management, no signal/slot boilerplate,
   just read tracked data and render it each frame.

3. **Docking support.** The docking branch allows users to rearrange panels
   (actions, spaces, future panels) by dragging tabs and splitting windows.
   This is valuable for a debug tool where different users care about
   different information.

4. **Proven in VR/XR tooling.** Monado (the open-source OpenXR runtime) uses
   Dear ImGui for its debug GUI (`monado-gui`).  SteamVR, Godot XR tools,
   and many VR debugging utilities use ImGui.  The ecosystem is battle-tested
   for this exact use case.

5. **Lightweight and embeddable.** ImGui is ~20 source files with no external
   dependencies beyond a graphics backend.  It can be vendored or fetched via
   CMake FetchContent without pulling in a large framework.

6. **Extensive widget library.** Tables with sorting/resizing, tree views,
   plots (via ImPlot), color pickers, sliders — all needed for the planned
   feature set.  ImPlot adds time-series graphing for potential future
   performance overlays.

7. **Future Vulkan instance reuse.** ImGui's Vulkan backend can accept an
   externally-created VkInstance/VkDevice.  If we later want to share the
   application's Vulkan instance (from `XR_KHR_vulkan_enable2`) for texture
   inspection, ImGui supports this directly.

**Rejected alternatives:**

- **Qt:** Far too heavy for an API layer.  Adds ~50 MB of shared libraries,
  complex deployment, LGPL licensing concerns for proprietary apps.  The
  signal/slot model adds unnecessary complexity for a debug overlay that just
  needs to show live data.

- **Nuklear:** Single-header C library, which is appealing for lightweight
  deployment.  However, it lacks official Vulkan/DX backends, has no docking
  support, and its widget set is more limited (no sortable tables, no built-in
  plot widgets).  The C-only constraint would complicate ImGui-style layouts.

- **egui:** Rust-based, which would require a Rust toolchain and FFI bridge
  for a C/C++ OpenXR layer.  The extra build complexity and language boundary
  are not justified when ImGui provides equivalent functionality natively in
  C++.

- **nanogui:** Limited to OpenGL and Metal.  No Vulkan or DirectX support
  means it cannot display Vulkan/DX textures for feature 2.  Smaller
  community and fewer widgets than ImGui.

---

## 2. Windowing Library: SDL3

### Candidates Considered

| Library | Platforms | GL | Vulkan | Status |
|---------|-----------|----|----|--------|
| **SDL3** | Windows, Linux, macOS, Android, iOS | Yes | Yes | Stable (2025+) |
| SDL2 | Same | Yes | Yes | Maintenance mode |
| GLFW | Windows, Linux, macOS | Yes | Yes | Active |
| Win32 + Xlib/Wayland | Platform-specific | Yes | Yes | Manual |

### Decision: **SDL3 (with SDL2 fallback)**

**Reasons:**

1. **ImGui's recommended backend** for new projects (per the official ImGui
   backends documentation).

2. **Better Wayland support** than SDL2.  Many Linux desktop environments are
   migrating to Wayland; SDL3 handles this more gracefully (no
   `SDL_VIDEODRIVER` workarounds).

3. **Cross-platform parity.** SDL3 runs on Windows, Linux, macOS, Android,
   and iOS.  GLFW lacks mobile support.

4. **Event handling.** SDL3's event system handles window management, input,
   and display queries — everything the debug GUI thread needs.

5. **CMake FetchContent support.** SDL3 can be fetched and built as part of
   our CMake project, making the layer self-contained.

**Fallback:** The CMakeLists.txt attempts `find_package(SDL3)` first, then
tries FetchContent, then falls back to SDL2.  ImGui supports both SDL2 and
SDL3 backends, so switching is a compile-time flag.

**Rejected:**

- **GLFW:** Good library, but no mobile support and slightly weaker Wayland
  support.  Since SDL handles more platforms and is equally well-supported
  by ImGui, SDL is preferred.

- **Native windowing (Win32/Xlib/Wayland):** Maximum control but enormous
  platform-specific code.  Not worth it when SDL abstracts this cleanly.

---

## 3. GUI Rendering API: OpenGL 3.3

### Decision: **OpenGL 3.3 Core Profile**

The debug GUI window uses its own OpenGL 3.3 context for rendering ImGui and
the 3D space visualization.

**Reasons:**

1. **Universal availability.** OpenGL 3.3 is supported on virtually all
   desktop GPUs from the last 15 years.  It works on Windows, Linux, and macOS
   (though macOS caps at 4.1).

2. **Independent of the application.** The layer creates its own GL context on
   a separate thread.  There is zero interaction with the application's
   graphics API — whether the app uses OpenGL, Vulkan, or D3D, the debug
   GUI's GL context is unaffected.

3. **Low overhead.** The debug GUI renders simple ImGui draw lists and a few
   hundred lines for the 3D scene.  OpenGL 3.3 handles this trivially with
   minimal driver overhead.

4. **ImGui native support.** `imgui_impl_opengl3` is one of the most mature
   and well-tested ImGui backends.

**Why not Vulkan for the GUI?**  Vulkan would be overkill for rendering a
debug GUI.  The setup cost (instance, device, swapchain, render pass,
pipeline, synchronization) is significant, and the debug GUI's rendering needs
are extremely simple.  OpenGL's simplicity is an advantage here.

**Future note:** When feature 2 (swapchain texture inspection) is added, the
layer may need to import textures from the application's graphics API.  For
Vulkan apps, this could use `VK_KHR_external_memory` or GL/VK interop
extensions.  For now, the GL context is purely for the GUI's own rendering.

---

## 4. Dispatch Approach: Manual Macro-Based Table

### Candidates Considered

| Approach | Tooling | Extensibility | Maintenance |
|----------|---------|--------------|-------------|
| **Khronos-style generated from xr.xml** | Python + xr.xml registry | All core functions auto-covered | Must re-run generator on spec updates |
| **Manual macro-based X-list** | None | Add one line per new function | Manual but trivial |
| **Per-function ad-hoc** | None | Copy-paste per function | Error-prone, no single source of truth |

### Decision: **Manual macro-based X-list**

An X-macro `LIST_INTERCEPTED_FUNCTIONS(X)` in `dispatch.h` is the single
source of truth.  It drives:
- The `NextDispatch` struct (one typed `PFN_xr*` member per function)
- `PopulateNextDispatch()` (fills `NextDispatch` from next layer)
- `RegisterAllInterceptors()` (builds the `name → function_pointer` map)
- `GetInterceptor()` (returns our wrapper or `nullptr`)

**Reasons for choosing manual over generated:**

1. **No external tooling dependency.** The Khronos code generators require
   Python 3, the `xr.xml` registry file, and several Python scripts from the
   OpenXR-SDK-Source tree (`automatic_source_generator.py`,
   `api_dump_generator.py`, etc.).  This is a heavy dependency for a
   standalone layer that intercepts only ~24 functions.

2. **Non-upstream extension support.** The Khronos generators only know about
   functions defined in `xr.xml`.  For non-upstream extensions (e.g.
   `XR_MNDX_*`, vendor extensions not yet in the registry), the generated
   code would need patching or a custom registry.  With the manual approach,
   adding a non-upstream function is: add one line to the X-macro, provide the
   `PFN_*` typedef (from the extension header), implement the interceptor.

3. **Simplicity and auditability.** The entire dispatch mechanism is ~100
   lines of C++ in `dispatch.h` + `dispatch.cpp`.  There is no generated code
   to debug, no build-time code generation step, and the X-macro ensures
   consistency across all uses.

4. **We intercept few functions.** The layer intercepts ~24 of the ~200+
   OpenXR functions.  Generating dispatch for all 200+ and intercepting only
   24 wastes build time and adds dead code.

**Why not per-function ad-hoc?**  Without a central function list, it's easy
to add an interceptor wrapper but forget to populate the next-dispatch pointer
(or vice versa).  The X-macro ensures that all three operations (struct member,
population, registration) are always in sync.

**Trade-off acknowledged:** When the OpenXR spec adds new functions we want to
intercept, we manually add them.  This is a deliberate trade-off:  the layer
intercepts a curated set of functions, not everything, so manual maintenance
is proportional to features added, not to spec growth.

---

## 5. Language: C++17

### Decision: **C++17**

**Reasons:**

1. Dear ImGui is C++ and using its API directly is simpler than going through
   C bindings (cimgui).
2. `std::shared_mutex` (C++17) provides efficient reader/writer locking for
   the GUI-thread / interceptor-thread model.
3. `std::string_view`, structured bindings, `std::optional`, and `if constexpr`
   reduce boilerplate.
4. The OpenXR headers are C-compatible; C++ can include them directly.
5. Nearly all OpenXR API layers in the ecosystem (api_dump, core_validation,
   best_practices) are C++.

---

## 6. GUI Threading: Dedicated Thread with Own Context

### Decision: **GUI runs on a dedicated `std::thread` with its own SDL window and OpenGL context**

**Reasons:**

1. **Decoupled from application rendering.** The layer must not interfere with
   the application's frame timing, graphics state, or threading model.  A
   separate thread with a separate GL context achieves complete isolation.

2. **Works with any app graphics API.** Whether the app uses OpenGL, Vulkan,
   D3D11, or D3D12, the GUI thread's GL context is independent.

3. **Frame rate independence.** The debug GUI runs at ~30 fps (configurable)
   regardless of the application's frame rate.  This minimizes CPU/GPU
   overhead from the debug tool.

4. **Clean lifecycle.** The thread starts in `xrCreateApiLayerInstance` and
   stops in `xrDestroyInstance`.  No hooks into the application's render loop.

**Synchronization:** `std::shared_mutex` per `InstanceData`.  The GUI thread
acquires a shared (read) lock when rendering; interceptors acquire a unique
(write) lock when updating tracked state.  Contention is minimal because:
- Write locks are brief (just updating a struct field)
- The GUI reads at 30 fps
- Most interceptors are called at 72-120 fps (frame rate), so writes are
  slightly more frequent but very short-lived

---

## 7. 3D Rendering: Minimal OpenGL Lines

### Decision: **Basic GL 3.3 line rendering, no mesh/PBR**

The 3D space visualization renders:
- Grid lines (XZ plane)
- Coordinate frame arrows (colored lines for X/Y/Z)
- Label text (projected 3D → 2D, rendered via ImGui's `ImDrawList`)

**Reasons:**

1. **Minimal overhead.** A few hundred line segments is trivial for any GPU.
   No textures, no meshes, no complex shaders.

2. **Clarity.** For debugging spatial relationships, clean coordinate frames
   and labels are more informative than 3D models.

3. **No additional dependencies.** No 3D model loading, no font rendering
   library — just GL line primitives and ImGui text.

**Future:** If controller models or environment meshes are desired, a more
capable renderer could be added to `gui/gui_spaces.cpp` without affecting
other components.

---

## 8. Math Library: Inline Header

### Decision: **Custom `gui_common.h` (~250 lines)**

Rather than depending on GLM, Eigen, or HandmadeMath, the 3D math needs
(mat4 multiply, perspective projection, lookAt, arcball rotation, basic vec3
operations) are implemented inline in a single header.

**Reasons:**

1. The total math code needed is ~250 lines — not enough to justify an
   external dependency.
2. Keeps the build self-contained (FetchContent already pulls ImGui and SDL).
3. The math is straightforward (4×4 matrices, quaternions) and unlikely to
   need the breadth of a full math library.

---

## Summary

| Decision | Choice | Key Rationale |
|----------|--------|---------------|
| UI framework | Dear ImGui (docking, v1.92+) | Best multi-API backend coverage, immediate-mode, proven in XR |
| Windowing | SDL3 (SDL2 fallback) | Cross-platform, ImGui recommended, good Wayland support |
| GUI rendering | OpenGL 3.3 | Universal, independent of app, minimal overhead |
| Dispatch | Manual X-macro | No tooling dependency, trivial non-upstream extension support |
| Language | C++17 | ImGui is C++, shared_mutex, modern stdlib |
| GUI threading | Dedicated thread, own GL context | Complete isolation from app |
| 3D rendering | GL line primitives | Minimal overhead, clear visualization |
| Math library | Inline header | ~250 lines, no external dependency needed |
