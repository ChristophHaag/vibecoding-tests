# OpenXR Debug GUI API Layer — Implementation Plan

## Overview

A standalone, runtime-agnostic OpenXR API layer (`XR_APILAYER_DEBUG_gui`) that
opens a separate debug window providing:

1. **Actions panel** — comprehensive list of action sets, actions, suggested
   bindings, active interaction profiles, and live action state updated each
   time the application syncs.
2. **Spaces panel** — interactive 3D visualization of all reference spaces and
   action poses with annotation, arcball rotation, panning, and zooming.

The layer is designed so that future features (swapchain texture inspection,
live action rebinding, etc.) can be added as new interceptor + GUI panel
modules without restructuring the existing code.

---

## Architecture

```
┌────────────────────────────────────────────────┐
│  OpenXR Application                            │
└──────────────────┬─────────────────────────────┘
                   │ xrFoo(...)
                   ▼
┌────────────────────────────────────────────────┐
│  XR_APILAYER_DEBUG_gui                         │
│                                                │
│  layer_main.cpp                                │
│    xrNegotiateLoaderApiLayerInterface          │
│    Layer_xrCreateApiLayerInstance              │
│    Layer_xrGetInstanceProcAddr ◄── dispatch.h  │
│                                    ┌───────┐   │
│  interceptors/                     │tracked│   │
│    instance.cpp  ──write──────────►│state  │   │
│    session.cpp   ──write──────────►│  .h   │   │
│    actions.cpp   ──write──────────►│       │   │
│    spaces.cpp    ──write──────────►│       │   │
│    frame.cpp     ──write──────────►│       │   │
│                                    └───┬───┘   │
│  gui/ (dedicated thread)               │ read  │
│    gui_main.cpp   SDL3+ImGui ◄─────────┘       │
│    gui_actions.cpp  Actions panel              │
│    gui_spaces.cpp   3D scene panel             │
│                                                │
└──────────────────┬─────────────────────────────┘
                   │ next_xrFoo(...)
                   ▼
┌────────────────────────────────────────────────┐
│  Next Layer / OpenXR Runtime                   │
└────────────────────────────────────────────────┘
```

**Thread model:**
- Interceptors run on application threads and **write** to tracked state under
  `std::unique_lock<std::shared_mutex>`.
- The GUI thread **reads** tracked state under
  `std::shared_lock<std::shared_mutex>`.
- The GUI has its own SDL3 window + OpenGL 3.3 context — fully independent of
  the application's graphics API and context.

---

## Directory Structure

```
debug_layer/
├── CMakeLists.txt                      Build system (shared lib, FetchContent deps)
├── XrApiLayer_debug_gui.json           Layer manifest
├── XrApiLayer_debug_gui.map            Linux linker version script
├── PLAN.md                             This file
├── DECISIONS.md                        Design decision rationale
└── src/
    ├── layer_main.cpp                  Entry: negotiate, create instance, GetInstanceProcAddr
    ├── dispatch.h                      X-macro interceptor list, NextDispatch, registration
    ├── dispatch.cpp                    Interceptor map population
    ├── instance_data.h                 Per-instance state container, global handle maps
    ├── instance_data.cpp               Handle lookup implementation
    ├── tracked_state.h                 All tracked data structures
    ├── interceptors/
    │   ├── instance.cpp                xrDestroyInstance, xrStringToPath, xrPathToString
    │   ├── session.cpp                 xrCreate/Destroy/Begin/EndSession
    │   ├── actions.cpp                 ActionSet/Action/Bindings/Sync/GetState (~10 funcs)
    │   ├── spaces.cpp                  Reference/Action spaces, xrLocateSpace
    │   └── frame.cpp                   xrWaitFrame, xrBeginFrame, xrEndFrame
    └── gui/
        ├── gui_main.h                  GUI thread public interface
        ├── gui_main.cpp                SDL3 + ImGui window, main loop, lifecycle
        ├── gui_common.h                Math helpers (vec3, mat4, arcball camera)
        ├── gui_actions.h               Actions panel interface
        ├── gui_actions.cpp             Actions/bindings/live-state panel
        ├── gui_spaces.h                Spaces panel interface
        └── gui_spaces.cpp              3D space visualization (FBO + arcball)
```

---

## Phase 1: Scaffolding & Layer Bootstrap

**Goal:** A loadable API layer that intercepts `xrCreateInstance` / `xrDestroyInstance`
and transparently forwards all other calls.

1. **CMakeLists.txt** — shared library target, FetchContent for Dear ImGui
   (docking branch v1.92+), SDL3, and OpenXR-Headers.  Set `XR_NO_PROTOTYPES`
   to avoid symbol conflicts with the OpenXR loader.  Use linker version script
   to export only `xrNegotiateLoaderApiLayerInterface`.

2. **Layer manifest** — standard `XrApiLayer_debug_gui.json` pointing at the
   `.so` / `.dll`.

3. **`layer_main.cpp`** —
   - `xrNegotiateLoaderApiLayerInterface()` validates loader structs, returns
     our `getInstanceProcAddr` and `createApiLayerInstance`.
   - `Layer_xrCreateApiLayerInstance()` advances the `XrApiLayerNextInfo`
     chain, calls the next layer's create, populates our `NextDispatch` from
     the next layer's `xrGetInstanceProcAddr`, creates `InstanceData`, and
     launches the GUI thread.
   - `Layer_xrGetInstanceProcAddr()` checks the interceptor map; returns our
     wrapper if found, else forwards to the next layer.

4. **`dispatch.h` / `.cpp`** — X-macro `LIST_INTERCEPTED_FUNCTIONS(X)` drives:
   - `NextDispatch` struct (typed `PFN_xr*` member per intercepted function)
   - `PopulateNextDispatch()` (fills NextDispatch via next-layer's
     `xrGetInstanceProcAddr`)
   - `RegisterAllInterceptors()` (populates the `name → our_fn` map)
   - `GetInterceptor(name)` (returns our function pointer or `nullptr`)

5. **`instance_data.h` / `.cpp`** — global `handle → InstanceData*` maps for
   `XrInstance`, `XrSession`, `XrActionSet`, `XrAction`, `XrSpace`.  Provides
   `GetInstanceData(handle)` helpers.

**Verification:** build, set env vars, run any OpenXR app — layer loads, app
works unchanged.

---

## Phase 2: State Tracking Interceptors

**Goal:** Intercept ~24 OpenXR functions and populate tracked state.  No GUI yet.

6. **`tracked_state.h`** — data structures:
   - `TrackedPath` — bidirectional `XrPath ↔ std::string` cache
   - `TrackedActionSet` — handle, name, localized name, priority, attached flag
   - `TrackedAction` — handle, name, localized name, XrActionType, subaction
     paths, parent action set
   - `TrackedSuggestedBinding` — interaction profile string, list of
     `{action handle, binding path string}`
   - `TrackedSpace` — handle, kind (REFERENCE/ACTION), ref type or
     action+subpath, pose offset, latest `XrSpaceLocation`
   - `TrackedSession` — handle, graphics API, state, attached action sets
   - `TrackedActionState` — per (action, subpath): isActive, current value,
     changedSinceLastSync, lastChangeTime
   - `TrackedActiveProfile` — per subpath: interaction profile path string

7. **Interceptors** (`src/interceptors/`):
   - **instance.cpp:** `xrDestroyInstance`, `xrStringToPath`, `xrPathToString`
   - **session.cpp:** `xrCreate/Destroy/Begin/EndSession`
   - **actions.cpp:** `xrCreateActionSet`, `xrDestroyActionSet`,
     `xrCreateAction`, `xrDestroyAction`,
     `xrSuggestInteractionProfileBindings`, `xrAttachSessionActionSets`,
     `xrSyncActions`, `xrGetActionState{Boolean,Float,Vector2f,Pose}`,
     `xrGetCurrentInteractionProfile`
   - **spaces.cpp:** `xrCreateReferenceSpace`, `xrCreateActionSpace`,
     `xrDestroySpace`, `xrLocateSpace`
   - **frame.cpp:** `xrWaitFrame`, `xrBeginFrame`, `xrEndFrame`

**Verification:** stderr logging per interceptor, confirm tracked data is
populated.

---

## Phase 3: GUI Window & Actions Panel

**Goal:** Open a Dear ImGui debug window showing action/actionset/binding data.

8. **`gui/gui_main.cpp`** — dedicated thread:
   - SDL3 window + OpenGL 3.3 context (independent of app)
   - ImGui with docking enabled
   - ~30 fps frame cap to minimize overhead
   - Reads `InstanceData` under `std::shared_lock`

9. **`gui/gui_actions.cpp`** — panels:
   - Action Sets table (name, priority, attached, action count)
   - Actions table (name, type, subaction paths)
   - Suggested Bindings tree (grouped by interaction profile)
   - Active Interaction Profiles display
   - Live Action State table with type-appropriate widgets:
     Boolean → checkbox, Float → progress bar, Vector2f → crosshair,
     Pose → isActive indicator

---

## Phase 4: 3D Space Visualization

**Goal:** Interactive 3D view of tracked spaces and poses.

10. **`gui/gui_spaces.cpp`** — renders to FBO, displayed via `ImGui::Image`:
    - Arcball camera (rotate / pan / zoom)
    - Floor grid (XZ plane, 1m spacing)
    - Origin gizmo (RGB XYZ arrows)
    - Reference spaces as labeled coordinate frames, color-coded by type
    - Action poses as smaller frames (faded when inactive)
    - Labels projected from 3D to screen via `ImDrawList`

---

## Phase 5: Polish & Testing

11. **Env var config:** `XR_DEBUG_GUI_DISABLE=1`, `XR_DEBUG_GUI_FPS=N`
12. **Thread safety audit** — shared_lock for reads, unique_lock for writes
13. **End-to-end test** with Monado remote driver + an OpenXR app

---

## Intercepted Functions (24)

| Domain   | Functions |
|----------|-----------|
| Instance | `xrDestroyInstance`, `xrStringToPath`, `xrPathToString` |
| Session  | `xrCreateSession`, `xrDestroySession`, `xrBeginSession`, `xrEndSession` |
| Actions  | `xrCreateActionSet`, `xrDestroyActionSet`, `xrCreateAction`, `xrDestroyAction`, `xrSuggestInteractionProfileBindings`, `xrAttachSessionActionSets`, `xrSyncActions`, `xrGetActionStateBoolean`, `xrGetActionStateFloat`, `xrGetActionStateVector2f`, `xrGetActionStatePose`, `xrGetCurrentInteractionProfile` |
| Spaces   | `xrCreateReferenceSpace`, `xrCreateActionSpace`, `xrDestroySpace`, `xrLocateSpace` |
| Frame    | `xrWaitFrame`, `xrBeginFrame`, `xrEndFrame` |

---

## Adding Non-Upstream Extension Support

To intercept a non-upstream extension function (e.g. `xrFooMNDX`):

1. Add `X(xrFooMNDX)` to `LIST_INTERCEPTED_FUNCTIONS` in `dispatch.h`
2. Add `PFN_xrFooMNDX` typedef if not already defined
3. Implement `Layer_xrFooMNDX(...)` in the appropriate interceptor file
4. Done — the X-macro propagates to NextDispatch, population, and registration

No code generator, no XML parsing, no external tooling.

---

## Future Extensibility

- **Swapchain texture inspection** → new `interceptors/swapchain.cpp` +
  `gui/gui_swapchains.cpp` + `src/graphics/` backend abstraction (GL, Vulkan,
  D3D texture copy)
- **Live action rebinding** → extend `gui/gui_actions.cpp` with input widgets,
  add state manipulation through tracked data
- **Performance overlay** → new `gui/gui_perf.cpp` using frame timing from
  `interceptors/frame.cpp`
