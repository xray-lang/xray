"""Tests for the shared sanitizer-lane plumbing.

The two load-bearing invariants, both testable without any sanitizer build:

  - a lane must refuse a build tree that does not actually have the sanitizer on
  - a lane must prove the exact source, generator, profile, and compiler tree
    before allowing Ninja to decide whether every target is current

Either failing silently produces the worst possible outcome for these lanes: a
green result the tree never earned.
"""

import os
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from pathlib import Path
from unittest import mock

from _support import bootstrap_xraytest, load_module

bootstrap_xraytest()
from xraytest import sanitizer  # noqa: E402

ROOT = Path(__file__).resolve().parents[3]
SCRIPTS = ROOT / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))
asan_runner = load_module(
    "run_asan_focused_under_test", ROOT / "scripts" / "run_asan_focused.py")


class AsanEntryPointTest(unittest.TestCase):
    def test_help_never_acquires_the_build_tree_or_runs_the_lane(self):
        with mock.patch.object(asan_runner.buildlock, "BuildTreeLock") as lock, \
                mock.patch.object(asan_runner, "_run_main") as run, \
                redirect_stdout(StringIO()) as output:
            self.assertEqual(
                asan_runner.main(["run_asan_focused.py", "--help"]), 0)
        self.assertIn("usage: run_asan_focused.py", output.getvalue())
        self.assertIn("h2-source", output.getvalue())
        lock.assert_not_called()
        run.assert_not_called()

    def test_full_profile_rejects_a_partial_build_inventory(self):
        with mock.patch.dict(os.environ, {
                "XR_ASAN_PROFILE": "full",
                "XR_ASAN_BUILD_TARGETS": "test_xr_program",
             }, clear=True), \
             mock.patch.object(asan_runner.sanitizer, "configure") as configure:
            self.assertEqual(asan_runner._run_main(["run_asan_focused.py"]), 1)
        configure.assert_not_called()

    def _run_full(self, root: Path, events: list[str], tree_problem=None) -> int:
        build_dir = root / "build-asan"
        build_dir.mkdir()
        (build_dir / asan_runner.platform.exe_name("xray")).write_bytes(b"binary")
        ok = sanitizer.proc.ProcResult(
            argv=("ctest",), returncode=0, stdout=b"", stderr=b"", timed_out=False)

        def configure(*_args, **_kwargs):
            events.append("configure")
            return True

        def build(*_args, **_kwargs):
            events.append("build")
            return True

        def verify(*_args, **_kwargs):
            events.append("verify")
            return tree_problem

        def ctest(*_args, **_kwargs):
            events.append("ctest")
            return ok

        with mock.patch.dict(os.environ, {
                "XR_ASAN_PROFILE": "full",
                "XR_ASAN_BUILD_DIR": "build-asan",
             }, clear=True), \
             mock.patch.object(asan_runner, "PROJECT_DIR", root), \
             mock.patch.object(asan_runner.sanitizer, "resolve_compiler_command",
                               side_effect=lambda command: command), \
             mock.patch.object(asan_runner.sanitizer,
                               "activate_windows_msvc_environment", return_value=True), \
             mock.patch.object(asan_runner.sanitizer,
                               "activate_windows_dynamic_asan_runtime", return_value=True), \
             mock.patch.object(asan_runner.sanitizer, "configure",
                               side_effect=configure), \
             mock.patch.object(asan_runner.sanitizer, "build", side_effect=build), \
             mock.patch.object(asan_runner.sanitizer, "verify_build_tree",
                               side_effect=verify), \
             mock.patch.object(asan_runner.sanitizer, "ctest", side_effect=ctest), \
             mock.patch.object(asan_runner.sanitizer, "ctest_has_match",
                               return_value=False), \
             mock.patch.object(asan_runner, "compile_workload", return_value=True):
            return asan_runner._run_main(["run_asan_focused.py"])

    def test_full_profile_always_builds_before_post_check_and_ctest(self):
        with tempfile.TemporaryDirectory(prefix="xt_asan_entry.") as temp:
            events: list[str] = []
            self.assertEqual(self._run_full(Path(temp), events), 0)
        self.assertEqual(events, ["configure", "build", "verify", "ctest"])

    def test_failed_post_check_starts_no_ctest(self):
        with tempfile.TemporaryDirectory(prefix="xt_asan_entry.") as temp:
            events: list[str] = []
            self.assertEqual(
                self._run_full(Path(temp), events, "wrong compiler identity"), 1)
        self.assertEqual(events, ["configure", "build", "verify"])

    def test_unknown_argument_fails_before_acquiring_the_build_tree(self):
        with mock.patch.object(asan_runner.buildlock, "BuildTreeLock") as lock, \
                mock.patch.object(asan_runner, "_run_main") as run, \
                redirect_stderr(StringIO()) as output:
            self.assertEqual(asan_runner.main(["run_asan_focused.py", "--typo"]), 2)
        self.assertIn("unrecognized arguments: --typo", output.getvalue())
        lock.assert_not_called()
        run.assert_not_called()

    def test_exact_execution_requires_shared_junit_set(self):
        messages = []

        def log(message, *, error=False):
            messages.append((message, error))

        with tempfile.TemporaryDirectory(prefix="xt_asan_junit.") as directory:
            report = Path(directory) / "ctest.xml"
            for profile_name, (expected, _) in asan_runner.EXACT_PROFILES.items():
                with self.subTest(profile=profile_name):
                    cases = "".join(f"<testcase name='{name}'/>" for name in expected)
                    report.write_text(f"<testsuite>{cases}</testsuite>", encoding="utf-8")
                    self.assertTrue(asan_runner.verify_exact_execution(
                        report, expected, profile_name, log
                    ))
                    names = list(expected)
                    cases = "".join(
                        f"<testcase name='{name}'/>"
                        for name in [*names[:-1], "same_count_wrong_test"]
                    )
                    report.write_text(f"<testsuite>{cases}</testsuite>", encoding="utf-8")
                    self.assertFalse(asan_runner.verify_exact_execution(
                        report, expected, profile_name, log
                    ))
        self.assertTrue(any("was not executed" in message and error
                            for message, error in messages))
        self.assertTrue(any("unexpected" in message and error
                            for message, error in messages))

    def test_generic_identity_asan_profile_reuses_shared_inventory(self):
        tests, targets = asan_runner.EXACT_PROFILES["generic-identity"]
        self.assertIs(tests, asan_runner.canonical_profile.GENERIC_IDENTITY_CTEST_NAMES)
        self.assertIs(targets, asan_runner.canonical_profile.GENERIC_IDENTITY_BUILD_TARGETS)

    def test_h2_reference_asan_profile_reuses_shared_inventory(self):
        tests, targets = asan_runner.EXACT_PROFILES["h2-reference"]
        self.assertIs(tests, asan_runner.canonical_profile.H2_REFERENCE_CTEST_NAMES)
        self.assertIs(targets, asan_runner.canonical_profile.H2_REFERENCE_BUILD_TARGETS)

    def test_h2_aggregate_asan_profile_reuses_shared_inventory(self):
        tests, targets = asan_runner.EXACT_PROFILES["h2"]
        self.assertIs(tests, asan_runner.canonical_profile.H2_CTEST_NAMES)
        self.assertIs(targets, asan_runner.canonical_profile.H2_BUILD_TARGETS)

    def test_h2_private_asan_profiles_reuse_shared_inventories(self):
        for name, tests, targets in (
            ("h2-source", asan_runner.canonical_profile.H2_SOURCE_CTEST_NAMES,
             asan_runner.canonical_profile.H2_SOURCE_BUILD_TARGETS),
            ("h2-vm", asan_runner.canonical_profile.H2_VM_CTEST_NAMES,
             asan_runner.canonical_profile.H2_VM_BUILD_TARGETS),
            ("h2-aot", asan_runner.canonical_profile.H2_AOT_CTEST_NAMES,
             asan_runner.canonical_profile.H2_AOT_BUILD_TARGETS),
        ):
            with self.subTest(name=name):
                actual_tests, actual_targets = asan_runner.EXACT_PROFILES[name]
                self.assertIs(actual_tests, tests)
                self.assertIs(actual_targets, targets)


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

    def test_non_bool_cache_identity_is_verified(self):
        self._cache("CMAKE_MSVC_RUNTIME_LIBRARY:UNINITIALIZED=MultiThreadedDLL\n")
        self.assertIsNone(sanitizer.verify_configured(
            self.build, "CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL"))
        self.assertIsNotNone(sanitizer.verify_configured(
            self.build, "CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDebugDLL"))

    def test_windows_compiler_identity_uses_windows_path_semantics(self):
        self._cache(
            "CMAKE_C_COMPILER:STRING=C:/Program Files/LLVM/bin/clang-cl.EXE\n"
            "CMAKE_CXX_COMPILER:STRING=C:\\Program Files\\LLVM\\bin\\clang-cl.EXE\n"
        )
        with mock.patch.object(sanitizer.platform, "IS_WINDOWS", True):
            self.assertIsNone(sanitizer.verify_configured(
                self.build,
                "CMAKE_C_COMPILER=c:\\program files\\llvm\\bin\\clang-cl.exe",
            ))
            self.assertIsNone(sanitizer.verify_configured(
                self.build,
                "CMAKE_CXX_COMPILER=C:/PROGRAM FILES/LLVM/bin/clang-cl.exe",
            ))
            self.assertIsNotNone(sanitizer.verify_configured(
                self.build,
                "CMAKE_C_COMPILER=C:/Program Files/LLVM/bin/clang.exe",
            ))

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

    def test_compiler_command_uses_path_resolution(self):
        with mock.patch.object(sanitizer.shutil, "which", return_value="C:/tools/clang.exe"):
            self.assertEqual(sanitizer.resolve_compiler_command("clang"),
                             "C:/tools/clang.exe")

    def test_compiler_command_finds_standard_windows_llvm(self):
        llvm_bin = self.build / "LLVM" / "bin"
        llvm_bin.mkdir(parents=True)
        compiler = llvm_bin / "clang.exe"
        compiler.write_text("binary\n", encoding="utf-8")
        with (mock.patch.object(sanitizer.shutil, "which", return_value=None),
              mock.patch.object(sanitizer.platform, "IS_WINDOWS", True),
              mock.patch.dict(os.environ, {"ProgramFiles": str(self.build)})):
            self.assertEqual(Path(sanitizer.resolve_compiler_command("clang")), compiler)

    def test_clang_cl_does_not_need_to_be_on_windows_path(self):
        llvm_bin = self.build / "LLVM" / "bin"
        llvm_bin.mkdir(parents=True)
        compiler = llvm_bin / "clang-cl.exe"
        compiler.write_text("binary\n", encoding="utf-8")
        with (mock.patch.object(sanitizer.shutil, "which", return_value=None),
              mock.patch.object(sanitizer.platform, "IS_WINDOWS", True),
              mock.patch.dict(os.environ, {"ProgramFiles": str(self.build)})):
            self.assertEqual(
                Path(sanitizer.resolve_compiler_command("clang-cl")), compiler)

    def test_complete_build_tree_identity_is_accepted(self):
        source = self.build / "source"
        source.mkdir()
        build = self.build / "tree"
        build.mkdir()
        (build / "build.ninja").write_text("# generated\n", encoding="utf-8")
        (build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={source.resolve()}\n"
            "CMAKE_GENERATOR:INTERNAL=Ninja\n"
            "CMAKE_BUILD_TYPE:STRING=Debug\n"
            "CMAKE_C_COMPILER:FILEPATH=fixture-clang\n"
            "CMAKE_CXX_COMPILER:FILEPATH=fixture-clang++\n"
            "ENABLE_ASAN:BOOL=ON\nENABLE_UBSAN:BOOL=ON\n",
            encoding="utf-8",
        )
        spec = sanitizer.BuildSpec(
            build_dir=build,
            sanitizer_flags=("ENABLE_ASAN=ON", "ENABLE_UBSAN=ON"),
            c_compiler="fixture-clang",
            cxx_compiler="fixture-clang++",
        )
        with mock.patch.object(sanitizer, "resolve_compiler_command",
                               side_effect=lambda command: command):
            self.assertIsNone(sanitizer.verify_build_tree(spec, source))

    def test_build_tree_identity_rejects_each_mismatched_dimension(self):
        source = self.build / "source"
        source.mkdir()
        build = self.build / "tree"
        build.mkdir()
        (build / "build.ninja").write_text("# generated\n", encoding="utf-8")
        base = (
            f"CMAKE_HOME_DIRECTORY:INTERNAL={source.resolve()}\n"
            "CMAKE_GENERATOR:INTERNAL=Ninja\n"
            "CMAKE_BUILD_TYPE:STRING=Debug\n"
            "CMAKE_C_COMPILER:FILEPATH=fixture-clang\n"
            "CMAKE_CXX_COMPILER:FILEPATH=fixture-clang++\n"
            "ENABLE_ASAN:BOOL=ON\nENABLE_UBSAN:BOOL=ON\n"
        )
        spec = sanitizer.BuildSpec(
            build_dir=build,
            sanitizer_flags=("ENABLE_ASAN=ON", "ENABLE_UBSAN=ON"),
            c_compiler="fixture-clang",
            cxx_compiler="fixture-clang++",
        )
        mutations = {
            "source root": (str(source.resolve()), str((self.build / "other").resolve())),
            "generator": ("CMAKE_GENERATOR:INTERNAL=Ninja",
                          "CMAKE_GENERATOR:INTERNAL=Unix Makefiles"),
            "build type": ("CMAKE_BUILD_TYPE:STRING=Debug",
                           "CMAKE_BUILD_TYPE:STRING=Release"),
            "C compiler": ("CMAKE_C_COMPILER:FILEPATH=fixture-clang",
                           "CMAKE_C_COMPILER:FILEPATH=other-clang"),
            "UBSan": ("ENABLE_UBSAN:BOOL=ON", "ENABLE_UBSAN:BOOL=OFF"),
        }
        with mock.patch.object(sanitizer, "resolve_compiler_command",
                               side_effect=lambda command: command):
            for label, (old, new) in mutations.items():
                with self.subTest(label=label):
                    (build / "CMakeCache.txt").write_text(
                        base.replace(old, new), encoding="utf-8")
                    self.assertIsNotNone(sanitizer.verify_build_tree(spec, source))

    def test_non_windows_does_not_need_msvc_environment(self):
        with mock.patch.object(sanitizer.platform, "IS_WINDOWS", False):
            self.assertTrue(sanitizer.activate_windows_msvc_environment(mock.Mock()))

    def test_windows_msvc_environment_is_loaded_from_vsdevcmd(self):
        installer = self.build / "Microsoft Visual Studio" / "Installer"
        installer.mkdir(parents=True)
        vswhere = installer / "vswhere.exe"
        vswhere.write_bytes(b"binary")
        installation = self.build / "Visual Studio" / "BuildTools"
        vsdevcmd = installation / "Common7" / "Tools" / "VsDevCmd.bat"
        vsdevcmd.parent.mkdir(parents=True)
        vsdevcmd.write_bytes(b"batch")
        reports = [
            sanitizer.proc.ProcResult(
                argv=(str(vswhere),), returncode=0,
                stdout=(str(installation) + "\r\n").encode(), stderr=b"",
                timed_out=False),
            sanitizer.proc.ProcResult(
                argv=("cmd.exe",), returncode=0,
                stdout=("Path=C:\\tools\r\nINCLUDE=C:\\include\r\n"
                        "LIB=C:\\lib\r\nLIBPATH=C:\\libpath\r\n").encode("utf-16-le"),
                stderr=b"", timed_out=False),
        ]
        messages = []

        def log(message, *, error=False):
            messages.append((message, error))

        with (mock.patch.object(sanitizer.platform, "IS_WINDOWS", True),
              mock.patch.object(sanitizer.shutil, "which", return_value=None),
              mock.patch.object(sanitizer.proc, "run", side_effect=reports),
              mock.patch.dict(os.environ, {
                  "ProgramFiles(x86)": str(self.build), "COMSPEC": "cmd.exe",
              }, clear=True)):
            self.assertTrue(sanitizer.activate_windows_msvc_environment(log))
            self.assertEqual(os.environ["INCLUDE"], r"C:\include")
            self.assertEqual(os.environ["LIB"], r"C:\lib")
            self.assertEqual(os.environ["PATH"], r"C:\tools")
        self.assertIn("MSVC x64 SDK environment activated", messages[-1][0])

    def test_windows_msvc_environment_fails_without_vswhere(self):
        messages = []

        def log(message, *, error=False):
            messages.append((message, error))

        with (mock.patch.object(sanitizer.platform, "IS_WINDOWS", True),
              mock.patch.object(sanitizer.shutil, "which", return_value=None),
              mock.patch.dict(os.environ, {}, clear=True)):
            self.assertFalse(sanitizer.activate_windows_msvc_environment(log))
        self.assertTrue(messages[-1][1])

    def test_windows_dynamic_asan_runtime_is_inherited_from_clang(self):
        resource = self.build / "LLVM" / "lib" / "clang" / "22"
        runtime = resource / "lib" / "windows"
        runtime.mkdir(parents=True)
        (runtime / "clang_rt.asan_dynamic-x86_64.dll").write_bytes(b"dll")
        missing_runtime = self.build / "LLVM" / "lib" / "runtime"
        reports = [
            sanitizer.proc.ProcResult(
                argv=("clang", "--print-runtime-dir"), returncode=0,
                stdout=(str(missing_runtime) + "\n").encode(), stderr=b"",
                timed_out=False),
            sanitizer.proc.ProcResult(
                argv=("clang", "--print-resource-dir"), returncode=0,
                stdout=(str(resource) + "\n").encode(), stderr=b"",
                timed_out=False),
        ]
        messages = []

        def log(message, *, error=False):
            messages.append((message, error))

        spec = sanitizer.BuildSpec(
            build_dir=self.build, sanitizer_flags=("ENABLE_ASAN=ON",))
        with (mock.patch.object(sanitizer.platform, "IS_WINDOWS", True),
              mock.patch.object(sanitizer, "resolve_compiler_command",
                                return_value="clang"),
              mock.patch.object(sanitizer.proc, "run", side_effect=reports),
              mock.patch.dict(os.environ, {"PATH": "existing"})):
            self.assertTrue(
                sanitizer.activate_windows_dynamic_asan_runtime(spec, log))
            self.assertEqual(os.environ["PATH"].split(os.pathsep)[0], str(runtime))
        self.assertIn("Windows ASan runtime=", messages[-1][0])

    def test_missing_windows_dynamic_asan_runtime_fails_closed(self):
        report = sanitizer.proc.ProcResult(
            argv=("clang", "--print-resource-dir"), returncode=0,
            stdout=(str(self.build / "missing") + "\n").encode(), stderr=b"",
            timed_out=False)
        messages = []

        def log(message, *, error=False):
            messages.append((message, error))

        spec = sanitizer.BuildSpec(
            build_dir=self.build, sanitizer_flags=("ENABLE_ASAN=ON",))
        with (mock.patch.object(sanitizer.platform, "IS_WINDOWS", True),
              mock.patch.object(sanitizer, "resolve_compiler_command",
                                return_value="clang"),
              mock.patch.object(sanitizer.proc, "run", return_value=report)):
            self.assertFalse(
                sanitizer.activate_windows_dynamic_asan_runtime(spec, log))
        self.assertTrue(messages[-1][1])


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
        identity = (
            f"CMAKE_HOME_DIRECTORY:INTERNAL={Path(self.tmp).resolve()}\n"
            "CMAKE_BUILD_TYPE:STRING=Debug\n"
            f"CMAKE_C_COMPILER:FILEPATH={sanitizer.resolve_compiler_command('clang')}\n"
            f"CMAKE_CXX_COMPILER:FILEPATH={sanitizer.resolve_compiler_command('clang++')}\n"
        )
        (self.build / "CMakeCache.txt").write_text(
            identity + text, encoding="utf-8")

    def _complete_ninja_tree(self):
        (self.build / "build.ninja").write_text("# generated\n", encoding="utf-8")

    def test_tree_with_sanitizer_off_is_not_reused(self):
        self._cache("CMAKE_GENERATOR:INTERNAL=Ninja\nENABLE_TSAN:BOOL=OFF\n")
        self._complete_ninja_tree()
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
        self._complete_ninja_tree()
        spec = sanitizer.BuildSpec(build_dir=self.build,
                                   sanitizer_flags=("ENABLE_TSAN=ON",))
        self.assertTrue(sanitizer.configure(spec, Path(self.tmp), 1, 5, self._log))
        self.assertIn("reusing existing configuration", " ".join(self.messages))

    def test_incomplete_ninja_tree_is_not_reused(self):
        self._cache("CMAKE_GENERATOR:INTERNAL=Ninja\nENABLE_TSAN:BOOL=ON\n")
        spec = sanitizer.BuildSpec(build_dir=self.build,
                                   sanitizer_flags=("ENABLE_TSAN=ON",))
        sanitizer.configure(spec, Path(self.tmp), 1, 5, self._log)
        joined = " ".join(self.messages)
        self.assertNotIn("reusing existing configuration", joined)
        self.assertIn("has no build.ninja", joined)

    def test_partial_flag_match_is_not_reused(self):
        # ASan on but UBSan off must still reconfigure: the lane asserts both.
        self._cache("CMAKE_GENERATOR:INTERNAL=Ninja\n"
                    "ENABLE_ASAN:BOOL=ON\nENABLE_UBSAN:BOOL=OFF\n")
        self._complete_ninja_tree()
        spec = sanitizer.BuildSpec(build_dir=self.build,
                                   sanitizer_flags=("ENABLE_ASAN=ON", "ENABLE_UBSAN=ON"))
        sanitizer.configure(spec, Path(self.tmp), 1, 5, self._log)
        joined = " ".join(self.messages)
        self.assertNotIn("reusing existing configuration", joined)
        self.assertIn("ENABLE_UBSAN=ON", joined)

    def test_foreign_source_tree_is_refused_without_deletion(self):
        foreign = self.build / "foreign"
        (self.build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={foreign.resolve()}\n"
            "CMAKE_GENERATOR:INTERNAL=Ninja\nENABLE_TSAN:BOOL=ON\n",
            encoding="utf-8",
        )
        sentinel = self.build / "keep.me"
        sentinel.write_text("owned by another checkout\n", encoding="utf-8")
        spec = sanitizer.BuildSpec(build_dir=self.build,
                                   sanitizer_flags=("ENABLE_TSAN=ON",))
        with mock.patch.object(sanitizer.proc, "run") as run:
            self.assertFalse(
                sanitizer.configure(spec, Path(self.tmp), 1, 5, self._log))
        self.assertTrue(sentinel.is_file())
        run.assert_not_called()


class BuildSpecTest(unittest.TestCase):
    def test_lanes_pin_supported_stdlib_bootstrap_shape(self):
        # Sanitizer evidence must use the same bootstrap contract as every
        # target-machine build. Inheriting the ON default would run a native AOT
        # generator before the compiler under test has proved the required
        # target families and can hang before sanitizer tests start.
        root = Path(__file__).resolve().parents[3]
        for name in ("run_asan_focused.py", "run_lsan_strict.py", "run_tsan_focused.py"):
            text = (root / "scripts" / name).read_text(encoding="utf-8")
            self.assertIn(
                'XRAY_STDLIB_VM_FASTPATHS=OFF', text,
                f"{name} does not pin the supported stdlib bootstrap shape")

    def test_asan_pins_supported_windows_crt_identity(self):
        root = Path(__file__).resolve().parents[3]
        text = (root / "scripts" / "run_asan_focused.py").read_text(encoding="utf-8")
        self.assertIn("CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL", text)

    def test_defaults(self):
        spec = sanitizer.BuildSpec(build_dir=Path("/tmp/x"),
                                   sanitizer_flags=("ENABLE_TSAN=ON",))
        self.assertEqual(spec.build_type, "Debug")
        self.assertEqual(spec.c_compiler, "clang")
        self.assertEqual(spec.targets, ())

    def test_ctest_writes_owned_junit_evidence_when_requested(self):
        completed = sanitizer.proc.ProcResult(
            argv=("ctest",), returncode=0, stdout=b"", stderr=b"", timed_out=False)
        with mock.patch.object(sanitizer.proc, "run", return_value=completed) as run:
            result = sanitizer.ctest(Path("build"), include="^exact$", jobs=3,
                                     junit=Path("evidence.xml"))
        self.assertTrue(result.ok)
        self.assertEqual(run.call_args.args[0][-2:],
                         ["--output-junit", "evidence.xml"])


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


class ConsoleOutputTest(unittest.TestCase):
    def test_narrow_console_escapes_unrepresentable_diagnostics(self):
        class NarrowStream:
            encoding = "ascii"

            def __init__(self):
                self.text = ""

            def write(self, value):
                self.text += value

        stream = NarrowStream()
        sanitizer.write_console(stream, "PASS \u2713\n")
        self.assertEqual(stream.text, "PASS \\u2713\n")


if __name__ == "__main__":
    unittest.main()
