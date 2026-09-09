"""Tests for crash-safe build-tree ownership."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from _support import bootstrap_xraytest

bootstrap_xraytest()

from xraytest import buildlock  # noqa: E402


class BuildTreeLockTest(unittest.TestCase):
    def test_canonical_sidecar_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / "nested" / ".." / "build"
            expected = Path(directory) / ".build.xray-build.lock"
            self.assertEqual(buildlock.lock_path(build), expected.resolve())
            self.assertEqual(buildlock.owner_path(build),
                             expected.with_suffix(".lock.owner").resolve())

    def test_live_owner_is_exclusive_and_release_is_reusable(self):
        with tempfile.TemporaryDirectory() as directory:
            first = buildlock.BuildTreeLock(Path(directory) / "build", timeout=0)
            second = buildlock.BuildTreeLock(Path(directory) / "build", timeout=0)
            self.assertTrue(first.acquire())
            try:
                self.assertFalse(second.acquire())
                self.assertIn(f'"pid":{os.getpid()}', second.owner_text())
            finally:
                first.release()
            self.assertTrue(second.acquire())
            second.release()

    def test_owner_record_replaces_prior_contents(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / "build"
            owner_path = buildlock.owner_path(build)
            owner_path.write_text("stale" * 2000, encoding="utf-8")
            with buildlock.BuildTreeLock(build, timeout=0) as held:
                owner = held.owner_text()
                self.assertTrue(owner.startswith("{"))
                self.assertIn(f'"pid":{os.getpid()}', owner)
                self.assertNotIn("stale", owner)
                self.assertLessEqual(owner_path.stat().st_size, 4095)

    def test_existing_unlocked_sidecar_is_not_a_stale_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / "build"
            buildlock.lock_path(build).write_bytes(b"\0")
            buildlock.owner_path(build).write_text("dead owner\n", encoding="utf-8")
            with buildlock.BuildTreeLock(build, timeout=0) as held:
                self.assertIn(f'"pid":{os.getpid()}', held.owner_text())

    def test_process_death_releases_the_kernel_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / "build"
            library = Path(__file__).resolve().parents[1]
            program = (
                "import sys\n"
                "from pathlib import Path\n"
                "sys.path.insert(0, sys.argv[1])\n"
                "from xraytest.buildlock import BuildTreeLock\n"
                "lease = BuildTreeLock(Path(sys.argv[2]), timeout=0)\n"
                "assert lease.acquire()\n"
                "print('held', flush=True)\n"
                "sys.stdin.read(1)\n"
            )
            with subprocess.Popen(
                    [sys.executable, "-c", program, str(library), str(build)],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True) as child:
                try:
                    assert child.stdout is not None
                    self.assertEqual(child.stdout.readline().strip(), "held")
                    self.assertFalse(
                        buildlock.BuildTreeLock(build, timeout=0).acquire())
                    child.terminate()
                    child.wait(timeout=5)
                    with buildlock.BuildTreeLock(build, timeout=0):
                        pass
                finally:
                    if child.poll() is None:
                        child.kill()
                        child.wait(timeout=5)

    def test_timeout_environment_is_fail_closed(self):
        for value in ("bad", "-1"):
            with self.subTest(value=value), mock.patch.dict(
                    os.environ, {"XR_BUILD_LOCK_TIMEOUT": value}):
                with self.assertRaises(ValueError):
                    buildlock.timeout_from_env()


if __name__ == "__main__":
    unittest.main()
