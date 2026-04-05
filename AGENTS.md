# AI Agent Guide — vibecoding-tests

This document describes how to run, use, and extend the OpenXR test automation
tooling in this repository.  It is written for AI agents operating in a
non-interactive shell.

---

## Repository layout

```
vibecoding-tests/
├── monado/                        Monado OpenXR runtime (git submodule)
│   └── build/                     Pre-built Monado (cmake -B monado/build monado)
├── openxr-simple-playground/      Sample OpenXR app (independent CMake project)
├── remote-driver-client/          AI agent remote control tool (source)
│   ├── main.c
│   └── CMakeLists.txt
├── build/                         Root build output
│   └── remote-driver-client/
│       └── monado-remote-client   ← the tool binary
├── CMakeLists.txt                 Root build (builds remote-driver-client only)
└── AGENTS.md                      This file
```

---

## The remote-driver-client tool

`monado-remote-client` connects to Monado's remote simulation driver over TCP
and lets an AI agent inject head and controller poses/inputs into the running
OpenXR runtime, enabling fully scripted testing of OpenXR applications without
physical hardware.

The tool's struct definitions come directly from the Monado submodule headers
(`monado/src/xrt/drivers/remote/r_interface.h`) — never copied.  Rebuilding
after a Monado protocol change picks up the new layout automatically.

---

## Building

From the repository root (only needs to be done once, or after source changes):

```sh
cmake -B build
cmake --build build
```

Binary: `build/remote-driver-client/monado-remote-client`

`monado/build` must already exist (generated headers live there).  To build
Monado itself from scratch:

```sh
cmake -B monado/build monado
cmake --build monado/build
```

---

## Running Monado with the remote driver

Always start Monado before the tool.  Use all three env vars when running
headless or in a non-interactive shell:

```sh
P_OVERRIDE_ACTIVE_CONFIG=remote \
XRT_COMPOSITOR_FORCE_XCB=1 \
XRT_NO_STDIN=1 \
monado/build/src/xrt/targets/service/monado-service &
```

- `P_OVERRIDE_ACTIVE_CONFIG=remote` — selects the remote simulation builder
  (it is excluded from automatic discovery; must be explicit)
- `XRT_COMPOSITOR_FORCE_XCB=1` — forces XCB compositor when not on real VR hardware
- `XRT_NO_STDIN=1` — required in non-interactive / piped shells; without it
  Monado may hang waiting for stdin

Monado is the active OpenXR runtime system-wide via:

```
monado/build/openxr_monado-dev.json → ~/.config/openxr/1/active_runtime.json
```

Wait for the line `Listening on port '4242'` in Monado's output before
connecting the tool or launching an OpenXR app.

---

## Using the remote client

```sh
build/remote-driver-client/monado-remote-client [host [port]]
# defaults: 127.0.0.1  4242
```

The tool reads commands from stdin, one per line, and writes `ok` or
`error: <message>` to stdout for each.  Status/diagnostic messages go to
stderr.  This makes it safe to pipe commands in and check stdout only.

On connect the server sends two packets: the reset state and the latest state.
The tool starts with the latest state as its working copy.

### Startup sequence

```sh
printf 'state\nquit\n' | build/remote-driver-client/monado-remote-client
```

### Commands

| Command | Effect |
|---|---|
| `head X Y Z QX QY QZ QW` | Set HMD center pose (position + quaternion) |
| `left_pose X Y Z QX QY QZ QW` | Set left controller pose |
| `right_pose X Y Z QX QY QZ QW` | Set right controller pose |
| `left_trigger VALUE` | Left trigger axis \[0..1\] |
| `right_trigger VALUE` | Right trigger axis \[0..1\] |
| `left_squeeze VALUE` | Left squeeze axis \[0..1\] |
| `right_squeeze VALUE` | Right squeeze axis \[0..1\] |
| `left_thumbstick X Y` | Left thumbstick \[-1..1, -1..1\] |
| `right_thumbstick X Y` | Right thumbstick |
| `left_trigger_click 0\|1` | Left trigger click button |
| `left_a_click 0\|1` | Left A button |
| `left_b_click 0\|1` | Left B button |
| `left_system_click 0\|1` | Left system button |
| `left_thumbstick_click 0\|1` | Left thumbstick click |
| `right_trigger_click 0\|1` | (same set for right controller) |
| `right_a_click 0\|1` | |
| `right_b_click 0\|1` | |
| `right_system_click 0\|1` | |
| `right_thumbstick_click 0\|1` | |
| `send` | **Transmit current working state to Monado** |
| `state` | Print current working state as JSON to stdout |
| `reset` | Revert working state to the server's initial state |
| `quit` / `exit` | Disconnect and exit |

**Important:** state changes (poses, buttons, axes) only take effect in Monado
after a `send` command.  You can batch multiple changes before sending.

Lines beginning with `#` and blank lines are ignored.

### Default positions (server reset state)

- Head: position (0, 1.6, 0), identity rotation
- Left controller: position (-0.2, 1.3, -0.5), identity rotation
- Right controller: position (0.2, 1.3, -0.5), identity rotation
- All buttons/axes: 0 / false

---

## Typical AI agent test workflow

```sh
# 1. Start Monado (background)
P_OVERRIDE_ACTIVE_CONFIG=remote XRT_COMPOSITOR_FORCE_XCB=1 XRT_NO_STDIN=1 \
    monado/build/src/xrt/targets/service/monado-service &
MONADO_PID=$!

# 2. Start the OpenXR app under test (background)
# Pipe sleep infinity to prevent hello_xr's "press any key" stdin read from
# getting immediate EOF and causing it to exit.
(sleep infinity) | hello_xr -G Vulkan2 &
APP_PID=$!

# 3. Control the simulated devices — smooth 90° head sweep over 5 seconds
python3 - <<'PYEOF' | build/remote-driver-client/monado-remote-client
import sys, math, time
steps = 25
for i in range(steps + 1):
    t = i / steps
    half_rad = math.radians(t * 90.0 / 2.0)   # yaw right 0→90°
    qy, qw = math.sin(half_rad), math.cos(half_rad)
    print(f"head 0 1.6 0  0 {qy:.5f} 0 {qw:.5f}")
    print("send")
    sys.stdout.flush()
    if i < steps:
        time.sleep(0.2)
print("quit")
PYEOF

# 4. Observe app output / take screenshot, then clean up
kill $APP_PID $MONADO_PID
```

---

## OpenXR test app

`hello_xr` is a system-wide OpenXR reference app:

```sh
# Must pipe stdin — without it hello_xr reads EOF on "press any key" and exits.
(sleep infinity) | hello_xr -G Vulkan2 >/tmp/hello_xr.log 2>&1 &
```

Wait for swapchain creation before sending input:

```sh
until grep -q "Creating swapchain for view 1" /tmp/hello_xr.log 2>/dev/null; do sleep 0.2; done
```

The `openxr-simple-playground` app (in `openxr-simple-playground/`) is a more
feature-rich test app built with SDL2 + OpenGL.  It uses the active runtime
symlink and exercises hand tracking, plane detection, and the
`XR_MNDX_xdev_space` extension.

### Building openxr-playground

```sh
# From the vibecoding-tests/ root — only needed once or after source changes:
cmake -B openxr-simple-playground/build openxr-simple-playground
cmake --build openxr-simple-playground/build
```

Binary: `openxr-simple-playground/build/openxr-playground`

### Running openxr-playground

Monado must be running first (see above).  The app uses SDL2 so it requires a
display (`DISPLAY` must be set).  It reads from stdin only on exit; pipe
`sleep infinity` to avoid immediate EOF:

```sh
# Start Monado (background), wait for it to be ready
P_OVERRIDE_ACTIVE_CONFIG=remote XRT_COMPOSITOR_FORCE_XCB=1 XRT_NO_STDIN=1 \
    /home/haagch-demo/projects/vibecoding-tests/monado/build/src/xrt/targets/service/monado-service \
    >/tmp/monado.log 2>&1 &
MONADO_PID=$!
until grep -q "Listening on port" /tmp/monado.log 2>/dev/null; do sleep 0.2; done

# Run the playground (pipe stdin so it doesn't get EOF immediately)
LOG=/tmp/playground.log
(sleep infinity) | openxr-simple-playground/build/openxr-playground >"$LOG" 2>&1 &
APP_PID=$!
```

Wait for the session to reach FOCUSED state before sending input:

```sh
until grep -q "state changed from 4 to 5" "$LOG" 2>/dev/null; do sleep 0.2; done
# "state 5" = XR_SESSION_STATE_FOCUSED — app is rendering and accepting input
```

Clean up:

```sh
kill $APP_PID $MONADO_PID
wait
```

**Success indicators** in `$LOG`:
- `Successfully created a session with OpenGL!`
- `Session started!`
- `EVENT: app->oxr.session app->oxr.state changed from 4 to 5` (FOCUSED — rendering)
- `Cleaned up!` (after receiving SIGTERM / exit request)

**Failure indicators**: any line containing `XR_ERROR`, `assert`, or `Segmentation fault`.

The playground's stdout is verbose: it prints extension availability, supported
swapchain formats, action bindings, and per-event state transitions.  Per-frame
output is intentionally absent — silence after `Session started!` is normal.

---

## Updating Monado

After pulling changes to the `monado/` submodule, rebuild Monado and then
rebuild the tool so its headers are current:

```sh
cmake --build monado/build
cmake --build build
```

No source edits needed — the tool includes Monado headers directly and
never contains copied struct definitions.

---

## Notes and caveats

- Before finalizing C/C++ changes, run `git clang-format` to normalize style.
- Accept formatting edits only in project-owned code (for example
  `openxr-simple-playground/` and `remote-driver-client/`).
- Do not accept formatting-only edits in external or vendored code (for example
  `openxr-simple-playground/external/` or `monado/`) unless explicitly asked.

- Only one client can be connected to the remote driver at a time.  If a
  previous client crashed without disconnecting, Monado may need to be
  restarted.
- The remote driver is **not** auto-discovered; it must be selected explicitly
  with `P_OVERRIDE_ACTIVE_CONFIG=remote`.
- The TCP port defaults to 4242 and is not configurable at runtime (set in
  Monado's config JSON if needed; see `monado/src/xrt/targets/common/target_builder_remote.c`).
- `hello_xr` does **not** log per-frame output, so its log going quiet after
  swapchain creation is normal — it is in the render loop.  Use `ps` or the
  `wchan` value `hrtimer_nanosleep` to confirm it is alive and looping.
- All paths above are relative to the `vibecoding-tests/` repository root.
