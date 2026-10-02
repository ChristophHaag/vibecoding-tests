#!/usr/bin/env python3
"""Named, self-contained Arch ARM environments on Steam Frame."""

import argparse
import contextlib
import fcntl
import getpass
import hashlib
import json
import os
from pathlib import Path
import platform
import pwd
import re
import select
import shlex
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import urllib.request


DRZEE = "https://arch-linux-repo.drzee.net/arch"
DRZEE_KEY = "9B2C213B21883BB65CE2FB900CF25682E6BA0751"
BOOTSTRAP = f"{DRZEE}/tarballs/os/aarch64/archlinux-bootstrap-latest-aarch64.tar.zst"
GOCRYPTFS_VERSION = "2.6.1"
GOCRYPTFS_KEY = "FFF3E01444FED7C316A3545A895F5BC123A02740"
NAME_PATTERN = re.compile(r"[a-z][a-z0-9_-]{0,47}")
DESKTOP_KEYS = (
    "DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR",
    "DBUS_SESSION_BUS_ADDRESS", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE",
    "PULSE_SERVER", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE",
)
# OpenGL through Vulkan; set for every entry (shell or command).
GRAPHICS_ENV = {"MESA_LOADER_DRIVER_OVERRIDE": "zink", "GALLIUM_DRIVER": "zink"}
SYNC_FILES = ("frame_arch.py", "FRAME_ARCH.md")
IMAGE = "localhost/frame-arch:base"
SEED_IMAGE = "localhost/frame-arch:bootstrap"
MARKER = "environment.json"

SETUP = r"""
set -eu
pacman-key --init
pacman-key --add /run/frame-arch-key.pub
pacman-key --lsign-key "$5"
cat > /etc/pacman.conf <<'CONFIG'
[options]
Architecture = aarch64
Color
CheckSpace
SigLevel = Required DatabaseOptional
LocalFileSigLevel = Required

[core]
Server = https://arch-linux-repo.drzee.net/arch/$repo/os/$arch

[extra]
Server = https://arch-linux-repo.drzee.net/arch/$repo/os/$arch
CONFIG
pacman -Syu --noconfirm --needed base sudo
groupadd --gid "$2" "$3"
useradd --uid "$1" --gid "$2" --home-dir "$4" --no-create-home --shell /bin/bash "$3"
install -d -m 0700 -o "$1" -g "$2" "$4"
printf '%s ALL=(ALL) NOPASSWD: ALL\n' "$3" > /etc/sudoers.d/frame-arch
chmod 0440 /etc/sudoers.d/frame-arch
pacman -Scc --noconfirm
"""


class Error(Exception):
    pass


def run(command, *, env=None, log=None, **kwargs):
    if log is None:
        return subprocess.run(command, env=env, check=True, **kwargs)
    with log.open("ab") as output:
        start = output.tell()
        try:
            result = subprocess.run(
                command, env=env, stdout=output, stderr=subprocess.STDOUT,
                check=True, **kwargs,
            )
        except subprocess.CalledProcessError:
            print(log.read_text(errors="replace")[-12000:], file=sys.stderr)
            raise
    with log.open("rb") as output:
        output.seek(start)
        for line in output.read().decode(errors="replace").splitlines():
            if line.startswith("error:"):
                print(line, file=sys.stderr)
    return result


def download(url, destination):
    print(f"Downloading {destination.name}", flush=True)
    temporary = destination.with_name(destination.name + ".part")
    with urllib.request.urlopen(url, timeout=60) as response, temporary.open("wb") as output:
        shutil.copyfileobj(response, output)
    temporary.replace(destination)


def verify_download(directory, key_url, fingerprint, url, filename, signature_suffix):
    keys = directory / "keys"
    keys.mkdir(mode=0o700, exist_ok=True)
    key = keys / (fingerprint + ".pub")
    download(key_url, key)
    result = run([
        "gpg", "--no-options", "--homedir", str(keys), "--batch",
        "--with-colons", "--show-keys", str(key),
    ], capture_output=True, text=True)
    records = [line.split(":") for line in result.stdout.splitlines()]
    primary_keys = [r for r in records if r[0] == "pub"]
    fingerprints = [r[9] for r in records if r[0] == "fpr"]
    if len(primary_keys) != 1 or not fingerprints or fingerprints[0] != fingerprint:
        raise Error(f"Unexpected signing key for {filename}; refusing to continue")
    keyring = keys / (fingerprint + ".gpg")
    run([
        "gpg", "--no-options", "--homedir", str(keys), "--batch", "--yes",
        "--dearmor", "--output", str(keyring), str(key),
    ], capture_output=True)
    artifact = directory / filename
    signature = directory / (filename + signature_suffix)
    download(url, artifact)
    download(url + signature_suffix, signature)
    run([
        "gpgv", "--homedir", str(keys), "--keyring", str(keyring),
        str(signature), str(artifact),
    ])
    return artifact, key


def desktop_environment():
    current = {key: os.environ[key] for key in DESKTOP_KEYS if key in os.environ}
    if current.get("WAYLAND_DISPLAY") or current.get("DISPLAY"):
        return current
    matches = []
    for process in Path("/proc").iterdir():
        if not process.name.isdecimal():
            continue
        try:
            if process.stat().st_uid != os.getuid():
                continue
            if (process / "comm").read_text().strip() != "plasmashell":
                continue
            values = {}
            for record in (process / "environ").read_bytes().split(b"\0"):
                if b"=" in record:
                    key, value = record.split(b"=", 1)
                    if key.decode() in DESKTOP_KEYS:
                        values[key.decode()] = value.decode()
            matches.append(values)
        except (FileNotFoundError, ProcessLookupError):
            continue
    if len(matches) > 1:
        raise Error("Multiple Plasma sessions found; run from the intended desktop terminal")
    if matches:
        return matches[0]
    print("No Plasma session found; entering with CLI access only", file=sys.stderr)
    return current


def password_from_user(from_stdin, *, confirm=False):
    if from_stdin:
        value = sys.stdin.readline()
        if not value:
            raise Error("No passphrase received on stdin")
        value = value.rstrip("\n")
    else:
        if not sys.stdin.isatty():
            raise Error("Passphrase needs a terminal, or --passphrase-stdin")
        value = getpass.getpass("Encrypted home passphrase: ")
        if confirm and value != getpass.getpass("Repeat passphrase: "):
            raise Error("Passphrases do not match")
    if not value or "\n" in value or "\r" in value:
        raise Error("Passphrase must be one nonempty line")
    return (value + "\n").encode()


class Environment:
    def __init__(self, root, name):
        if not NAME_PATTERN.fullmatch(name):
            raise Error("Name must start with a lowercase letter and contain only a-z, 0-9, _ or -")
        self.root = Path(root).expanduser().resolve()
        self.directory = self.root / name
        self.name = name
        self.config = None
        if any(c in str(self.directory) for c in ":\n\r,"):
            raise Error("Environment paths cannot contain colons, commas or newlines")
        if self.directory.is_symlink():
            raise Error("Environment directory must not be a symlink")
        protected = (Path.home().resolve(), Path.cwd().resolve())
        if any(self.directory == p or self.directory in p.parents for p in protected):
            raise Error("Environment directory cannot be home, cwd, or an ancestor of either")
        if self.directory.exists() and self.directory.stat().st_uid != os.getuid():
            raise Error("Environment directory is not owned by the current user")

    def load(self):
        marker = self.directory / MARKER
        if marker.is_symlink() or not marker.is_file():
            raise Error(f"No managed environment at {self.directory}")
        self.config = json.loads(marker.read_text())
        if (
            not isinstance(self.config, dict)
            or self.config.get("format") != 1
            or self.config.get("name") != self.name
            or self.config.get("uid") != os.getuid()
            or self.config.get("home_mode") not in ("shared", "private", "encrypted")
            or self.config.get("storage_driver") not in ("overlay", "vfs")
            or self.config.get("cgroup_manager", "cgroupfs") not in ("systemd", "cgroupfs")
            or self.config.get("phase") not in ("creating", "ready")
            or not isinstance(self.config.get("host_mount", False), bool)
            or not isinstance(self.config.get("gid"), int)
            or not isinstance(self.config.get("user"), str)
            or not isinstance(self.config.get("host_home"), str)
        ):
            raise Error("Invalid environment metadata; refusing to operate")
        return self

    def save(self):
        temporary = self.directory / (MARKER + ".tmp")
        temporary.write_text(json.dumps(self.config, indent=2) + "\n")
        temporary.replace(self.directory / MARKER)

    @contextlib.contextmanager
    def lock(self):
        with (self.directory / "lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            yield

    def engine_environment(self):
        environment = os.environ.copy()
        for key in (
            "CONTAINER_HOST", "CONTAINER_CONNECTION", "CONTAINERS_STORAGE_CONF",
            "CONTAINERS_CONF_OVERRIDE", "PODMAN_CONNECTIONS_CONF", "STORAGE_OPTS",
        ):
            environment.pop(key, None)
        environment.update({
            "XDG_RUNTIME_DIR": str(self.directory / "run"),
            "TMPDIR": str(self.directory / "run" / "tmp"),
            "XDG_CONFIG_HOME": str(self.directory / "config"),
            "XDG_DATA_HOME": str(self.directory / "data"),
            "XDG_CACHE_HOME": str(self.directory / "cache"),
            "XDG_STATE_HOME": str(self.directory / "state"),
            "CONTAINERS_CONF": str(self.directory / "containers.conf"),
            "REGISTRY_AUTH_FILE": str(self.directory / "auth.json"),
            "DBUS_SESSION_BUS_ADDRESS": f"unix:path=/run/user/{os.getuid()}/bus",
        })
        runtime = self.directory / "run"
        if runtime.is_symlink():
            raise Error("Runtime directory must not be a symlink")
        boot_id = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        stamp = self.directory / "boot-id"
        with (self.directory / "runtime-lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            previous = stamp.read_text().strip() if stamp.exists() else None
            if previous is not None and previous != boot_id and runtime.exists():
                stale = Path(tempfile.mkdtemp(prefix="run-stale-", dir=self.directory))
                runtime.replace(stale)
            runtime.mkdir(mode=0o700, exist_ok=True)
            (runtime / "tmp").mkdir(mode=0o700, exist_ok=True)
            if previous != boot_id:
                temporary = self.directory / "boot-id.tmp"
                temporary.write_text(boot_id + "\n")
                temporary.replace(stamp)
            for stale in sorted(self.directory.glob("run-stale-*")):
                if stale.is_symlink() or not stale.is_dir() or stale.stat().st_uid != os.getuid():
                    raise Error(f"Unexpected stale runtime directory: {stale}")
                # crun leaves directories owned by subordinate UIDs across reboots.
                run(
                    self.podman_command("unshare", "rm", "-rf", "--", stale),
                    env=environment,
                )
        return environment

    def podman_command(self, *arguments):
        return [
            "podman", "--root", str(self.directory / "storage"),
            "--runroot", str(self.directory / "run" / "storage"),
            "--tmpdir", str(self.directory / "run" / "libpod"),
            "--storage-driver", self.config["storage_driver"],
            "--cgroup-manager", self.config.get("cgroup_manager", "cgroupfs"),
            "--events-backend", "file",
            "--runtime", "crun", *map(str, arguments),
        ]

    def podman(self, *arguments, **kwargs):
        return run(self.podman_command(*arguments), env=self.engine_environment(), **kwargs)

    def container_exists(self, name=None):
        result = subprocess.run(
            self.podman_command("container", "exists", name or self.name),
            env=self.engine_environment(), check=False,
        )
        if result.returncode not in (0, 1):
            raise Error("Could not inspect this environment's container")
        return result.returncode == 0

    def home_source(self):
        if self.config["home_mode"] == "shared":
            return Path(self.config["host_home"])
        return self.directory / "home"

    def encryption_command(self, *arguments, password):
        self.podman(
            "unshare", self.directory / "bin" / "gocryptfs", "-q", "-nosyslog",
            "-passfile", "/dev/stdin", *map(str, arguments),
            input=password, log=self.directory / "encryption.log",
        )

    def encrypted_mount(self):
        result = subprocess.run(
            self.podman_command("unshare", "findmnt", "--json", "--mountpoint", self.directory / "home"),
            env=self.engine_environment(), capture_output=True, text=True,
        )
        if result.returncode == 1:
            return None
        if result.returncode:
            raise Error(f"Could not inspect encrypted home: {result.stderr.strip()}")
        return json.loads(result.stdout)["filesystems"][0]

    def unlock(self, from_stdin):
        if self.config["home_mode"] != "encrypted":
            return
        home = self.directory / "home"
        existing = self.encrypted_mount()
        if existing:
            if (
                existing["fstype"] != "fuse.gocryptfs"
                or existing["source"] != str(self.directory / "cipher")
            ):
                raise Error("Unexpected filesystem mounted on encrypted home")
            return
        if any(home.iterdir()):
            raise Error("Encrypted home's unmounted directory is not empty")
        self.encryption_command(
            "-allow_other", "-acl", self.directory / "cipher", home,
            password=password_from_user(from_stdin),
        )
        mounted = self.encrypted_mount()
        if (
            not mounted or mounted["fstype"] != "fuse.gocryptfs"
            or mounted["source"] != str(self.directory / "cipher")
        ):
            raise Error("Encrypted home did not mount; container will not start")

    def stop_container(self, *, info=None, log=None):
        if info is None:
            info = json.loads(self.podman("inspect", self.name, capture_output=True, text=True).stdout)[0]
        sessions = info.get("ExecIDs") or []
        if sessions:
            print(f"Stopping {len(sessions)} active exec session(s) in {self.name}", file=sys.stderr)
            if not re.fullmatch(r"[a-f0-9]{64}", info["Id"]):
                raise Error("Invalid container identifier; refusing to stop its sessions")
            for session in sessions:
                if not re.fullmatch(r"[a-f0-9]{64}", session):
                    raise Error("Invalid exec-session identifier; refusing to stop it")
                pidfile = self.directory / "storage/overlay-containers" / info["Id"] / "userdata" / session / "exec_pid"
                try:
                    recorded_pid = pidfile.read_text().strip()
                except FileNotFoundError:
                    recorded_pid = None
                if recorded_pid is not None:
                    pid = int(recorded_pid)
                    if pid <= 1 or pid == info["State"]["Pid"]:
                        raise Error("Invalid exec-session PID; refusing to signal it")
                    try:
                        descriptor = os.pidfd_open(pid)
                    except ProcessLookupError:
                        descriptor = None
                    if descriptor is not None:
                        try:
                            if not select.select([descriptor], [], [], 0)[0]:
                                try:
                                    group = (Path("/proc") / str(pid) / "cgroup").read_text()
                                except FileNotFoundError:
                                    if not select.select([descriptor], [], [], 0)[0]:
                                        raise Error("Could not verify the exec-session cgroup")
                                    group = None
                                expected = info["State"]["CgroupPath"]
                                if group is not None and (not expected or not any(
                                    line.split(":", 2)[2] == expected
                                    or line.split(":", 2)[2].startswith(expected + "/")
                                    for line in group.splitlines()
                                )):
                                    raise Error("Exec-session PID is outside this container's cgroup; refusing to signal it")
                                # Podman 5.5 mistakenly repeats SIGTERM instead of escalating to SIGKILL.
                                try:
                                    if group is not None:
                                        signal.pidfd_send_signal(descriptor, signal.SIGTERM)
                                        if not select.select([descriptor], [], [], 10)[0]:
                                            signal.pidfd_send_signal(descriptor, signal.SIGKILL)
                                            if not select.select([descriptor], [], [], 10)[0]:
                                                raise Error("Exec session did not stop after SIGKILL")
                                except ProcessLookupError:
                                    pass
                        finally:
                            os.close(descriptor)
                self.podman("container", "cleanup", "--exec", session, "--rm", self.name, log=log)
        self.podman("stop", "--time", "10", self.name, log=log)

    def stop(self):
        if self.container_exists():
            self.stop_container()
        if self.config["home_mode"] == "encrypted":
            mounted = self.encrypted_mount()
            if mounted:
                if mounted["fstype"] != "fuse.gocryptfs" or mounted["source"] != str(self.directory / "cipher"):
                    raise Error("Refusing to unmount an unexpected filesystem")
                self.podman("unshare", "fusermount3", "-u", self.directory / "home")

    def remove(self):
        self.stop()
        if (self.directory / "bootstrap").exists():
            self.podman("unshare", "rm", "-rf", "--", self.directory / "bootstrap")
        self.podman("system", "reset", "--force")
        mounts = run(["findmnt", "--json", "--list"], capture_output=True, text=True)
        for filesystem in json.loads(mounts.stdout)["filesystems"]:
            target = Path(filesystem["target"])
            if target == self.directory or self.directory in target.parents:
                raise Error(f"Still mounted at {target}; refusing to delete")
        shutil.rmtree(self.directory)

    def create(self, mode, driver, from_stdin):
        if self.directory.exists() and set(p.name for p in self.directory.iterdir()) - {"frame_arch.py"}:
            raise Error(f"Directory already contains state: {self.directory}; use remove before recreating")
        self.directory.mkdir(parents=True, mode=0o700, exist_ok=True)
        self.directory.chmod(0o700)
        account = pwd.getpwuid(os.getuid())
        self.config = {
            "format": 1, "name": self.name, "uid": os.getuid(),
            "gid": os.getgid(), "user": account.pw_name,
            "host_home": str(Path.home()), "home_mode": mode,
            "host_mount": True,
            "storage_driver": driver, "cgroup_manager": "systemd", "phase": "creating",
        }
        self.save()
        source = Path(__file__).resolve()
        if source != self.directory / "frame_arch.py":
            shutil.copyfile(source, self.directory / "frame_arch.py")
        for child in ("run", "config", "data", "cache", "bin", "downloads", "bootstrap"):
            (self.directory / child).mkdir(mode=0o700)
        (self.directory / "containers.conf").write_text(
            '[engine]\ncgroup_manager = "systemd"\nevents_logger = "file"\nlock_type = "file"\n'
            f'image_copy_tmp_dir = {json.dumps(str(self.directory / "run" / "tmp"))}\n'
        )
        with self.lock():
            downloads = self.directory / "downloads"
            archive, key = verify_download(
                downloads, f"{DRZEE}/extra/os/aarch64/public.key",
                DRZEE_KEY, BOOTSTRAP, "bootstrap.tar.zst", ".sig",
            )
            print("Importing verified ARM64 bootstrap", flush=True)
            work = self.directory / "bootstrap"
            self.podman(
                "unshare", "tar", "--zstd", "-xf", archive, "-C", work,
                log=self.directory / "create.log",
            )
            rootfs = work / "root.aarch64"
            if not (rootfs / "usr" / "bin" / "pacman").is_file():
                raise Error("Unexpected bootstrap layout")
            producer = subprocess.Popen(
                self.podman_command("unshare", "tar", "-C", rootfs, "-cf", "-", "."),
                env=self.engine_environment(), stdout=subprocess.PIPE,
            )
            try:
                self.podman(
                    "import", "--arch", "arm64", "-", SEED_IMAGE,
                    stdin=producer.stdout, log=self.directory / "create.log",
                )
            finally:
                producer.stdout.close()
                result = producer.wait()
            if result:
                raise Error("Failed to repack bootstrap")
            self.podman("unshare", "rm", "-rf", "--", work)
            print("Installing minimal base and configuring the user (create.log has details)", flush=True)
            self.podman(
                "run", "--name", "setup", "--network", "host", "--cgroups", "disabled",
                "--log-driver", "k8s-file",
                "--volume", f"{key}:/run/frame-arch-key.pub:ro", SEED_IMAGE,
                "/bin/bash", "-c", SETUP, "setup", self.config["uid"],
                self.config["gid"], self.config["user"], self.config["host_home"], DRZEE_KEY,
                log=self.directory / "create.log",
            )
            self.podman("commit", "setup", IMAGE, log=self.directory / "create.log")
            self.podman("rm", "setup", log=self.directory / "create.log")
            self.podman("rmi", SEED_IMAGE, log=self.directory / "create.log")
            if mode != "shared":
                (self.directory / "home").mkdir(mode=0o700)
            if mode == "encrypted":
                archive_name = f"gocryptfs_v{GOCRYPTFS_VERSION}_linux-static_arm64.tar.gz"
                release, _ = verify_download(
                    downloads, "https://nuetzlich.net/gocryptfs-signing-key.pub",
                    GOCRYPTFS_KEY,
                    f"https://github.com/rfjakob/gocryptfs/releases/download/v{GOCRYPTFS_VERSION}/{archive_name}",
                    archive_name, ".asc",
                )
                with tarfile.open(release, "r:gz") as archive_file:
                    binaries = [m for m in archive_file if m.isfile() and Path(m.name).name == "gocryptfs"]
                    if len(binaries) != 1:
                        raise Error("Unexpected gocryptfs release layout")
                    with archive_file.extractfile(binaries[0]) as binary:
                        (self.directory / "bin" / "gocryptfs").write_bytes(binary.read())
                (self.directory / "bin" / "gocryptfs").chmod(0o700)
                (self.directory / "cipher").mkdir(mode=0o700)
                self.encryption_command(
                    "-init", self.directory / "cipher",
                    password=password_from_user(from_stdin, confirm=True),
                )
            self.create_container()
            self.config["phase"] = "ready"
            self.save()
        print(f"Created {self.name} ({mode} home) at {self.directory}", flush=True)

    def create_container(self):
        home = self.config["host_home"]
        arguments = [
            "create", "--name", self.name, "--hostname", self.name,
            "--log-driver", "k8s-file",
            "--userns", "keep-id", "--user", f'{self.config["uid"]}:{self.config["gid"]}',
            "--group-add", "keep-groups", "--privileged", "--cgroups", "no-conmon",
            "--network", "host", "--ipc", "host", "--pid", "host",
            "--security-opt", "label=disable", "--workdir", home,
            "--env", f"HOME={home}", "--env", f'USER={self.config["user"]}',
            "--env", f'LOGNAME={self.config["user"]}', "--env", "LANG=C.UTF-8",
            "--volume", f"{self.home_source()}:{home}:rw",
        ]
        if self.config.get("host_mount", False):
            arguments += ["--volume", "/:/run/host:rw,rslave"]
        mounts = (
            (Path("/dev/dri"), "/dev/dri", "rw,rslave"),
            (Path("/dev/bus/usb"), "/dev/bus/usb", "rw,rslave"),
            (Path("/dev/input"), "/dev/input", "rw,rslave"),
            (Path("/dev/snd"), "/dev/snd", "rw,rslave"),
            (Path("/dev/video-dec0"), "/dev/video-dec0", "rw"),
            (Path("/dev/video-enc0"), "/dev/video-enc0", "rw"),
            (Path("/sys"), "/sys", "ro,rslave"),
            (Path("/tmp"), "/tmp", "rw,rslave"),
            (Path(f'/run/user/{self.config["uid"]}'), f'/run/user/{self.config["uid"]}', "rw,rslave"),
            (Path("/run/dbus/system_bus_socket"), "/run/dbus/system_bus_socket", "rw"),
            (Path("/opt/steamvr"), "/opt/steamvr", "ro"),
        )
        for source, target, options in mounts:
            if source.exists():
                arguments += ["--volume", f"{source}:{target}:{options}"]
        if Path("/opt/steamvr").is_dir():
            arguments += ["--env", "VR_OVERRIDE=/opt/steamvr"]
        registry = Path(home) / ".config/openvr/openvrpaths.vrpath"
        if registry.is_file():
            arguments += [
                "--volume", f"{registry}:/run/frame-openvr/openvrpaths.vrpath:ro",
                "--env", "VR_PATHREG_OVERRIDE=/run/frame-openvr/openvrpaths.vrpath",
            ]
        if Path("/opt/steamvr/steamxr_linuxarm64.json").is_file():
            arguments += ["--env", "XR_RUNTIME_JSON=/opt/steamvr/steamxr_linuxarm64.json"]
        self.podman(*arguments, IMAGE, "/usr/bin/sleep", "infinity", log=self.directory / "create.log")

    def mount_host(self):
        if self.config["phase"] != "ready":
            raise Error("Creation must finish before adding host filesystem access")
        info = json.loads(self.podman("inspect", self.name, capture_output=True, text=True).stdout)[0]
        if any(
            mount["Source"] == "/" and mount["Destination"] == "/run/host" and mount["RW"]
            for mount in info["Mounts"]
        ):
            self.config["host_mount"] = True
            self.save()
            print(f"{self.name} already exposes the host filesystem at /run/host")
            return
        backup = self.name + "-host-mount-backup"
        if self.container_exists(backup):
            raise Error(f"Migration backup {backup} already exists; resolve it before retrying, without deleting the environment")
        original = self.config.copy()
        running = info["State"]["Running"]
        if running and self.config["home_mode"] == "encrypted":
            mounted = self.encrypted_mount()
            if (
                not mounted or mounted["fstype"] != "fuse.gocryptfs"
                or mounted["source"] != str(self.directory / "cipher")
            ):
                raise Error("Running encrypted environment does not have its expected unlocked home; refusing migration")
        renamed = False
        log = self.directory / "host-mount.log"
        try:
            if running:
                self.stop_container(info=info, log=log)
            self.podman("commit", self.name, IMAGE, log=log)
            self.podman("rename", self.name, backup, log=log)
            renamed = True
            self.config["host_mount"] = True
            self.create_container()
            if running:
                self.podman("start", self.name, log=log)
                self.podman("exec", self.name, "/usr/bin/test", "-d", "/run/host" + self.config["host_home"], log=log)
            self.save()
        except (Error, OSError, subprocess.CalledProcessError, KeyboardInterrupt) as error:
            print(f"Host-mount migration failed: {error}; restoring the previous container", file=sys.stderr)
            self.config = original
            if renamed:
                if self.container_exists():
                    self.podman("rm", "--force", self.name, log=log)
                self.podman("rename", backup, self.name, log=log)
            if running:
                self.podman("start", self.name, log=log)
            self.save()
            raise
        self.podman("rm", backup, log=log)
        print(f"{self.name} now exposes the host filesystem read-write at /run/host")

    def login_shell(self):
        result = self.podman(
            "exec", self.name, "/usr/bin/getent", "passwd", str(self.config["uid"]),
            capture_output=True, text=True,
        )
        records = result.stdout.splitlines()
        fields = records[0].split(":") if len(records) == 1 else []
        if len(fields) != 7 or fields[2] != str(self.config["uid"]):
            raise Error("Could not identify the container user's login shell")
        shell = fields[6] or "/bin/sh"
        if not Path(shell).is_absolute():
            raise Error("The container user's login shell must be an absolute path")
        return shell

    def enter(self, command, from_stdin):
        if self.config["phase"] != "ready":
            raise Error("Creation did not finish; remove this environment and recreate it")
        desktop = desktop_environment()
        with self.lock():
            self.unlock(from_stdin)
            self.podman("start", self.name, log=self.directory / "create.log")
        arguments = ["exec", "--interactive"]
        if sys.stdin.isatty() and sys.stdout.isatty():
            arguments += ["--tty"]
        for key, value in {**desktop, **GRAPHICS_ENV}.items():
            arguments += ["--env", f"{key}={value}"]
        if not command:
            shell = self.login_shell()
            arguments += ["--env", f"SHELL={shell}"]
            command = [shell, "-l"]
        arguments += [self.name, *command]
        result = subprocess.run(self.podman_command(*arguments), env=self.engine_environment())
        return result.returncode


def list_environments(root):
    parent = Path(root)
    if not parent.is_dir():
        return []
    return sorted(
        entry.name for entry in parent.iterdir()
        if NAME_PATTERN.fullmatch(entry.name) and (entry / MARKER).is_file()
    )


def ssh_options(args):
    options = ["ssh", "-x"]
    if args.identity:
        options += ["-i", str(Path(args.identity).expanduser())]
    for option in args.ssh_option:
        options += ["-o", option]
    if args.ssh.startswith("-"):
        raise Error("Invalid SSH destination")
    return options


def remote_root(args):
    if args.root is None:
        return '"$HOME"/.local/share/frame-arch'
    if args.root.startswith("~/"):
        return '"$HOME"/' + shlex.quote(args.root[2:].rstrip("/"))
    if args.root.startswith("/"):
        return shlex.quote(args.root.rstrip("/"))
    raise Error("SSH --root must be absolute or start with ~/")


def remote_list(args):
    script = (
        f'for d in {remote_root(args)}/*/; do '
        '[ -f "${d}environment.json" ] && basename "$d"; done | LC_ALL=C sort'
    )
    result = run([*ssh_options(args), args.ssh, script], capture_output=True, text=True)
    return [name for name in result.stdout.split() if NAME_PATTERN.fullmatch(name)]


def remote_sync(args):
    options = ssh_options(args)
    here = Path(__file__).resolve().parent
    names = " ".join(SYNC_FILES)
    if args.mode == "push":
        archive = subprocess.run(["tar", "-C", str(here), "-cf", "-", *SYNC_FILES], check=True, capture_output=True).stdout
        run([*options, args.ssh, 'umask 077; tar -C "$HOME" -xf -'], input=archive)
        print(f"pushed: {names}")
    elif args.mode == "pull":
        archive = run([*options, args.ssh, f'cd "$HOME" && tar -cf - {names}'], capture_output=True).stdout
        subprocess.run(["tar", "-C", str(here), "-xf", "-"], input=archive, check=True)
        print(f"pulled: {names}")
    else:
        sums = run([*options, args.ssh, f'cd "$HOME" && sha256sum {names} 2>/dev/null || true'], capture_output=True, text=True).stdout
        remote_sums = {line.split()[1]: line.split()[0] for line in sums.splitlines() if len(line.split()) == 2}
        for name in SYNC_FILES:
            local = hashlib.sha256((here / name).read_bytes()).hexdigest() if (here / name).is_file() else None
            if name not in remote_sums:
                state = "remote-missing"
            elif local is None:
                state = "local-missing"
            else:
                state = "same" if local == remote_sums[name] else "DIFFERENT"
            print(f"{state:15} {name}")
    return 0


def remote(args):
    options = ssh_options(args)
    if args.action == "list":
        print("\n".join(remote_list(args)))
        return 0
    if args.action == "sync":
        return remote_sync(args)
    if args.action == "enter" and args.name is None:
        names = remote_list(args)
        if not names:
            raise Error("No environments found")
        args.name = names[0]
    directory = remote_root(args) + "/" + shlex.quote(args.name)
    if not NAME_PATTERN.fullmatch(args.name):
        raise Error("Invalid environment name")
    staging = (
        f"set -eu; umask 077; d={directory}; "
        'test ! -L "$d"; '
        + ('mkdir -p -- "$d"; ' if args.action == "create" else 'test -f "$d/environment.json"; ')
        + 't=$(mktemp "$d/.transport.XXXXXX"); trap \'rm -f -- "$t"\' EXIT; '
        'cat > "$t"; mv -f -- "$t" "$d/frame_arch.py"'
    )
    run([*options, args.ssh, staging], input=Path(__file__).read_bytes())
    forwarded = []
    if args.root is not None:
        forwarded += ["--root", args.root]
    if args.passphrase_stdin:
        forwarded += ["--passphrase-stdin"]
    forwarded += [args.action, args.name]
    if args.action == "create":
        forwarded += ["--home", args.home, "--storage-driver", args.storage_driver]
    if args.action == "enter" and args.command:
        forwarded += ["--", *args.command]
    if args.action in ("remove", "mount-host") and args.yes:
        forwarded += ["--yes"]
    invocation = f"exec python3 {directory}/frame_arch.py " + shlex.join(forwarded)
    interactive = not args.passphrase_stdin and sys.stdin.isatty() and sys.stdout.isatty()
    return subprocess.run([*options, *(["-t"] if interactive else []), args.ssh, invocation]).returncode


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ssh", help="Run on an SSH destination, e.g. steamos@frame")
    parser.add_argument("--identity", help="SSH private-key file (never copied)")
    parser.add_argument("--ssh-option", action="append", default=[], metavar="KEY=VALUE")
    parser.add_argument("--root", help="State parent; default: ~/.local/share/frame-arch on the Frame")
    parser.add_argument("--passphrase-stdin", action="store_true", help="Read one passphrase line from stdin, not a terminal")
    commands = parser.add_subparsers(dest="action", required=True)
    create = commands.add_parser("create", help="Create a minimal native Arch environment")
    create.add_argument("name")
    create.add_argument("--home", choices=("shared", "private", "encrypted"), default="shared")
    create.add_argument("--storage-driver", choices=("overlay", "vfs"), default="overlay")
    enter = commands.add_parser("enter", help="Enter a shell or run a command")
    enter.add_argument("rest", nargs=argparse.REMAINDER, metavar="[NAME] [-- COMMAND...]",
                       help="Environment name (default: first listed), then an optional command")
    commands.add_parser("list", help="List environment names, one per line")
    sync = commands.add_parser("sync", help="Compare/copy this script and its docs to/from the Frame's home (needs --ssh)")
    sync.add_argument("mode", nargs="?", choices=("status", "push", "pull"), default="status")
    commands.add_parser("stop", help="Stop the container and lock an encrypted home").add_argument("name")
    remove = commands.add_parser("remove", help="Stop and delete this environment's directory")
    remove.add_argument("name")
    remove.add_argument("--yes", action="store_true", help="Confirm deletion without a prompt")
    mount_host = commands.add_parser("mount-host", help="Add /run/host to an existing environment, preserving its system and home")
    mount_host.add_argument("name")
    mount_host.add_argument("--yes", action="store_true", help="Confirm recreation and stopping active sessions")
    args = parser.parse_args(argv)
    if args.action == "enter":
        rest = args.rest
        args.name = rest.pop(0) if rest and rest[0] != "--" else None
        if rest[:1] == ["--"]:
            rest = rest[1:]
        args.command = rest
    return args


def main(argv=None):
    args = parse_args(argv)
    os.umask(0o077)
    if args.ssh:
        return remote(args)
    if args.identity or args.ssh_option:
        raise Error("SSH options require --ssh")
    if args.action == "sync":
        raise Error("sync requires --ssh")
    if os.getuid() == 0:
        raise Error("Run as the desktop user, not root")
    if platform.machine() != "aarch64":
        raise Error("Local execution requires an ARM64 Frame; use --ssh from a PC")
    required = ["podman", "crun", "findmnt"]
    if args.action == "create":
        required += ["gpg", "gpgv", "tar", "zstd"]
        if args.home == "encrypted":
            required += ["fusermount3"]
    for tool in required:
        if not shutil.which(tool):
            raise Error(f"Missing host prerequisite: {tool}")
    root = args.root or str(Path.home() / ".local/share/frame-arch")
    if args.action == "list":
        print("\n".join(list_environments(root)))
        return 0
    if args.action == "enter" and args.name is None:
        names = list_environments(root)
        if not names:
            raise Error("No environments found")
        args.name = names[0]
    environment = Environment(root, args.name)
    if args.action == "create":
        environment.create(args.home, args.storage_driver, args.passphrase_stdin)
        return 0
    environment.load()
    if args.action == "enter":
        return environment.enter(args.command, args.passphrase_stdin)
    if args.action == "remove" and not args.yes:
        response = input(f"Delete {environment.directory} and ALL its private/encrypted data? [y/N] ")
        if response.lower() != "y":
            raise Error("Removal cancelled")
    if args.action == "mount-host" and not args.yes:
        response = input(f"Expose the host filesystem read-write in {args.name}, stopping its current shells/apps? [y/N] ")
        if response.lower() != "y":
            raise Error("Host-mount migration cancelled")
    with environment.lock():
        if args.action == "stop":
            environment.stop()
            suffix = "; encrypted home locked" if environment.config["home_mode"] == "encrypted" else ""
            print(f"Stopped {args.name}{suffix}")
        elif args.action == "mount-host":
            environment.mount_host()
        else:
            environment.remove()
            print(f"Removed {environment.directory}; shared host-home files were not deleted")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Error, OSError, ValueError, tarfile.TarError, subprocess.CalledProcessError) as error:
        print(f"frame-arch: {error}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("frame-arch: interrupted; partial state is kept for explicit removal", file=sys.stderr)
        sys.exit(130)
