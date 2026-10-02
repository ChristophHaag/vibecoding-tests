# AI Agent Guide — steam_frame

Native ARM64 Arch environments (rootless Podman containers) on a Steam Frame
headset, managed by `frame_arch.py` and driven from this PC over SSH.

## Policies (public repo!)

- **Never commit** SSH key paths, known-hosts paths, the Frame's user/host, or
  **environment/container names**. Say "the first env"; use a placeholder like `work`
  in docs.
- Connection options live outside the repo in `~/.config/frame-env.conf`
  (`FRAME_ARCH_ARGS="--ssh USER@HOST --identity KEYFILE --ssh-option …"`) or the
  `$FRAME_ARCH_ARGS` env var. Never echo that file or paste error output containing
  key paths into docs, commits, or logs.
- **All functionality lives in `frame_arch.py`** (the only script synced to the Frame).
  Do not add more scripts. `frame-env.sh` is a *local-only* helper that does nothing
  but pass the connection options from the config to `frame_arch.py`; it is not
  copied to the Frame. Put new features in the Python script and document them in
  `FRAME_ARCH.md`.
- Never `rm -rf` an env directory; use `frame_arch.py remove`.

## What the envs are

Each env is a minimal Arch Linux ARM64 container with its own base system and
Podman storage plus a home (`shared` host home / `private` / `encrypted`). The host
filesystem is at `/run/host`; passwordless sudo is container-only, not host root.
State: `~/.local/share/frame-arch/NAME` on the Frame. Details: `FRAME_ARCH.md`.

## Entering efficiently

```sh
steam_frame/frame-env.sh list                       # env names, one per line
steam_frame/frame-env.sh enter                      # login shell in the FIRST env (needs a tty)
steam_frame/frame-env.sh enter NAME                 # named env
steam_frame/frame-env.sh enter -- CMD ARGS…         # command in the first env
steam_frame/frame-env.sh enter NAME -- CMD ARGS…    # command in NAME
```

- **Agents: use `enter [NAME] -- CMD…`.** It needs no tty and preserves the exit
  status. For several steps use `enter -- bash -c '…'`; each call is a separate SSH
  connection plus script staging, so batch work.
- `frame_arch.py` itself sets `MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink`
  on every entry; do not set them elsewhere.
- Default env = first of `list` (sorted). Never hardcode its name.
- `../../ssh_frame.sh` is a raw host shell (command read from stdin, no args/tty) —
  use it only for host-level commands, e.g. `echo 'cmd' | bash ../../ssh_frame.sh`.
- Entering starts the container and leaves it running; stop with
  `frame-env.sh stop NAME`.

## Syncing with the Frame

`frame_arch.py` and `FRAME_ARCH.md` are mirrored to the Frame's `~`:

```sh
steam_frame/frame-env.sh sync          # status: same / DIFFERENT / remote-missing
steam_frame/frame-env.sh sync push     # repo → Frame (repo is the source of truth)
steam_frame/frame-env.sh sync pull     # Frame → repo (recover Frame-side edits; review git diff)
```

Run `sync push` after changing either file. Per-env staged copies of the script
are refreshed automatically on every SSH `enter`/`create`.

## Tests

`cd steam_frame && python3 -m unittest test_frame_arch` (unit tests only; no Frame needed).

## Keep current

Update this file when commands or policies change.
