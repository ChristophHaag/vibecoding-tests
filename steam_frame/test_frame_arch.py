import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import frame_arch


class RuntimeCleanupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.environment = frame_arch.Environment(self.temporary.name, "test")
        self.directory = self.environment.directory
        self.directory.mkdir()
        self.environment.config = {"storage_driver": "overlay", "cgroup_manager": "systemd"}
        self.runtime = self.directory / "run"
        self.stamp = self.directory / "boot-id"
        self.boot_id = Path("/proc/sys/kernel/random/boot_id").read_text().strip()

    def old_runtime(self):
        self.runtime.mkdir()
        mount = self.runtime / "crun" / "container" / "tmpmount"
        mount.mkdir(parents=True)
        (mount / "old-state").write_text("stale")
        self.stamp.write_text("previous-boot\n")

    def remove_in_namespace(self, command, *, env):
        self.assertEqual(command, self.environment.podman_command(
            "unshare", "rm", "-rf", "--", command[-1],
        ))
        stale = Path(command[-1])
        self.assertEqual(stale.parent, self.directory)
        self.assertTrue(stale.name.startswith("run-stale-"))
        self.assertEqual(env["XDG_RUNTIME_DIR"], str(self.runtime))
        self.assertEqual(env["TMPDIR"], str(self.runtime / "tmp"))
        self.assertTrue((self.runtime / "tmp").is_dir())
        self.assertEqual(self.stamp.read_text().strip(), self.boot_id)
        shutil.rmtree(stale)

    def test_reboot_removes_only_stale_runtime_in_namespace(self):
        self.old_runtime()
        for name in ("storage", "home", "cipher"):
            (self.directory / name).mkdir()
            (self.directory / name / "keep").write_text(name)
        with patch("frame_arch.run", side_effect=self.remove_in_namespace) as command:
            self.environment.engine_environment()
        command.assert_called_once()
        self.assertFalse((self.runtime / "crun").exists())
        self.assertEqual(list(self.directory.glob("run-stale-*")), [])
        for name in ("storage", "home", "cipher"):
            self.assertEqual((self.directory / name / "keep").read_text(), name)

    def test_failed_cleanup_is_reported_and_retried_without_resetting_runtime(self):
        self.old_runtime()
        failure = subprocess.CalledProcessError(1, ["podman", "unshare"])
        with patch("frame_arch.run", side_effect=failure):
            with self.assertRaises(subprocess.CalledProcessError):
                self.environment.engine_environment()
        stale = list(self.directory.glob("run-stale-*"))
        self.assertEqual(len(stale), 1)
        self.assertTrue((stale[0] / "crun" / "container" / "tmpmount" / "old-state").is_file())
        (self.runtime / "current-state").write_text("keep")
        with patch("frame_arch.run", side_effect=self.remove_in_namespace) as command:
            self.environment.engine_environment()
        command.assert_called_once()
        self.assertEqual((self.runtime / "current-state").read_text(), "keep")
        self.assertEqual(list(self.directory.glob("run-stale-*")), [])

    def test_second_reboot_retries_all_pending_runtime_directories(self):
        self.old_runtime()
        with patch("frame_arch.run", side_effect=subprocess.CalledProcessError(1, ["podman"])):
            with self.assertRaises(subprocess.CalledProcessError):
                self.environment.engine_environment()
        (self.runtime / "next-stale-state").write_text("stale")
        self.stamp.write_text("another-previous-boot\n")
        with patch("frame_arch.run", side_effect=self.remove_in_namespace) as command:
            self.environment.engine_environment()
        self.assertEqual(command.call_count, 2)
        self.assertEqual(list(self.directory.glob("run-stale-*")), [])
        self.assertFalse((self.runtime / "next-stale-state").exists())

    def test_current_boot_preserves_runtime_without_podman_cleanup(self):
        self.runtime.mkdir()
        (self.runtime / "current-state").write_text("keep")
        self.stamp.write_text(self.boot_id + "\n")
        with patch("frame_arch.run") as command:
            self.environment.engine_environment()
        command.assert_not_called()
        self.assertEqual((self.runtime / "current-state").read_text(), "keep")

    def test_runtime_symlink_is_rejected(self):
        self.runtime.symlink_to(self.directory / "storage", target_is_directory=True)
        with patch("frame_arch.run") as command:
            with self.assertRaisesRegex(frame_arch.Error, "Runtime directory must not be a symlink"):
                self.environment.engine_environment()
        command.assert_not_called()

    def test_stale_runtime_symlink_is_rejected(self):
        (self.directory / "run-stale-link").symlink_to(self.directory, target_is_directory=True)
        with patch("frame_arch.run") as command:
            with self.assertRaisesRegex(frame_arch.Error, "Unexpected stale runtime directory"):
                self.environment.engine_environment()
        command.assert_not_called()

    def test_engine_environment_remains_isolated(self):
        overrides = (
            "CONTAINER_HOST", "CONTAINER_CONNECTION", "CONTAINERS_STORAGE_CONF",
            "CONTAINERS_CONF_OVERRIDE", "PODMAN_CONNECTIONS_CONF", "STORAGE_OPTS",
        )
        with patch.dict(os.environ, {key: "host-override" for key in overrides}):
            environment = self.environment.engine_environment()
        for key in overrides:
            self.assertNotIn(key, environment)
        self.assertEqual(environment["CONTAINERS_CONF"], str(self.directory / "containers.conf"))


if __name__ == "__main__":
    unittest.main()
