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

## Test scripts (`scripts/`)

Use the scripts for reproducible setup/teardown instead of ad-hoc shell
pipelines. They detach everything into its own session (`setsid`), wait on
readiness with bounded timeouts, and fail loudly:

```sh
bash scripts/service-down.sh                        # stop everything (safe when idle)
bash scripts/service-up.sh [/tmp/monado.log]        # start service, wait for port 4242
bash scripts/hello-up.sh [/tmp/hello.log]           # start hello_xr, wait for swapchains
bash scripts/playground-up.sh [/tmp/playground.log] # start playground, wait for FOCUSED
bash scripts/capture-heavy-warp.sh 0 0.03 6 0.4 /tmp/ab 25  # head-motion burst + settled fresh ref
python3 scripts/analyze-warp.py warped.png fresh.png --crop x,y,w,h  # FFT-aligned halo/missing metrics as JSON
```

- Always `service-down` first: a previous service can linger in teardown
  while still holding port 4242, and the next instance then dies with
  `ERROR [setup_accept_fd] bind: -1`. The script SIGTERMs, escalates to
  SIGKILL, removes `/run/user/1000/monado.pid`, and reports leftovers.
- `pkill -x openxr-playground` does NOT work (comm truncates to 15 chars);
  the script matches the build-tree path instead. Same for any `pgrep -c`
  singularity check — match the path, not the short name.
- Known Monado issue (do not try to fix it in passing): freezing an app
  at the wrong moment of its frame cycle makes frame pacing schedule the
  next `xrWaitFrame` after resume for a very long time, so the app looks
  wedged and recovers only slowly. After any freeze, allow a long settle
  (tens of seconds) and check the app log for session-state progress
  before concluding the app, the warp, or the service is stuck.
- Any `pkill`/`pgrep -f <pattern>` MUST use the bracket trick
  (e.g. `[o]penxr-playground`, `pg-stdin-holde[r]`): the pattern otherwise
  matches the calling shell's own command line and the script SIGTERMs or
  SIGSTOPs itself (this has happened more than once — a frozen shell that
  times out instead of a clean kill).
- Use a fresh log file per run; concurrent runs appending to one log are
  unreadable. The scripts truncate the log they are given.
- Do not launch apps with direct `... &` background pipelines: the harness
  waits on the pipe and the call times out (the app may survive, or be
  killed with the group — either way it is unobservable). The scripts
  return immediately after detaching.

## Test apps

`hello_xr` needs a live stdin pipe (it exits on stdin EOF);
`openxr-playground` stalls early in setup on EOF stdin (it blocks, it does
not exit). The scripts provide a blocking stdin holder for both — do not
redirect app stdin from `/dev/null`.

```sh
(sleep infinity) | stdbuf -oL -eL hello_xr -G Vulkan2
(sleep infinity) | openxr-simple-playground/build/openxr-playground
```

Useful readiness checks:

```sh
until grep -q "swapchain for view 1" /tmp/hello_xr.log 2>/dev/null; do sleep 0.2; done
until grep -q "state changed from 4 to 5" /tmp/playground.log 2>/dev/null; do sleep 0.2; done
```

- `hello_xr` going quiet after startup is normal; it does not log per-frame activity.
- `openxr-playground` needs `DISPLAY` and reaches usable input state at session state `5` (`FOCUSED`).
- When redirecting app output to a log file, `stdbuf -oL -eL` is REQUIRED,
  not optional: without it the playground's output sits in a 4–8 KB stdio
  buffer and the log looks frozen at `Using preferred swapchain format`
  while the app is actually running fine. (This exact misread cost a full
  debugging session.)
- Playground env knobs (test hooks in `main.cpp`, see
  `openxr-simple-playground/AGENTS.md`): `PLAYGROUND_NO_QUAD=1` submits only
  the projection(+depth) layer so the compositor takes the single-layer
  fast path (required for the openwarp depth warp);
  `PLAYGROUND_NO_QUIT=1` ignores SDL_QUIT so runs survive;
  `PLAYGROUND_FRAME_MS` paces frames (default 50; the stock `sleep(1.5)`
  makes ~0.66 fps).

## End-to-end testing

- After changing any runnable subproject or its build/runtime wiring, prefer an end-to-end smoke test on live Monado before finishing.
- Build only the subproject you changed, then run one canonical test path instead of inventing a new ad hoc sequence.
- Keep logs in `/tmp` and clean up the app and Monado processes before you stop.

### Seeing the compositor output

The compositor opens a `Monado` XCB window (e.g. 960x540 side-by-side
eyes) showing the distorted output. Screenshot it any time:

```sh
import -window "Monado" /tmp/shot.png
```

> Capture is broken sandbox-wide since the 2026-09-06 ~02:05 UTC system
> update (`import` dies with `missing an image filename`, `xwd`/`scrot`
> BadMatch, `ffmpeg x11grab` reports `screen size 0x0`). Do not burn
> sessions retrying capture commands; see
> `docs/openwarp-stale-capture.md` for status.

This closes the loop for rendering-path work: capture a stale frame
(warp pose new, source old) plus a settled fresh frame at the same pose
and compare with `analyze-warp.py` — no eyeballing screenshots. Only
same-pose pairs count: comparing a mid-motion frame against fresh at a
*different* pose measures parallax, not warp error. Full methodology
(stale-window identification, masks, attribution): see
`docs/openwarp-stale-capture.md`; pipeline reference:
`monado/doc/openwarp_integration.md` §8; edge-sharpness work log:
`docs/openwarp-edge-sharpness.md`.

Do NOT use `kill -STOP` on the app to pin the render pose: the compositor
only presents on client commits, so a frozen client means a frozen mirror
(verified: 30 s dwell through 0.3 m + 20° motion, zero pixels changed) —
and freezing risks Monado's known frame-pacing stall (below), which then
masquerades as a warp bug. If screenshots stop updating, restart fresh
(`service-down`/`service-up`/`hello-up`/`playground-up`) instead of
freeze/thaw cycles.

- A mirror that stays bit-identical across head moves (same md5 at
  different poses) with the app `FOCUSED` is a wedged session, not a
  perfect warp. `kill -9` on the playground without restarting the
  service wedges the replacement app instance in `xrWaitFrame` (0 CPU
  time, no new commits). Always `service-down`/`service-up` together
  with an app restart after a `kill -9`, then re-verify with one head
  move + screenshot md5 change before measuring anything.

### Stale capture scripts (recipes)

- `scripts/capture-stale-hunt.py` — stop-on-hit hunter for blackout-type
  events over a persistent remote-client connection (yaw recipe below).
  Prints `HIT <png>` + `FRESH <png>` (exit 0) or `MISS` + best near-miss
  (exit 1). Hit signature: green pixels collapse (<50% of baseline)
  while pink stays in view (>25% of baseline).
- `scripts/capture-stale-burst.py` — full-burst variant for offline edge
  analysis; supports absolute poses **including translation** and saves
  every dense frame plus the settled fresh frame:
  `python3 scripts/capture-stale-burst.py /tmp/frayA 0 0 0 0 1 0.15 0 0 0 1`
- `scripts/analyze-warp.py` — FFT-aligned halo/missing/smear metrics for
  a (warped, fresh) pair (`--crop x,y,w,h` restricts to static geometry).

```sh
printf 'head 0 1.7 0 -0.0872 0 0 0.9962\nsend\nquit\n' | \
	remote-driver-client/build/monado-remote-client
sleep 15
python3 scripts/capture-stale-hunt.py \
	-0.0859 0.1729 0.0151 0.9811 -0.0859 -0.1729 -0.0151 0.9811 \
	--out /tmp/hunt1
```

Critical: a live app re-renders within ~1–3 frames, so only the first
1–2 burst frames are truly stale — identify them offline with the
hand-masked diff timeline (thousands of px = pre-delivery, hundreds =
stale, ~0 = settled), never by index. Details, masks, attribution
decision tree, and the ghost/double numeric method live in
`docs/openwarp-stale-capture.md`.

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
until grep -q "swapchain for view 1" /tmp/hello_xr-e2e.log 2>/dev/null; do sleep 0.2; done

printf 'state\nhead 0 1.7 0 0 0 0 1\nsend\nquit\n' | \
	remote-driver-client/build/monado-remote-client

kill $APP_PID $MONADO_PID
wait $APP_PID $MONADO_PID 2>/dev/null || true
```

Success signals from the tested path:

- Monado log: `Config selected remote` and `Listening on port '4242'`.
- App log: `swapchain for view 1` (hello_xr prints `Creating color and depth swapchain for view 1`).
- Remote client: `Connected.`, `Ready.`, JSON state output, then `ok` for `state`, `head`, and `send`.

## Are frames actually reaching the compositor? — diagnostic recipe

The canonical smoke test only checks that the app *connects* (`swapchain
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
   the GL↔Vulkan swapchain sync (semaphore) is broken and the layer commit
   never reaches the server. The app will still report `FOCUSED` and its own
   window will keep animating, but Monado's compositor renders nothing. This
   failure was seen historically with the OpenGL playground in this sandbox,
   but current builds submit cleanly (verified: thousands of playground
   frames, zero such failures) — so if it appears now, suspect a regression
   or a wedged service (run `service-down`/`service-up`) rather than a
   permanent environment limitation.

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

## Agent time-traps (symptom → correct signal)

Every one of these cost a session. Check this list before concluding
anything about poses, frames, or the warp:

1. **Coarse polls fake latency.** `sleep 5` + screenshot cannot resolve
   sub-second pose delivery; the first post-`send` capture is already
   transitioning (verified with a dense burst: pre-delivery frame
   md5-identical to fresh, full settle ~0.15 s). Never sleep 25–45 s to
   "settle" — ~10–15 s max, then hunt with `capture-stale-hunt.py`.
2. **Static mirror ≠ wedged app.** With a static head and static scene
   the mirror is *supposed* to be bit-identical across screenshots.
   Liveness signal: two shots at a 90°-asymmetric cube interval (1.3 s,
   not 1/3/10 s — the cubes spin at 0.25 rot/s, so integer seconds land
   on symmetric orientations and hide rotation entirely), or one head
   move + dense polling.
3. **FOCUSED ≠ frames render.** Session state 5 only means the session
   runs; use the "Are frames actually reaching the compositor?" recipe
   before trusting any rendering conclusion.
4. **Mirror capture is broken sandbox-wide since 2026-09-06.** `import`
   dies at startup (`missing an image filename`), plain XGetImage
   (`xwd`, `scrot`) fails with BadMatch, `ffmpeg x11grab` reports
   `screen size 0x0`. When capture works again, the method is:
   `import -window Monado -depth 8 rgb:-` to stdout (~25 ms/shot) +
   in-process numpy parse (see `scripts/capture-stale-hunt.py`), PNG
   encode only for hits — ~4/s PNG encode cannot catch the ~50 ms
   stale window. Details: `docs/openwarp-stale-capture.md`.
5. **Evenly spaced screenshots are fresh by construction.** The app
   resubmits ~100× between 5 s-spaced shots at 50 ms pacing. Hunt
   closed-loop (dense captures + realtime score + stop-on-hit), don't
   sweep blindly and rank later.
6. **Do not slow the app.** `PLAYGROUND_FRAME_MS >= 100` wedges it in
   this sandbox (0% CPU, stuck in `do_wait`, zero submits; the mirror
   then only warp-only updates a frozen frame — spectacular but
   meaningless blackouts). Hunt at default 50 ms pacing.
7. **Zero color pixels ≠ blackout.** The usual cause is the object out
   of view (legitimate clipping). Gate on hands-present (pink+green
   joint counts near baseline) before ranking cube counts, then eyeball
   the winners.
8. **Different-pose comparisons measure parallax, not warp error.** Only
   same-pose stale-vs-fresh pairs count (the hunt script saves both).
9. **One `monado-remote-client` per `send` churns TCP.** A persistent
   connection (stdin pipe, read the `ok`s — see the hunt script's `RC`
   class) lets you alternate poses every ~2 s with no reconnects.
10. **Whole-object blackout with surviving outlines = occlusion false
    positives, not missing splats.** Symptom: an object's body goes black
    while its silhouette stays colored (verified: green hand after a yaw
    jump). The stale-foreground check compared source depth against the
    *warp* view-depth, but head rotation swings each eye several cm, so
    same-surface interiors misread as occluded. Bisect with
    `MONADO_OPENWARP_DISABLE_OCCLUSION=1` (blackout vanishes → the check
    is the killer), fix by comparing against the point's own render-view
    depth (`renderV` in the openwarp UBO, `source_visible()` in
    `openwarp.comp`). Outlines survive because edge depth filtering
    pushes their source depth farther, so they pass the bad check.

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
