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

The active OpenXR runtime is expected to point at Monado's generated manifest:

```text
monado/build/openxr_monado-dev.json -> ~/.config/openxr/1/active_runtime.json
```

## Test apps

`hello_xr` and `openxr-playground` should both get a live stdin pipe so they do not exit immediately on EOF:

```sh
(sleep infinity) | hello_xr -G Vulkan2
(sleep infinity) | openxr-simple-playground/build/openxr-playground
```

Useful readiness checks:

```sh
until grep -q "Creating swapchain for view 1" /tmp/hello_xr.log 2>/dev/null; do sleep 0.2; done
until grep -q "state changed from 4 to 5" /tmp/playground.log 2>/dev/null; do sleep 0.2; done
```

- `hello_xr` going quiet after startup is normal; it does not log per-frame activity.
- `openxr-playground` needs `DISPLAY` and reaches usable input state at session state `5` (`FOCUSED`).

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
- Format only project-owned code. Do not make formatting-only edits in vendored or external code unless explicitly asked.
