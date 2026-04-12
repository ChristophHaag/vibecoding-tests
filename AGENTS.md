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
