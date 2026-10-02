# Frame Arch environments

`frame_arch.py` creates named, minimal native ARM64 Arch environments on Steam
Frame using the unofficial Arch Ports/drzee repositories. It works directly on
the Frame or over SSH from a PC, using Python's standard library and the Frame's
existing rootless Podman/crun. No Distrobox installation, host package changes,
host repository configuration, or replacement desktop services are needed.

Each environment has its **own base system and container storage**, plus a shared,
private, or encrypted home. There is intentionally no shared image store: deleting
one environment does not break another.

New environments also expose the host filesystem **read-write at `/run/host`**,
subject to normal host permissions and existing read-only filesystem mounts.
The outside home is `/run/host/home/steamos` on the Frame. This is separate from
the environment's own `$HOME`, including when that home is encrypted.

## Usage on the Frame

Run as the normal desktop user, **not with sudo**:

```sh
python3 frame_arch.py create work --home shared
python3 frame_arch.py enter work

python3 frame_arch.py create personal --home encrypted
python3 frame_arch.py enter personal
python3 frame_arch.py stop personal

# Optional: separate home without encryption.
python3 frame_arch.py create scratch --home private
python3 frame_arch.py enter scratch -- /bin/bash -c 'id; uname -m'
```

Names start with a lowercase letter, contain only `a-z`, `0-9`, `_`, and `-`, and
are at most 48 characters. The default home mode is `shared`.

Creation downloads and verifies the drzee ARM64 bootstrap, upgrades it, installs
`base` and `sudo`, and creates a user matching the host UID/GID. No kernel,
desktop, graphics stack, or VR packages are added beyond bootstrap/base
dependencies. Inside the container, passwordless sudo controls the container's
system, **not host root**.

The container is created stopped. `enter` starts it and opens the container
user's configured login shell, read from its passwd database on each entry.
New environments default to Bash; the host's login shell or `$SHELL` does not
override the container's setting. An empty passwd shell field uses `/bin/sh`.
The selected shell starts with `-l` and is exported as `$SHELL`.

To change the default inside an environment, for example:

```sh
sudo pacman -S --needed zsh
sudo chsh -s /usr/bin/zsh "$USER"
```

An explicit command following `--` runs directly, without a login-shell wrapper,
preserving its exit status:

```sh
python3 frame_arch.py enter work -- sudo pacman -Syu
python3 frame_arch.py enter work -- firefox
```

Exiting the shell **does not stop the container or lock an encrypted home**.
Use `stop` explicitly. `enter` asks for the encryption passphrase only while the
home is locked; `create` also asks for confirmation.

## Usage from a PC

The same local script stages a copy inside the target environment directory.
Nothing is installed globally on either machine; SSH keys stay on the PC.
Configure the host/key/known-hosts policy normally in `~/.ssh/config`, or pass
options explicitly:

```sh
python3 frame_arch.py --ssh steamos@frame --identity ~/.ssh/frame_key \
  create work --home shared
python3 frame_arch.py --ssh steamos@frame --identity ~/.ssh/frame_key \
  enter work
```

Repeated `--ssh-option KEY=VALUE` forwards ordinary SSH options, for example
`--ssh-option UserKnownHostsFile=/path/to/known_hosts`. SSH host verification
is not disabled. An existing SSH wrapper is unnecessary.

For convenient repeated use in a shell:

```sh
framearch() {
  python3 ./frame_arch.py --ssh steamos@frame --identity ~/.ssh/frame_key "$@"
}
framearch create personal --home encrypted
framearch enter personal
framearch stop personal
```

For this workspace's existing `ssh_frame.sh` connection settings, use:

```sh
framearch() {
  python3 ./frame_arch.py --ssh steamos@frame --identity ~/ssh_key \
    --ssh-option UserKnownHostsFile="$PWD/ssh_hosts" \
    --ssh-option GlobalKnownHostsFile="$HOME/ssh_hosts_global" "$@"
}
```

Encryption can also read **one passphrase line** from stdin with the global
`--passphrase-stdin` option, allowing a password manager to supply it without
putting it in arguments or a file. This disables SSH terminal allocation, so
prefer an explicit noninteractive command rather than an interactive shell.
The script never saves the passphrase.

## State and removal

Default location: `~/.local/share/frame-arch/NAME` on the Frame, keeping mutable
state on `/home` rather than SteamOS's read-only system partition.

Use global `--root /absolute/path` to choose another parent directory; over SSH
`--root '~/.local/share/another-parent'` is also supported. Use the same root on
subsequent commands. Prefer a local filesystem, such as the Frame's ext4 `/home`;
the default storage driver is `overlay`. `create --storage-driver vfs` is a
slower, more space-intensive fallback.

The single environment directory contains the manager copy, metadata, downloads
and signing keys, build logs, isolated Podman configuration/storage/runtime state,
container file logs, and any private home or encrypted backing files. Transient
processes and systemd cgroup scopes necessarily exist while the environment is active. A boot stamp
discards stale runtime files after a reboot, preserving the base system and home.
Cleanup runs inside Podman's user namespace because crun's stale `tmpmount`
directories can be owned by subordinate UIDs, making ordinary host-user removal
fail with `Permission denied: 'tmpmount'`. The old runtime is first moved aside
so Podman starts with fresh runtime state; interrupted cleanup is retried on the
next invocation. This does not delete container storage or encrypted-home data.

```sh
python3 frame_arch.py stop work
python3 frame_arch.py remove work
python3 frame_arch.py remove personal --yes
```

**Use `remove`, not `rm -rf` on a live environment.** It first stops the container,
unmounts an encrypted home normally (not lazily), resets only that directory's
isolated engine, and deletes that one directory. It refuses deletion if host
mounts remain beneath it. No shared host-home files or other environments are
deleted. The parent directory may remain empty.

Interrupted creation leaves explicit partial state; `remove NAME --yes`, then
recreate. Do not use an unqualified `podman system reset`: that would address
your ordinary Podman store, not this script's isolated one.

For an environment created before `/run/host` support was added:

```sh
python3 frame_arch.py mount-host work
# Also supported over SSH, with the same connection options:
python3 frame_arch.py --ssh steamos@frame mount-host work --yes
```

This stops active shells/apps, commits the existing container filesystem so
installed software and system configuration survive, and recreates the container
with the host mount. A previously running container is restarted; a stopped one
stays stopped. The home is not replaced, and an unlocked encrypted home stays
unlocked. Ordinary migration failures restore the previous container. A reserved
`NAME-host-mount-backup` is retained until the new container is ready; an abrupt
termination may leave it for explicit recovery. Do not delete the environment to
resolve a migration backup. Migration diagnostics are in `host-mount.log`.
Active exec sessions are ended before stopping the container, avoiding the
host-PID-namespace cleanup refusal. The Frame's Podman 5.5 repeats SIGTERM where
its forced exec shutdown should send SIGKILL, which leaves interactive Zsh
sessions alive. The script signals only recorded exec PIDs after verifying their
container cgroup, using PID file descriptors to avoid PID-reuse races, then asks
Podman to clean up those sessions. SIGTERM gets ten seconds before SIGKILL.

## Home modes and encryption

| Mode | Container home | Important consequence |
|------|----------------|-----------------------|
| `shared` | Existing host home, writable | Native access to existing files and configuration; applications can modify them |
| `private` | Environment's `home/`, mounted at the usual host home path | Separate configuration and files, without encryption |
| `encrypted` | gocryptfs view of the environment's `cipher/` | Home content and names encrypted at rest; passphrase required to unlock |

Encrypted mode downloads the official static ARM64 gocryptfs 2.6.1 release and
verifies its signature. FUSE mounting/unmounting happens **inside the isolated
Podman user/mount namespace**. `allow_other` lets container-mapped identities
access the mount there; this does not require changing `/etc/fuse.conf` or
enabling host-wide `user_allow_other`. The environment directory is mode 0700.
The host-visible `home/` mountpoint remains empty; plaintext is visible to the
container and processes entering its namespace.

Only the home is encrypted, **not the base filesystem, package cache, build logs,
host filesystem exposed under `/run/host`, host-shared `/tmp`, or runtime sockets**.
Applications writing outside their home
can leave plaintext elsewhere. Encryption does not isolate applications from the
host or other same-user processes. Back up persistent data while stopped; do not
restore old `run/` contents or a `boot-id` stamp from a full environment backup.
At minimum, keep all of `cipher/`, including `gocryptfs.conf` and directory-IV files.
The script does not retain an unencrypted master key or provide password recovery.

## Host integration and optional packages

The container uses the host network, IPC and PID namespaces and preserved
supplementary device groups. Rootless privileged mode exposes host devices;
additional mounts expose GPU, USB, input and audio device directories and the
Frame's video-device aliases while retaining Podman's private `/dev/pts` for
working interactive terminals. Read-only `/sys`, `/tmp`, the whole host
`/run/user/UID`, and the system D-Bus socket are also mounted. It discovers the current
desktop environment locally, or from the same user's running `plasmashell` over
SSH. Multiple Plasma sessions require running from the intended desktop terminal.

This makes the existing KWin/Plasma, PipeWire, XDG portals and NetworkManager
reachable. It deliberately does **not** forward host `LD_LIBRARY_PATH`: the
Steam runtime's libraries must not accidentally override the newer Arch userland.
Ordinary host permissions, ACLs, polkit authorization, and hardware support still
apply; rootless sudo cannot grant missing host USB permissions. Do not start
replacement PipeWire, NetworkManager, Plasma or SteamVR daemons inside it.

Install clients/drivers only when needed, for example inside the environment:

```sh
sudo pacman -Syu --needed mesa vulkan-freedreno vulkan-tools ffmpeg pipewire
sudo pacman -S --needed openvr openxr
# Optional host-network management client and desktop application:
sudo pacman -S --needed networkmanager firefox
```

`vulkan-freedreno` supplies the container's native Turnip driver. Installing
`networkmanager` is for its client tools, not for starting its daemon. The script
is an integrated trusted-user environment, **not a security sandbox**: host
devices, services, runtime sockets and the host filesystem are accessible.
Container sudo does not become host root; the host's normal permissions still
limit access under `/run/host`. Writes through `/run/host` affect real host files
and are outside the encrypted home.

When available, host `/opt/steamvr` is mounted read-only, the host OpenVR registry
is mounted separately even for a private home, and `VR_OVERRIDE`,
`VR_PATHREG_OVERRIDE`, and `XR_RUNTIME_JSON` point clients at host SteamVR.
These are native ARM64 clients, not FEX/Proton integration.

Live checks on the Frame demonstrated ARM64 entry, private-home writes,
passwordless container sudo, a desktop-portal property query, host PipeWire
connectivity, container Turnip Vulkan enumeration and five-frame Wayland cube
presentation on host KWin, actual H.264 V4L2 hardware
decoding, an OpenVR background client connecting to host SteamVR and detecting the
HMD, OpenXR runtime/system enumeration through host SteamVR, and creation of a
native `hello_xr` Vulkan2 session. The bounded sample did not begin the session
or submit frames, so rendering to the headset remains unverified. Encrypted
creation, unlock, write, stop/relock, wrong-password rejection, re-unlock and
removal also worked without host FUSE-policy changes. Shared-home access,
stop/restart, preservation of another environment during removal, concurrent
entry, command exit-status propagation, and interactive SSH/container terminals
were also exercised.

These checks do **not** establish every application's compatibility, all USB
permissions, Wi-Fi changes authorized by polkit, every codec or browser's
acceleration, OpenGL ES compatibility, or actual OpenVR/OpenXR frame submission.
FEX support and desktop-entry export are not implemented.

## Trust and prerequisites

The drzee repositories are unofficial. Signature checking verifies the selected
signer's artifacts, not their safety or reproducibility. The bootstrap's pinned
primary-key fingerprint is:

```text
9B2C213B21883BB65CE2FB900CF25682E6BA0751
```

Installed packages require signatures (`Required DatabaseOptional`); repository
database signatures are optional. The official gocryptfs release key is:

```text
FFF3E01444FED7C316A3545A895F5BC123A02740
```

Unexpected keys or invalid/missing artifact signatures abort creation. All sources
are public; private Frame repository URLs are never used. During investigation,
drzee's database briefly reported an inconsistent `python-platformdirs` entry
despite a successful pacman transaction. Successful-command `error:` diagnostics
are surfaced; full build output is retained in `create.log`. The script does not
rewrite repository metadata or relax signatures to work around upstream issues.

The checked Frame already supplied Python 3, Podman, crun, subordinate UID/GID
ranges, a user systemd/D-Bus session, GNU tar with zstd, GPG/gpgv, findmnt, and
fusermount3 plus `/dev/fuse`. Local execution requires AArch64 and a non-root
desktop user. A PC needs Python 3 and OpenSSH; dependency/key checks happen remotely.

Sources and context:

- [Arch Linux Ports AArch64](https://ports.archlinux.page/aarch64/)
- [drzee repositories and bootstrap](https://arch-linux-repo.drzee.net/arch/)
- [Podman rootless user namespaces](https://docs.podman.io/en/latest/markdown/podman-unshare.1.html)
- [Podman container creation](https://docs.podman.io/en/latest/markdown/podman-create.1.html)
- [Podman 5.5 exec shutdown implementation](https://github.com/containers/podman/blob/v5.5.2/libpod/oci_conmon_exec_common.go)
- [gocryptfs manual](https://nuetzlich.net/gocryptfs/man/)
- [Distrobox integration model](https://distrobox.it/)
- [Earlier repository and platform research](STEAM_FRAME_NATIVE_LINUX_OPTIONS.md)
