"""Tests for the shared sanitizer-lane plumbing.

The two load-bearing invariants, both testable without any sanitizer build:

  - a lane must refuse a build tree that does not actually have the sanitizer on
  - a lane reusing a binary must refuse one older than the sources

Either failing silently produces the worst possible outcome for these lanes: a
green result the tree never earned.
"""

import os
import tempfile
import time
import unittest
from pathlib import Path

from _support import bootstrap_xraytest

bootstrap_xraytest()
from xraytest import sanitizer  # noqa: E402


class CacheInspectionTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="xt_san.")
        self.build = Path(self.tmp)

    def tearDown(self):
        import shutil

        shutil.rmtree(self.tmp, ignore_errors=True)

    def _cache(self, text):
        (self.build / "CMakeCache.txt").write_text(text, encoding="utf-8")

    def test_generator_read_from_cache(self):
        self._cache("CMAKE_GENERATOR:INTERNAL=Ninja\n")
        self.assertEqual(sanitizer.configured_generator(self.build), "Ninja")

    def test_generator_none_when_unconfigured(self):
        self.assertIsNone(sanitizer.configured_generator(self.build))

    def test_makefiles_generator_detected(self):
        self._cache("CMAKE_GENERATOR:INTERNAL=Unix Makefiles\n")
        self.assertEqual(sanitizer.configured_generator(self.build), "Unix Makefiles")

    def test_sanitizer_on_accepted(self):
        self._cache("ENABLE_ASAN:BOOL=ON\nENABLE_UBSAN:BOOL=ON\n")
        self.assertIsNone(sanitizer.verify_configured(self.build, "ENABLE_ASAN=ON"))

    def test_sanitizer_off_rejected(self):
        # The critical one: pointing a lane at a plain build directory must be
        # an error, not a clean run.
        self._cache("ENABLE_ASAN:BOOL=OFF\n")
        problem = sanitizer.verify_configured(self.build, "ENABLE_ASAN=ON")
        self.assertIsNotNone(problem)
        self.assertIn("ENABLE_ASAN", problem)

    def test_sanitizer_absent_rejected(self):
        self._cache("CMAKE_BUILD_TYPE:STRING=Debug\n")
        self.assertIsNotNone(sanitizer.verify_configured(self.build, "ENABLE_ASAN=ON"))

    def test_unconfigured_tree_is_not_an_error(self):
        # Nothing to contradict yet; configure() will create it.
        self.assertIsNone(sanitizer.verify_configured(self.build, "ENABLE_ASAN=ON"))

    def test_tsan_flag_checked_by_name(self):
        self._cache("ENABLE_TSAN:BOOL=ON\n")
        self.assertIsNone(sanitizer.verify_configured(self.build, "ENABLE_TSAN=ON"))
        self.assertIsNotNone(sanitizer.verify_configured(self.build, "ENABLE_ASAN=ON"))

    def test_raw_flag_verified_as_substring(self):
        # The TSan lane instruments through CMAKE_C_FLAGS rather than an
        # ENABLE_* option, so there is no BOOL to read; the instrumentation
        # flag itself is what proves the tree is instrumented.
        self._cache('CMAKE_C_FLAGS:STRING=-fsanitize=thread -fno-omit-frame-pointer\n')
        self.assertIsNone(sanitizer.verify_configured(self.build, "-fsanitize=thread"))

    def test_raw_flag_absent_is_rejected(self):
        self._cache("CMAKE_C_FLAGS:STRING=-O2\n")
        problem = sanitizer.verify_configured(self.build, "-fsanitize=thread")
        self.assertIsNotNone(problem)
        self.assertIn("-fsanitize=thread", problem)

    def test_verification_targets_prefers_explicit_list(self):
        # A lane that names verify_cache_contains must be checked by that, not
        # by its configure-time flags: the two answer different questions.
        spec = sanitizer.BuildSpec(
            build_dir=self.build,
            sanitizer_flags=("CMAKE_C_FLAGS=-fsanitize=thread",),
            verify_cache_contains=("-fsanitize=thread",),
        )
        self.assertEqual(spec.verification_targets(), ("-fsanitize=thread",))

    def test_verification_targets_defaults_to_sanitizer_flags(self):
        spec = sanitizer.BuildSpec(build_dir=self.build,
                                   sanitizer_flags=("ENABLE_ASAN=ON",))
        self.assertEqual(spec.verification_targets(), ("ENABLE_ASAN=ON",))


class StaleSourceTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="xt_stale.")
        self.root = Path(self.tmp)
        (self.root / "src").mkdir()
        self.source = self.root / "src" / "a.c"
        self.source.write_text("int a;\n", encoding="utf-8")
        self.binary = self.root / "xray"
        self.binary.write_text("binary\n", encoding="utf-8")

    def tearDown(self):
        import shutil

        shutil.rmtree(self.tmp, ignore_errors=True)

    def _touch(self, path, offset):
        stamp = time.time() + offset
        os.utime(path, (stamp, stamp))

    def test_current_binary_is_not_stale(self):
        self._touch(self.source, -100)
        self._touch(self.binary, 0)
        self.assertIsNone(sanitizer.stale_source(self.binary, self.root))

    def test_newer_source_is_stale(self):
        # A binary older than a source would certify code never built under the
        # sanitizer; the lane must refuse it.
        self._touch(self.binary, -100)
        self._touch(self.source, 0)
        found = sanitizer.stale_source(self.binary, self.root)
        self.assertIsNotNone(found)
        self.assertEqual(found.name, "a.c")

    def test_missing_binary_reports_nothing(self):
        # Absence is handled by the caller's own existence check.
        self.assertIsNone(sanitizer.stale_source(self.root / "nope", self.root))

    def test_top_level_file_root_checked(self):
        cml = self.root / "CMakeLists.txt"
        cml.write_text("project(x)\n", encoding="utf-8")
        self._touch(self.binary, -100)
        self._touch(self.source, -200)
        self._touch(cml, 0)
        found = sanitizer.stale_source(self.binary, self.root)
        self.assertIsNotNone(found)
        self.assertEqual(found.name, "CMakeLists.txt")


class ReuseGuardTest(unittest.TestCase):
    """configure() may only reuse a tree whose sanitizer flags actually match.

    A Ninja tree left by an interrupted run can have the sanitizer OFF. Reusing
    it builds an uninstrumented binary and the lane reports a clean result it
    never earned -- the exact failure this caught in practice, where a
    half-built ENABLE_TSAN=OFF tree was silently reused by the TSan lane.
    """

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="xt_reuse.")
        self.build = Path(self.tmp) / "build-x"
        self.build.mkdir()
        self.messages = []

    def tearDown(self):
        import shutil

        shutil.rmtree(self.tmp, ignore_errors=True)

    def _log(self, message, *, error=False):
        self.messages.append(message)

    def _cache(self, text):
        (self.build / "CMakeCache.txt").write_text(text, encoding="utf-8")

    def test_tree_with_sanitizer_off_is_not_reused(self):
        self._cache("CMAKE_GENERATOR:INTERNAL=Ninja\nENABLE_TSAN:BOOL=OFF\n")
        spec = sanitizer.BuildSpec(build_dir=self.build,
                                   sanitizer_flags=("ENABLE_TSAN=ON",))
        # ninja may be absent in this environment; either way the tree must be
        # discarded rather than reused, which is what this asserts.
        sanitizer.configure(spec, Path(self.tmp), 1, 5, self._log)
        joined = " ".join(self.messages)
        self.assertNotIn("reusing existing configuration", joined)
        self.assertIn("configured without ENABLE_TSAN=ON", joined)

    def test_matching_tree_is_reused(self):
        self._cache("CMAKE_GENERATOR:INTERNAL=Ninja\nENABLE_TSAN:BOOL=ON\n")
        spec = sanitizer.BuildSpec(build_dir=self.build,
                                   sanitizer_flags=("ENABLE_TSAN=ON",))
        self.assertTrue(sanitizer.configure(spec, Path(self.tmp), 1, 5, self._log))
        self.assertIn("reusing existing configuration", " ".join(self.messages))

    def test_partial_flag_match_is_not_reused(self):
        # ASan on but UBSan off must still reconfigure: the lane asserts both.
        self._cache("CMAKE_GENERATOR:INTERNAL=Ninja\n"
                    "ENABLE_ASAN:BOOL=ON\nENABLE_UBSAN:BOOL=OFF\n")
        spec = sanitizer.BuildSpec(build_dir=self.build,
                                   sanitizer_flags=("ENABLE_ASAN=ON", "ENABLE_UBSAN=ON"))
        sanitizer.configure(spec, Path(self.tmp), 1, 5, self._log)
        joined = " ".join(self.messages)
        self.assertNotIn("reusing existing configuration", joined)
        self.assertIn("ENABLE_UBSAN=ON", joined)


class BuildSpecTest(unittest.TestCase):
    def test_lanes_do_not_disable_stdlib_fastpaths(self):
        # Turning XRAY_STDLIB_VM_FASTPATHS off looks like a free saving --
        # generating them is unrelated to memory safety -- but
        # test_stdlib_vm_fastpath_abi is registered under
        # if(XRAY_STDLIB_VM_FASTPATHS), so disabling it does not make that test
        # cheaper, it removes it from the lane. This pins the decision: a gate
        # may not trade coverage for speed.
        import re

        root = Path(__file__).resolve().parents[3]
        for name in ("run_asan_focused.py", "run_lsan_strict.py", "run_tsan_focused.py"):
            text = (root / "scripts" / name).read_text(encoding="utf-8")
            self.assertNotIn(
                'XRAY_STDLIB_VM_FASTPATHS=OFF', text,
                f"{name} disables the stdlib fastpaths, which drops "
                "test_stdlib_vm_fastpath_abi from the lane")

    def test_defaults(self):
        spec = sanitizer.BuildSpec(build_dir=Path("/tmp/x"),
                                   sanitizer_flags=("ENABLE_TSAN=ON",))
        self.assertEqual(spec.build_type, "Debug")
        self.assertEqual(spec.c_compiler, "clang")
        self.assertEqual(spec.targets, ())


class DefaultJobsTest(unittest.TestCase):
    def tearDown(self):
        os.environ.pop("XT_JOBS_PROBE", None)

    def test_env_override(self):
        os.environ["XT_JOBS_PROBE"] = "3"
        self.assertEqual(sanitizer.default_jobs("XT_JOBS_PROBE"), 3)

    def test_defaults_to_all_cores(self):
        # These lanes run RUN_SERIAL and own the machine, so a fixed small
        # number would leave most cores idle during the dominant build step.
        self.assertGreaterEqual(sanitizer.default_jobs("XT_JOBS_PROBE"), 1)

    def test_garbage_falls_back(self):
        os.environ["XT_JOBS_PROBE"] = "not-a-number"
        self.assertGreaterEqual(sanitizer.default_jobs("XT_JOBS_PROBE"), 1)


if __name__ == "__main__":
    unittest.main()
