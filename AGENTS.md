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

# 3. Control the simulated devices
{
  # Look left 45 degrees (rotate head around Y axis)
  echo 'head 0 1.6 0  0 0.383 0 0.924'
  echo 'send'

  # Press and release right trigger
  echo 'right_trigger 1.0'
  echo 'right_trigger_click 1'
  echo 'send'
  echo 'right_trigger 0.0'
  echo 'right_trigger_click 0'
  echo 'send'

  # Print state for verification
  echo 'state'
  echo 'quit'
} | build/remote-driver-client/monado-remote-client

# 4. Observe app output / take screenshot, then clean up
kill $APP_PID $MONADO_PID
```

---

## OpenXR test app

`hello_xr` is a system-wide OpenXR reference app:

```sh
hello_xr -G Vulkan2
```

The `openxr-simple-playground` app (in `openxr-simple-playground/`) is a more
feature-rich test app built with SDL2 + OpenGL.  It uses the active runtime
symlink and exercises hand tracking, plane detection, and the
`XR_MNDX_xdev_space` extension.

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

- Only one client can be connected to the remote driver at a time.  If a
  previous client crashed without disconnecting, Monado may need to be
  restarted.
- The remote driver is **not** auto-discovered; it must be selected explicitly
  with `P_OVERRIDE_ACTIVE_CONFIG=remote`.
- The TCP port defaults to 4242 and is not configurable at runtime (set in
  Monado's config JSON if needed; see `monado/src/xrt/targets/common/target_builder_remote.c`).
- All paths above are relative to the `vibecoding-tests/` repository root.
