# AI Agent Guide — vibecoding-tests

## Scope

- This top-level file is only for repo-wide workflow: workspace layout, build boundaries, Monado startup, and shared test-app usage.
- Project-specific instructions belong in the project folder. In particular, debug_layer-only guidance lives in `debug_layer/AGENTS.md` and should stay there.

## Workspace model

- There is no root CMake project. Do not add one back.
- Open the repo in VS Code as multiple workspace folders or treat each subfolder as its own project root: `monado/`, `debug_layer/`, `remote-driver-client/`, and `openxr-simple-playground/`.
- Configure and build each subproject from its own directory with its own build tree.
- Prefer Ninja. Use per-project build dirs such as `build/` or `build-agent/`. Keep generated files ignored.

## Build commands

Build Monado first if another project depends on its generated headers or on the runtime. Run these from `monado/`:

```sh
cmake -G Ninja -S . -B build
cmake --build build
```

Build the remote driver client from `remote-driver-client/`:

```sh
cmake -G Ninja -S . -B build
cmake --build build
```

Build the sample playground from `openxr-simple-playground/`:

```sh
cmake -G Ninja -S . -B build
cmake --build build
```

Build `debug_layer` from `debug_layer/`; see `debug_layer/AGENTS.md` for layer-specific constraints and smoke tests.

## Shared runtime workflow

Start Monado before the remote client or any OpenXR app:

```sh
P_OVERRIDE_ACTIVE_CONFIG=remote \
XRT_COMPOSITOR_FORCE_XCB=1 \
XRT_NO_STDIN=1 \
monado/build/src/xrt/targets/service/monado-service &
```

- `P_OVERRIDE_ACTIVE_CONFIG=remote` is required. The remote driver is not auto-discovered.
- `XRT_NO_STDIN=1` matters in non-interactive shells; without it Monado can block on stdin.
- Wait for `Listening on port '4242'` before connecting the remote client or launching an app.
- Only one remote client can be connected at a time. If a previous client crashed, restart Monado.
- `ERROR [u_config_json_get_remote_settings] No remote node` can appear in a working remote setup. Treat `Config selected remote` and `Listening on port '4242'` as the readiness signals.

The active OpenXR runtime is expected to point at Monado's generated manifest:

```text
monado/build/openxr_monado-dev.json -> ~/.config/openxr/1/active_runtime.json
```

## Test apps

`hello_xr` and `openxr-playground` should both get a live stdin pipe so they do not exit immediately on EOF:

```sh
(sleep infinity) | stdbuf -oL -eL hello_xr -G Vulkan2
(sleep infinity) | openxr-simple-playground/build/openxr-playground
```

Useful readiness checks:

```sh
until grep -q "Creating swapchain for view 1" /tmp/hello_xr.log 2>/dev/null; do sleep 0.2; done
until grep -q "state changed from 4 to 5" /tmp/playground.log 2>/dev/null; do sleep 0.2; done
```

- `hello_xr` going quiet after startup is normal; it does not log per-frame activity.
- `openxr-playground` needs `DISPLAY` and reaches usable input state at session state `5` (`FOCUSED`).
- When redirecting app output to a log file, prefer `stdbuf -oL -eL` so readiness checks observe lines promptly instead of waiting on stdio buffering.

## End-to-end testing

- After changing any runnable subproject or its build/runtime wiring, prefer an end-to-end smoke test on live Monado before finishing.
- Build only the subproject you changed, then run one canonical test path instead of inventing a new ad hoc sequence.
- Keep logs in `/tmp` and clean up the app and Monado processes before you stop.

Canonical remote-driver-client smoke test:

```sh
# From vibecoding-tests/
P_OVERRIDE_ACTIVE_CONFIG=remote \
XRT_COMPOSITOR_FORCE_XCB=1 \
XRT_NO_STDIN=1 \
stdbuf -oL -eL monado/build/src/xrt/targets/service/monado-service \
	> /tmp/monado-e2e.log 2>&1 &
MONADO_PID=$!
until grep -q "Listening on port '4242'" /tmp/monado-e2e.log 2>/dev/null; do sleep 0.2; done

(sleep infinity) | stdbuf -oL -eL hello_xr -G Vulkan2 \
	> /tmp/hello_xr-e2e.log 2>&1 &
APP_PID=$!
until grep -q "Creating swapchain for view 1" /tmp/hello_xr-e2e.log 2>/dev/null; do sleep 0.2; done

printf 'state\nhead 0 1.7 0 0 0 0 1\nsend\nquit\n' | \
	remote-driver-client/build/monado-remote-client

kill $APP_PID $MONADO_PID
wait $APP_PID $MONADO_PID 2>/dev/null || true
```

Success signals from the tested path:

- Monado log: `Config selected remote` and `Listening on port '4242'`.
- App log: `Creating swapchain for view 1`.
- Remote client: `Connected.`, `Ready.`, JSON state output, then `ok` for `state`, `head`, and `send`.

## Are frames actually reaching the compositor? — diagnostic recipe

The canonical smoke test only checks that the app *connects* (`Creating swapchain
for view 1`, `state changed from 4 to 5`) and that Monado *starts up*
(`Config selected remote`, `Listening on port '4242'`). It does **not** verify
that the compositor actually renders a frame. An app can reach `FOCUSED` and
still never composite a single frame, because the test apps render to their own
debug windows regardless of the compositor. When you are asked to "verify it
works" or to debug a rendering path (e.g. openwarp), use this recipe before
trusting any conclusion.

### Frame flow, end to end

A composited frame travels this path. Any break here means the compositor
renders nothing, even though the app looks fine.

```
client:    xrEndFrame
           → oxr_session_frame_end (state_trackers/oxr/oxr_session_frame_end.c:1990)
           → xrt_comp_layer_commit(xc, sync_handle)
                          # GL clients: also layer_commit_with_semaphore
ipc:       ipc_compositor_layer_commit (ipc/client/ipc_client_compositor.c:740)
           → IPC message
           → ipc_server_handler: xrt_comp_layer_commit (ipc/server/ipc_server_handler.c:1417)
server:    compositor_layer_commit (compositor/main/comp_compositor.c:306)
           → comp_renderer_draw     (compositor/main/comp_compositor.c:330)
           → comp_renderer_draw     (compositor/main/comp_renderer.c:1242)
              ├── comp_target_check_ready() must be true, else early return
              ├── dispatch_compute / dispatch_graphics (comp_renderer.c:923/1150)
              │     ├── openwarp: renderer_dispatch_compute_depth_warp → render_compute_*
              │     └── default: chl_frame_state_cs_default_pipeline / chl_frame_state_gfx_*
              └── renderer_submit_queue → present
```

### Step-by-step verification

Run Monado with `XRT_COMPOSITOR_LOG=trace` and `XRT_LOG=debug` on the service,
and the same `XRT_LOG=debug` on the client (app) process. **Note:** the env
var for the global log level is `XRT_LOG`, **not** `XRT_LOG_LEVEL` — the
correct knob is `DEBUG_GET_ONCE_LOG_OPTION(global_log, "XRT_LOG",
U_LOGGING_WARN)` in `u_logging.c`, and its default is `WARN` (so `U_LOG_I`
markers are silently filtered if you only set `XRT_LOG_LEVEL` or nothing).
`XRT_COMPOSITOR_LOG=trace` enables the compositor's `COMP_SPEW`/`COMP_DEBUG`
trace logs (e.g. `LAYER_COMMIT`); that is **independent** of `XRT_LOG`.

Then check, in order:

1. **Client connected to *this* server**: in the Monado log,
   `INFO [client_connected] Client N connected`. If absent, the app is talking
   to a stale Monado from a previous run — kill leftovers and re-check the
   `active_runtime.json` symlink (`monado/build/openxr_monado-dev.json →
   ~/.config/openxr/1/active_runtime.json`). Rebuild `openxr_monado` after any
   commit (`cmake --build build --target openxr_monado`) so the client library
   and service share the same `u_git_tag` — mismatches break the IPC even with
   `IPC_IGNORE_VERSION=1`.

2. **GL/VK interop sync at xrEndFrame**: in the *app* log, grep for
   `ipc_call_compositor_layer_sync_with_semaphore failed: XRT_ERROR_IPC_FAILURE`
   or `ipc_call_space_locate_device failed: XRT_ERROR_IPC_FAILURE`. If present,
   the GL↔Vulkan swapchain sync (semaphore) is broken in this environment and
   the layer commit never reaches the server. The app will still report
   `FOCUSED` and its own window will keep animating, but Monado's compositor
   renders nothing. This is a known sandbox limitation with no real fix from
   the compositor side; verify on hardware with working GL/VK interop.

3. **Server receives the commit**: in the Monado log, `LAYER_COMMIT` at SPEW
   level (set `XRT_COMPOSITOR_LOG=trace`) or a temporary `U_LOG_I` at the top of
   `compositor_layer_commit` (comp_compositor.c:306). If absent after the app
   reached `FOCUSED` and you waited 10+ seconds, the IPC path is broken —
   almost always the GL/VK sync from step 2.

4. **`comp_renderer_draw` is reached**: temporary `U_LOG_I` at the top of
   `comp_renderer_draw` (comp_renderer.c:1242). Also log
   `comp_target_check_ready()` here. If `check_ready` is `false`, the function
   returns early after emulating timing — **no `dispatch_compute` is called and
   no frame is rendered**, but the frame loop still ticks.

5. **Compute or graphics path**: temporary `U_LOG_I` in `dispatch_compute`
   (comp_renderer.c:1150) and `dispatch_graphics` (comp_renderer.c:923). For
   the openwarp depth fast path, also a marker in
   `renderer_dispatch_compute_depth_warp` (comp_renderer.c:~1015). For the
   default compute path, `chl_frame_state_cs_default_pipeline` is called from
   inside `dispatch_compute`.

6. **Validation**: run with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`
   and grep the Monado log for `Validation Error` and `Validation Warning`.
   Resource init (pipeline/layout/descriptor pool/UBO creation) runs even
   without a frame being dispatched, so a clean init is necessary but not
   sufficient — you also need a marker inside the per-frame function to prove
   the path actually executed.

### Common misreadings

- **`state changed from 4 to 5` (FOCUSED) does not mean frames render.** It
  only means the OpenXR session reached the focused state. Compositing is a
  separate, later step that the app triggers by calling `xrEndFrame` and the
  runtime delivering the commit to the compositor.
- **`WARN [renderer_wait_for_present] Compositor probably missed frame by
  …ms` is from teardown, not a per-frame loop.** The *only* call site of
  `renderer_wait_for_present` is `comp_renderer.c:1435`, inside the renderer
  destroy path (after `render_compute_fini` / `render_gfx_fini`), guarded by
  `if (present_success)`. A single such warning at shutdown is normal and is
  not evidence of a running frame loop.
- **A quiet app log is not a hung compositor.** `hello_xr` does not log
  per-frame activity, and `openxr-playground` only logs at session transitions.
  Use the server-side markers, not app silence, to decide whether frames are
  being composited.
- **The playground's own window is not the compositor.** The playground renders
  its scene to an SDL window it owns. If the only thing you can see animating
  is that window, the compositor may still be idle.

### Quick instrumented run

To get a definitive answer in one go, add these temporary `U_LOG_I` calls
(remove before committing), rebuild Monado, run with trace logging, and start
the playground:

```c
// comp_renderer.c:1242 (comp_renderer_draw)
U_LOG_I("DRAW ready=%d use_compute=%d", comp_target_check_ready(r->c->target), r->settings->use_compute);
// comp_renderer.c:1150 (dispatch_compute)
U_LOG_I("DISPATCH_COMPUTE fast=%d layers=%u", frame_state->data.fast_path, layer_count);
// comp_renderer.c:~1015 (renderer_dispatch_compute_depth_warp)
U_LOG_I("OPENWARP_DISPATCH");
// comp_compositor.c:306 (compositor_layer_commit)
U_LOG_I("LAYER_COMMIT");
```

After a 20-second playground run with `PLAYGROUND_NO_QUAD=1`, `grep -c` each
marker in the Monado log. `LAYER_COMMIT > 0` is the first gate; `DRAW > 0` and
`DISPATCH_COMPUTE > 0` confirm the renderer actually ran; `OPENWARP_DISPATCH >
0` confirms the openwarp fast path was taken. Any `0` upstream of a `> 0`
downstream tells you exactly where the chain breaks.

**Known-good pattern** (this repo, `monado` + `openxr-playground` with
`PLAYGROUND_NO_QUAD=1`, `XRT_COMPOSITOR_COMPUTE=1`, `XRT_COMPOSITOR_LOG=trace`,
`XRT_LOG=debug`, default simulated HMD):

| Gate | Marker / log | Expected |
|---|---|---|
| Client connected | `[client_connected] Client N` | > 0 |
| Per-frame draw | `DRAW use_compute=1` | > 0, `use_compute=1` |
| Compute dispatch | `DISPATCH_COMPUTE fast=1 layers=1 type0=1` | > 0, `type0=1` = `XRT_LAYER_PROJECTION_DEPTH` |
| Openwarp fast path | `OPENWARP_DISPATCH` | > 0 |
| Compositor trace | `LAYER_COMMIT finished drawing` | > 0 (matches DRAW) |
| Validation | `Validation Error` / `Validation Warning` | 0 (per-frame), but see note |

A 15-second run produced ~1034 draws and ~912 openwarp dispatches (the
remainder are the `else` default compute branch in the few frames before the
playground reached `FOCUSED`). Note: this same run produced 1 validation
**error** at *shutdown* — `vkDestroyDevice: 60 leaked objects` — from the
compute target's `rtr` array / swapchain images not being fully freed in this
sandbox's teardown path. It is unrelated to the per-frame openwarp path and
does not block the per-frame verification gates above.

## Keep instructions current

- If you hit setup trouble with Monado, the remote client, the debug layer wrapper, or a test app, fix the workflow and update the relevant `AGENTS.md` in the same change.
- Keep one canonical command sequence per project. Replace stale steps instead of stacking alternatives.
- When you learn a new failure mode that is easy to misread, add the concrete symptom and the correct success signal.

## Remote driver client

Binary:

```text
remote-driver-client/build/monado-remote-client
```

Basic use:

```sh
printf 'state\nquit\n' | remote-driver-client/build/monado-remote-client
```

- The client reads commands from stdin and writes `ok` or `error: ...` to stdout.
- Pose/button/axis edits are only applied to Monado after `send`.
- The full command list is documented in `remote-driver-client/main.c`.

## Repo-wide conventions

- Build and test only the subproject you are changing.
- Rebuild `remote-driver-client` after Monado protocol/header changes because it includes Monado headers directly.
- Prefer a live Monado smoke test before finishing when the changed subproject can actually be exercised that way.
- When the active session allows commits and the work has reached a coherent, tested checkpoint, make a commit instead of leaving finished work uncommitted.
- Commit messages should describe both what changed and why the change was made, not just a short title fragment.
- Format only project-owned code. Do not make formatting-only edits in vendored or external code unless explicitly asked.
