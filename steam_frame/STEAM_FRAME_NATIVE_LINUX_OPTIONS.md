# Native Linux userland options on Steam Frame

Research and live observations: **2026-09-30**.

This document records the investigation, not an implemented setup. No container
was created, no packages were installed, and no Frame configuration was changed.
Repository metadata was fetched into memory, and small host-side capability
checks were run. Package counts and versions are a dated snapshot.

**Subsequent implementation:** `frame_arch.py` now provides named drzee-based
environments with shared, private, or encrypted homes. See [FRAME_ARCH.md](FRAME_ARCH.md)
for usage, cleanup, and later container-side observations. The research findings
below describe the earlier investigation rather than claiming the setup remains
unimplemented.

**Private Frame repository URLs are intentionally omitted.** Do not copy those
URLs into public documentation, issue reports, images, or external searches.

## Goal and scope

Install and run native ARM64 Linux applications from a mutable userland while
using the existing host KWin/Plasma session. Desired integration includes
PipeWire, USB devices, XDG portals, Wi-Fi control, real GPU access, hardware video
decoding, and OpenXR/OpenVR applications connecting to the host SteamVR.

Two home profiles are desired: the existing desktop user's home, and a separate
encrypted home. FEX integration is a bonus; Lepton is outside the initial scope.

## Main conclusions

- Rootless **Distrobox with Podman** is the best-documented fit for the integrated
  desktop userland. Keep Plasma, PipeWire, NetworkManager, and SteamVR on the host.
- A public **Valve/Collabora Holo Core ARM64 container exists**, with an explicit
  Distrobox example. It is a substantial but incomplete, frozen development
  preview, not the complete Frame operating system.
- The Frame's own configured repositories are a different, device-oriented
  package set. They include packages absent from the public preview, such as
  Firefox and Frame-specific graphics/VR components.
- **Arch Linux Ports/drzee** provides much broader, actively maintained ARM64
  coverage, but it is unofficial and requires trusting its maintainer and build
  infrastructure.
- Host Vulkan, H.264 hardware decoding, desktop audio connectivity, and a native
  OpenVR background-client connection were verified. **Container rendering,
  container hardware decoding, and container OpenXR/OpenVR frame submission were
  not verified.** The complete mandatory feature set is therefore not yet proven.

## Userland choices

| Option | Advantages | Limitations |
|---|---|---|
| Public Holo Core preview | Valve/Collabora provenance; ready-made ARM64 OCI images; documented Distrobox use | Frozen November 2025 package state; incomplete selection; no automatic Frame graphics/SteamVR integration |
| Frame-repository-matched userland | Matches the device's base and release overlay; includes its browser and hardware-specific packages | No ready-made matching container was established; configured endpoints are non-public; package selection is limited |
| Arch Ports/drzee userland | Nearly complete package-count coverage; current desktop/development software | Unofficial; independent maintainer/build/signing trust; Frame integration remains separate |
| Arch Linux ARM userland | Separate established ARM distro; community ARM64 OCI images exist | Not upstream Arch or Valve's port; compatibility with Frame-specific libraries still needs verification |

Choose a coherent base. Do not casually combine Holo preview, Frame release, and
drzee repositories: their library versions, SONAME transitions, package splits,
and dependency sets differ.

### Public Holo Core containers

Published images:

```text
registry.gitlab.steamos.cloud/holo/holo-core-aarch64-preview/base:latest
registry.gitlab.steamos.cloud/holo/holo-core-aarch64-preview/base-devel:latest
```

Both `latest` tags were confirmed through the public GitLab registry API. The
announcement demonstrates rootless Distrobox creation with `linux/arm64`.
Neither image was pulled or run during this investigation.

The published repository configuration contains only public preview `core` and
`extra`, not the Frame's private release overlay. The public mirror alias
`mash-20251118` redirected to `mash-20251118.3` at the time of investigation.

The package state is based on **2025-11-18**, despite published database files
being updated in July 2026. The project explicitly describes this as an "as is"
technology preview, not a permanent OS or production environment. Do not assume
that `latest` means current upstream Arch or ongoing security updates.

#### Public Holo package coverage

Counts from the actual non-debug package databases:

| Repository | Holo ARM64 preview | Upstream Arch x86_64 archive, 2025-11-18 | Count ratio |
|---|---:|---:|---:|
| `core` | 254 | 276 | 92.0% |
| `extra` | 4,306 | 14,508 | 29.7% |
| Total | 4,560 | 14,784 | 30.8% |

Holo contains **2,992 ARM64 packages plus 1,568 architecture-independent
packages**. These are binary-package counts, not distinct applications. Ratios
compare counts, not an exact intersection of package names.

The selection is the development/image-creation seed set plus its runtime and
build dependencies. It is much larger than a minimal Steam runtime, but not a
complete rebuild of Arch.

| Area | Present in public Holo |
|---|---|
| Development | GCC, Clang, Rust, Go, Python, Node.js, OpenJDK, Git, CMake, Ninja |
| Desktop | Plasma, Qt, KDE portal backend, Chromium, LibreOffice, GIMP, Inkscape, Kdenlive |
| Multimedia/graphics | MPV, VLC, FFmpeg, PipeWire, Mesa, Turnip (`vulkan-freedreno`), Vulkan tools, OpenXR |
| Container/encryption tools | Podman, gocryptfs |

Checked and absent: **Firefox, Thunderbird, Blender, Krita, Distrobox, CryFS**.
Absence from this selection does not imply that an application cannot run on ARM64.

Representative versions: glibc 2.42, GCC 15.2, Python 3.13, Rust 1.91, Qt 6.10,
Plasma 6.5, Mesa 25.2.7, Chromium 142, and FFmpeg 8.

### The Frame's configured repositories

Effective priority order:

| Repository | Purpose | Non-debug package entries |
|---|---|---:|
| `deckard-arch-hotfixes-release-0.3` | First-priority Frame release overlay | 768 |
| `core` | Pinned Valve ARM64 foundational package rebuild | 239 |
| `extra` | Corresponding desktop/library/application rebuild | 3,680 |
| `core-debug` | Base debug-symbol companion | Not included in totals |
| `extra-debug` | Extra debug-symbol companion | Not included in totals |

There are **4,687 non-debug entries and 4,429 unique package names**, because the
overlay replaces some base packages.

The names `core` and `extra` do **not** mean upstream Arch's public repositories.
Their servers select a particular Valve ARM64 build pipeline. The live base
databases were last modified on **2025-10-17**; the overlay was modified on
**2026-09-28**. This is a frozen base with selected updates, not rolling Arch.

Live metadata and package-file availability were checked without refreshing
pacman's on-disk databases.

| Package | Frame overlay version | Recorded build date |
|---|---|---|
| Firefox | `152.0.3-1` | 2026-06-26 |
| Chromium | `129.0.6668.58-1.14` | 2026-08-25 |
| Qt base | `6.8.0-2.1` | 2025-12-09 |
| FFmpeg | `2:7.0-1.1` | 2025-12-02 |
| Frame Mesa | `26.3.0_devel+git8aa73b4b-1` | 2026-08-31 |
| Frame SteamVR | `r25358740+fefbdc82-1` | 2026-09-28 |

Neither browser was installed. Both package artifacts were reachable. A
print-only Firefox transaction resolved successfully to Firefox plus `libxss`,
given the host's already-installed dependencies. No package was downloaded or
installed, and application launch was not tested. A recent build date alone
does not establish current browser security backports.

Other available packages include GIMP, Inkscape, Kdenlive, MPV, VLC, development
tools, Plasma, PipeWire, and OpenXR. Checked and absent: Blender, Krita,
LibreOffice, Thunderbird, Distrobox, gocryptfs, and CryFS.

The public Holo image does **not** automatically expose these sources.
Configuring a container locally to use the device's sources is technically
possible, but has not been implemented or tested. A userland assembled against
the same base-plus-overlay family is more coherent than simply adding the
overlay to the public preview. For example, the Frame base uses glibc 2.39,
whereas public Holo uses glibc 2.42; other ABI differences also matter.

Do not interpret package availability as a recommendation to modify the
immutable host root filesystem.

### Upstream Arch and Arch Ports/drzee

Upstream Arch still officially supports only x86_64. Its AArch64 Ports page
records **Unofficial** status and **no architecture RFC**. This is an
upstream-associated preparatory effort, not official ARM64 binary repositories.
It is distinct from the Arch Linux ARM project.

Arch Ports documents drzee repositories with `core`, `extra`, and a small
supplementary `forge` repository. Packages target **ARMv8.2-A or newer**, suitable
for the Frame's CPU.

Counts from current package databases:

| Repository | Package entries |
|---|---:|
| `core` | 266 |
| `extra` | 14,889 |
| Total | 15,155 |

The dashboard reported 15,304 packages in the corresponding x86_64 set, roughly
99% by package count. This does not establish identical package names, version
parity, or flawless dependency closure. It also reported 248 outdated
architecture-independent packages.

All checked examples were present: Firefox, Chromium, Thunderbird, Blender,
Krita, LibreOffice, GIMP, Inkscape, Kdenlive, MPV, VLC, development tools,
Distrobox, Podman, gocryptfs, CryFS, Plasma, PipeWire, OpenXR, and Turnip.

Representative versions were Python 3.14, Rust 1.98, Qt 6.11, Plasma 6.7,
Mesa 26.2, and FFmpeg 9. Bootstrap root filesystems are published; the latest
listed bootstrap was dated **2026-09-15**. It was not downloaded or imported.

For comparison, the regular Docker Official `archlinux` image is AMD64-only.
An ARM64 image setting does not convert it into a native userland. An alternative
community Arch Linux ARM image is `ghcr.io/menci/archlinuxarm:base`; its build
workflow documents ARM64 support, but it was not tested on the Frame.

## Distrobox integration with the existing desktop

Keep host services on the host. Use the container for applications, libraries,
package installation, and development tools.

| Feature | Integration approach and limitations |
|---|---|
| Wayland/XWayland | Use the existing Plasma display sockets and launch environment; do not start a second compositor |
| Desktop launcher | `distrobox-export` can export applications and CLI wrappers to the host |
| Themes/settings | Use appropriate Qt/GTK integration packages; shared home shares many settings, custom home needs its own settings |
| PipeWire | Container-side client libraries connect to host sockets; no second audio server |
| XDG portals | Preserve the Plasma session bus and use its host KDE backend; individual portal operations remain to be tested |
| USB | Share devices and udev information; host permissions still apply; container-only udev rules do not grant host access |
| Wi-Fi/Bluetooth/mounts | Keep host daemons; use `distrobox-host-exec` for reliable host control, or explicitly expose the system bus with host authorization |
| System administration | Container package administration works; container root is not host root in rootless mode |

The stable Distrobox implementation deliberately excludes the host **system
D-Bus socket** from automatic socket integration. Ordinary networking does not
imply that container NetworkManager clients can control host Wi-Fi automatically.
The existing host Plasma panel can continue controlling it.

The official Steam Deck guide is useful background, but must not be copied
verbatim: it has AMD64-specific downloads and a workaround diverting clients
away from native PipeWire.

### Observed Frame desktop arrangement

- The host is SteamOS ARM64 with a nested KWin Wayland/Plasma session.
- Plasma uses `XDG_RUNTIME_DIR=/run/user/1000/nested_plasma`, with its own session
  bus and XWayland display `:2`.
- SteamVR uses the outer `/run/user/1000` runtime and session bus.
- The nested runtime already links PipeWire and Pulse-compatible sockets to the
  outer session. A PipeWire client successfully reached the host server.
- The nested session has its own running KDE portal backend and portal service.
- Podman, `crun`, subordinate UID/GID ranges, DRM devices, and relevant user
  device groups are available. Distrobox itself was not installed.

Launch from the actual Plasma environment, not an unrelated SSH login
environment. Preserve the full user runtime tree, including targets of socket
symlinks. Session-bus socket names are dynamic and should not be hardcoded from
this observation.

## GPU and hardware video decoding

Host capabilities verified:

- Vulkan reported **Turnip / Adreno 750**, using Valve's Mesa 26.3 development
  build, with device Vulkan API 1.4 support.
- External-memory and external-semaphore FD extensions were present.
- A generated 1280x720 H.264 stream was hardware-decoded through
  `/dev/video-dec0`, using the **Qualcomm Iris V4L2 decoder**, without creating
  media files.

Relevant host devices include `/dev/dri/renderD128` and `/dev/video-dec0`
(`/dev/video22`). Sharing device nodes does not supply userspace drivers or
grant permissions beyond those available to the host user.

Video decoding uses a separate API from ordinary Vulkan rendering. The working
check used FFmpeg's `h264_v4l2m2m` decoder. It does not prove VA-API, Vulkan Video,
other codecs, or hardware acceleration in every browser/player.

Initially favor the Frame's graphics package set and compatible dependencies,
including relevant Valve layers. A separate Mesa can be explored, but must be
tested against the host kernel and VR compositor. Avoid blindly mounting the
host's entire `/usr/lib` over the guest's libraries.

## Native OpenXR/OpenVR applications and host SteamVR

The desired design is a **container application loading client-side runtime
libraries and connecting to the existing host SteamVR**, not another SteamVR
instance inside the container.

The Frame has native libraries under `/opt/steamvr/bin/linuxarm64`. A native
OpenVR background client launched with the Plasma environment detected the HMD
and initialized successfully against the running host runtime, then shut down.
It did not render or take over the scene.

Expected requirements:

- Expose the host SteamVR runtime tree at a consistent path, preferably
  `/opt/steamvr`, with compatible dependencies.
- Preserve the user's identity and host IPC/shared-memory, networking,
  process visibility, and runtime/socket access.
- Make relevant Frame Vulkan/OpenXR layers available where required.
- Configure runtime discovery, particularly with a separate container home.

Distrobox normally shares host IPC, networking, and processes. Initially avoid
`--init` and `--unshare-*`, which change that integration. Rootful recipes for
running a VR compositor in a container are not automatically applicable to this
client-only setup.

For native ARM64 OpenXR, the existing runtime manifest can be selected with:

```text
XR_RUNTIME_JSON=/opt/steamvr/steamxr_linuxarm64.json
```

The manifest loads `bin/linuxarm64/vrclient.so` relative to its directory.
Selecting a JSON file alone does not make its libraries or IPC usable.

For OpenVR, retain the host path registry or select it through
`VR_PATHREG_OVERRIDE`. `VR_OVERRIDE` can select the runtime directory. The host
registry currently points to `/opt/steamvr` and host configuration/log paths.
Those external paths matter for an encrypted-home profile.

**Not verified:** container-side OpenXR initialization, OpenXR/OpenVR rendering,
frame submission, input handling, or any complete VR application inside a box.
These must be established before considering the mandatory requirements met.

## Shared home and encrypted home

Distrobox Assemble can declare multiple profiles. The default home shares the
desktop user's home; a second profile can use a custom `home`/`--home` path.
Distrobox itself does not encrypt that directory.

Options:

- **gocryptfs:** unlock an encrypted backing directory into a host mountpoint and
  use that mountpoint as the box's home. The Frame has `/dev/fuse` and
  `fusermount3`; gocryptfs is available in public Holo and drzee, but absent from
  the checked Frame repositories.
- **LUKS-backed filesystem:** an alternative with a conventional filesystem
  inside the encrypted block device/image; host setup and unlock integration
  would need implementation.

Unlock and mount before creating/starting the box. Refuse startup if the mount
is absent, so files are not written into an unencrypted mountpoint directory.
Stop the box and its applications before unmounting: bind mounts can otherwise
retain access to the decrypted filesystem.

Important boundaries:

- `--home` does **not** prevent Distrobox from mounting the original host home.
- Distrobox is an integration tool, not a strong sandbox.
- Encryption protects the chosen backing data at rest, not against applications
  running while it is unlocked.
- Host SteamVR logs, desktop recents, temporary files, container-root writes,
  swap, and crash dumps may remain outside the encrypted home.
- Encrypting only home does not imply a confidential complete session.

## Optional FEX integration

The host's `/usr/bin/FEXBash` is a Valve wrapper using the installed FEX
compatibility tool and Steam Linux Runtime. It is not a globally registered
binfmt handler for arbitrary x86 execution; no such handler was registered at
the time of inspection.

FEX supports running x86 Linux applications on ARM64 and forwarding graphics
calls to native libraries. Reusing the Frame's launch chain from a container
would require additional integration and was not tested. Native ARM64
applications do not need it. Lepton was not investigated further.

## Repository trust and supply-chain considerations

### drzee

Positive evidence:

- Arch Ports explicitly describes its coverage as broad and well maintained,
  while clearly labeling the packages unofficial.
- The maintainer has longstanding public account provenance.
- Builder code, packaging sources/forks, and build histories/logs are public.
- Sampled current packages have detached cryptographic signatures.

Trust limits:

- Builds are not performed on Arch infrastructure or signed by Arch staff.
- Public recipes and operator-generated logs do not prove that published
  binaries were produced by that exact process.
- The build host runs its own unofficial packages, and the tooling also trusts
  selected recipes/forks. This creates a shared build/toolchain trust boundary.
- Private-key custody, independent signing isolation, and independently
  reproducible source-to-binary results were not established.
- A key downloaded from the repository itself is not independent authentication
  of that key's owner.
- A valid signature cannot rule out a malicious signer or compromised
  build/signing infrastructure.

Concrete checks on 2026-09-30:

| Evidence | Result |
|---|---|
| Embedded `PGPSIG` entries in drzee `core.db` / `extra.db` | None; this does not imply unsigned package artifacts |
| Detached signatures for sampled `attr` and `ld-lsb` ARM64 packages | Both reachable |
| `attr` checksum against repository metadata | Matched |
| `attr` detached signature against the published drzee key | Valid (`gpgv` exit 0) |
| Conventional database signature paths | HTTP 403; absence was not conclusively established |

Published signing-key fingerprint:

```text
9B2C 213B 2188 3BB6 5CE2 FB90 0CF2 5682 E6BA 0751
```

This records the observed key; it is not an independent identity verification.
The sampled signature dated from 2026-09-17. No packages were executed, installed,
or imported into a permanent keyring for this check.

Assessment: a credible community experimental repository, not an official trust
equivalent. No numerical probability of malicious packages can be justified
from this evidence. No confirmed vulnerabilities or malicious packages were
identified in the bounded provenance/signing review; it was not a binary audit.

### Frame and public Holo

The Frame's effective policy is `PackageOptional` and `DatabaseOptional`, with
trusted-signature restrictions when signatures are present. The two checked
browser packages had no embedded signatures and their conventional detached
`.sig` paths returned HTTP 404. Public Holo uses `SigLevel=Optional`.

Optional permits unsigned packages; it does not disable verification of
signatures that are present. Institutional Valve/Collabora provenance is a
different trust property from mandatory cryptographic package authentication.
A frozen preview also need not be security-current.

Package-source trust matters especially with Distrobox: a compromised application
can access shared host files and an unlocked encrypted home. Rootless execution
does not turn this integrated setup into a protective sandbox.

## Remaining work before implementation can be called usable

1. Choose one coherent userland/repository family and establish its ongoing
   update and signing policy.
2. Create a rootless box and verify application launch in the existing nested
   Plasma session, PipeWire/Pulse connectivity, and required portal operations.
3. Verify real GPU rendering and actual hardware decoding inside that box,
   including required application/codec combinations.
4. Run native OpenXR and OpenVR rendering applications against the host SteamVR;
   verify frame submission, tracking/input, and relevant Valve layers.
5. Verify USB permissions and host service control without competing daemons.
6. Implement both home profiles and fail-closed encrypted-mount lifecycle.

These are future acceptance criteria, not completed work.

## Public references

- [Distrobox integration and security model](https://distrobox.it/)
- [Distrobox create, sharing flags, custom home](https://distrobox.it/usage/distrobox-create/)
- [Distrobox Assemble profiles](https://distrobox.it/usage/distrobox-assemble/)
- [Distrobox application export](https://distrobox.it/usage/distrobox-export/)
- [Execute commands on the host](https://distrobox.it/posts/execute_commands_on_host/)
- [Steam Deck guide, requiring adaptation for Frame](https://distrobox.it/posts/steamdeck_guide/)
- [Distrobox stable creation implementation](https://github.com/89luca89/distrobox/blob/1.8.2.5/distrobox-create)
- [Distrobox stable host/socket integration](https://github.com/89luca89/distrobox/blob/1.8.2.5/distrobox-init)
- [Collabora: building the Holo Core AArch64 port and Distrobox example](https://www.collabora.com/news-and-blog/news-and-events/building-an-arch-linux-aarch64-port-for-holo-core.html)
- [Public Holo source repository and preview disclaimer](https://gitlab.steamos.cloud/holo/holo-core-aarch64-preview)
- [Holo seed selection](https://gitlab.steamos.cloud/holo/holo-core-aarch64-preview/-/raw/main/spec.yaml)
- [Public Holo snapshot](https://holo-packages.steamos.cloud/holo-core-aarch64-preview/mash-20251118.3/)
- [Public Holo pacman configuration](https://holo-packages.steamos.cloud/holo-core-aarch64-preview/mash-20251118.3/pacman.conf)
- [Public Holo mirrorlist](https://holo-packages.steamos.cloud/holo-core-aarch64-preview/mash-20251118.3/pacman.mirrorlist)
- [Upstream Arch archive used for package-count comparison](https://archive.archlinux.org/repos/2025/11/18/)
- [Arch Linux Ports status](https://ports.archlinux.page/)
- [Arch Ports AArch64 status, drzee repositories and CPU requirements](https://ports.archlinux.page/aarch64/)
- [drzee build dashboard](https://arch-linux-repo.drzee.net/arch/reports/latest.html)
- [drzee builder](https://github.com/solskogen/archlinux-aarch64-builder)
- [drzee bootstrap root filesystems](https://arch-linux-repo.drzee.net/arch/tarballs/os/aarch64/)
- [drzee published signing key](https://arch-linux-repo.drzee.net/arch/extra/os/aarch64/public.key)
- [Arch Linux ARM](https://archlinuxarm.org/)
- [Community Arch Linux ARM OCI image](https://github.com/Menci/docker-archlinuxarm)
- [Docker Official Arch image architecture](https://github.com/docker-library/docs/tree/master/archlinux)
- [OpenXR runtime discovery and `XR_RUNTIME_JSON`](https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/main/specification/loader/runtime.adoc)
- [Valve OpenVR runtime/path overrides](https://github.com/ValveSoftware/openvr/blob/master/src/vrcore/vrpathregistry_public.cpp)
- [gocryptfs quickstart](https://nuetzlich.net/gocryptfs/quickstart/)
- [FEX project](https://github.com/FEX-Emu/FEX)
