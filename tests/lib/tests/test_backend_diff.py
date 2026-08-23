"""Unit tests for the backend-diff runner's own logic.

Covers the three things the migration contract requires: argument/env parsing,
command construction, and the ratchet decision. The command-construction test
in particular pins that the case-directory key includes .xr.expected, so a
changed oracle changes the key -- the miss that would otherwise diff a stale
binary against a new expectation and pass by accident.
"""

import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path, PureWindowsPath
from unittest import mock

from _support import bootstrap_xraytest, load_module

bootstrap_xraytest()

_RUNNER = Path(__file__).resolve().parents[2] / "diff" / "run_backend_diff.py"


rbd = load_module("rbd_under_test", _RUNNER)


class PathIdentityTest(unittest.TestCase):
    def test_windows_path_uses_manifest_separator(self):
        self.assertEqual(
            rbd.canonical_path_text(
                PureWindowsPath("tests", "diff", "cases", "nested", "case.xr")
            ),
            "tests/diff/cases/nested/case.xr",
        )

    def test_repository_relative_case_name_is_manifest_canonical(self):
        case = rbd.PROJECT_DIR / "tests" / "diff" / "cases" / "case.xr"
        self.assertEqual(rbd.rel_path(case), "tests/diff/cases/case.xr")


class JobConfigTest(unittest.TestCase):
    def test_explicit_job_count_is_not_auto(self):
        jobs, auto = rbd.configure_jobs("4")
        self.assertEqual(jobs, 4)
        self.assertFalse(auto)

    def test_auto_is_capped_and_flagged(self):
        jobs, auto = rbd.configure_jobs("auto")
        self.assertTrue(auto)
        self.assertLessEqual(jobs, 16)
        self.assertGreaterEqual(jobs, 1)

    def test_garbage_falls_back_to_serial(self):
        jobs, auto = rbd.configure_jobs("not-a-number")
        self.assertEqual(jobs, 1)

    def test_auto_jobs_honor_cold_and_hot_caps(self):
        env = {
            "XRAY_DIFF_COLD_MAX_AUTO_JOBS": "4",
            "XRAY_DIFF_HOT_MAX_AUTO_JOBS": "8",
        }
        with mock.patch.dict(os.environ, env, clear=True):
            self.assertEqual(rbd.cap_auto_jobs_for_cache_state(16, True, "cold"), 4)
            self.assertEqual(rbd.cap_auto_jobs_for_cache_state(16, True, "hot"), 8)

    def test_explicit_jobs_ignore_cache_state_caps(self):
        with mock.patch.dict(
            os.environ, {"XRAY_DIFF_COLD_MAX_AUTO_JOBS": "4"}, clear=True
        ):
            self.assertEqual(rbd.cap_auto_jobs_for_cache_state(12, False, "cold"), 12)


class ShardValidationTest(unittest.TestCase):
    def test_valid_shard(self):
        self.assertEqual(rbd.validate_shard("4", "2"), (4, 2))

    def test_index_past_total_rejected(self):
        with self.assertRaises(ValueError):
            rbd.validate_shard("4", "4")

    def test_non_numeric_rejected(self):
        with self.assertRaises(ValueError):
            rbd.validate_shard("x", "0")


class CaseKeyTest(unittest.TestCase):
    def setUp(self):
        import tempfile

        self.tmp = tempfile.mkdtemp(prefix="xt_bd.")
        self.dir = Path(self.tmp)

    def tearDown(self):
        import shutil

        shutil.rmtree(self.tmp, ignore_errors=True)

    def _case_dir_key(self):
        from xraytest import cache

        return cache.dir_key(self.dir, rbd.CASE_DIR_GLOBS)

    def test_expected_oracle_is_part_of_the_key(self):
        # The load-bearing invariant: change .xr.expected, the key must change,
        # or a cached binary would be diffed against a new oracle and pass.
        (self.dir / "c.xr").write_text("print(1)\n", encoding="utf-8")
        (self.dir / "c.xr.expected").write_text("1\n", encoding="utf-8")
        before = self._case_dir_key()
        (self.dir / "c.xr.expected").write_text("2\n", encoding="utf-8")
        after = self._case_dir_key()
        self.assertNotEqual(before, after)

    def test_args_sidecar_is_part_of_the_key(self):
        (self.dir / "c.xr").write_text("print(1)\n", encoding="utf-8")
        (self.dir / "c.args").write_text("--foo\n", encoding="utf-8")
        before = self._case_dir_key()
        (self.dir / "c.args").write_text("--bar\n", encoding="utf-8")
        self.assertNotEqual(before, self._case_dir_key())

    def test_cache_name_is_bounded_and_readable(self):
        name = rbd.case_cache_name("some/dir/a_very_long_case_name_that_exceeds.xr", "key")
        self.assertLessEqual(len(name), 32 + 1 + 24)
        self.assertTrue(name.startswith("a_very_long_case_name"))


class SidecarTest(unittest.TestCase):
    def setUp(self):
        import tempfile

        self.tmp = tempfile.mkdtemp(prefix="xt_bd_sc.")
        self.dir = Path(self.tmp)
        self.case = self.dir / "c.xr"
        self.case.write_text("// anchor: my-anchor\n// diff-backends: vm,aot\nprint(1)\n", encoding="utf-8")

    def tearDown(self):
        import shutil

        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_reads_anchor_directive(self):
        self.assertEqual(rbd.read_first_directive(self.case, "// anchor: ", 1), "my-anchor")

    def test_diff_backends_only_scanned_in_head(self):
        self.assertEqual(
            rbd.read_first_directive(self.case, "// diff-backends: ", 5), "vm,aot"
        )

    def test_args_first_line_split(self):
        (self.dir / "c.args").write_text("--a --b c\n", encoding="utf-8")
        self.assertEqual(rbd.read_args(self.case), ["--a", "--b", "c"])

    def test_missing_stdin_is_empty_bytes(self):
        self.assertEqual(rbd.read_stdin(self.case), b"")

    def test_expected_sidecar_read_as_bytes(self):
        Path(str(self.case) + ".expected").write_bytes(b"out\n")
        self.assertEqual(rbd.read_expected_stdout(self.case), b"out\n")

    def test_invalid_utf8_build_log_is_safe_for_windows_console(self):
        text = rbd.decode_build_log(b"diagnostic: \xff\n")
        self.assertEqual(text, "diagnostic: \\xff\n")
        text.encode("gbk", "strict")

    def test_unavailable_console_code_point_is_escaped(self):
        self.assertEqual(
            rbd.console_safe_text("diagnostic: \ufde8", "gbk"),
            "diagnostic: \\ufde8",
        )


class OptimizerPolicyPropagationTest(unittest.TestCase):
    POLICY = "vm=full,aot=full"

    def _config(self, root: Path) -> rbd.RunnerConfig:
        return rbd.RunnerConfig(
            xray=Path(sys.executable),
            backends=["vm", "aot"],
            jobs=1,
            aot_opt="2",
            aot_cache=root / "objects",
            aot_bin_cache=root / "binaries",
            embed_bin_cache=root / "embedded",
            diff_stderr=False,
            xi_opt=self.POLICY,
            case_timeout=30,
        )

    def test_diff_environment_becomes_the_single_runner_policy(self):
        case_result = rbd.CaseResult(order=0, status="pass", output="PASS\n")
        env = {
            "XRAY_DIFF_SINGLE_CASE": "case.xr",
            "XRAY_DIFF_XI_OPT": self.POLICY,
            # This is the compiler's process-level setting, not the runner's
            # provenance input. A conflicting value must not win.
            "XRAY_XI_OPT": "vm=none,aot=none",
        }
        with mock.patch.dict(os.environ, env, clear=True):
            with mock.patch.object(rbd, "diff_stable_cache_dir", return_value=Path("cache")):
                with mock.patch.object(rbd, "run_case", return_value=case_result) as run_case:
                    with contextlib.redirect_stdout(io.StringIO()):
                        status = rbd.main(["run_backend_diff.py", sys.executable])

        self.assertEqual(status, 0)
        config = run_case.call_args.args[0]
        self.assertEqual(config.xi_opt, self.POLICY)

    def test_vm_command_receives_the_runner_policy(self):
        with tempfile.TemporaryDirectory(prefix="xt_backend_diff_policy_vm.") as tmp:
            root = Path(tmp)
            case = root / "case.xr"
            case.write_text("print(1)\n", encoding="utf-8")
            result = rbd.proc.ProcResult((), 0, b"1\n", b"", False)
            with mock.patch.object(rbd.proc, "run", return_value=result) as run:
                backend = rbd.run_backend(self._config(root), "vm", case, [], b"")

        self.assertEqual(backend.rc, 0)
        self.assertEqual(
            run.call_args.args[0],
            [Path(sys.executable), "run", "--xi-opt", self.POLICY, case],
        )

    def test_aot_build_command_receives_the_same_runner_policy(self):
        with tempfile.TemporaryDirectory(prefix="xt_backend_diff_policy_aot.") as tmp:
            root = Path(tmp)
            case = root / "case.xr"
            case.write_text("print(1)\n", encoding="utf-8")
            calls = []

            def fake_run(argv, **kwargs):
                calls.append((list(argv), kwargs))
                output = Path(argv[argv.index("-o") + 1])
                output.write_bytes(b"binary")
                return rbd.proc.ProcResult(tuple(argv), 0, b"", b"", False)

            config = self._config(root)
            with mock.patch.object(rbd.proc, "run", side_effect=fake_run):
                binary, _ = rbd.build_case_binary(config, "aot", case, "case.xr", "case-key")

        self.assertIsNotNone(binary)
        self.assertEqual(
            calls[0][0],
            [
                Path(sys.executable),
                "build",
                "--xi-opt",
                self.POLICY,
                "--native",
                "-O",
                "2",
                "--cache-dir",
                config.aot_cache,
                case,
                "-o",
                calls[0][0][-1],
            ],
        )


class RatchetWiringTest(unittest.TestCase):
    """The runner delegates to xraytest.ratchet; verify the wiring, including
    the partial-run suppression of the now-passing check."""

    def test_new_failure_flagged(self):
        from xraytest import ratchet

        v = ratchet.evaluate(failed={"x.xr"}, baseline=set())
        self.assertEqual(v.new_failures, ["x.xr"])
        self.assertFalse(v.ok)

    def test_partial_run_never_reports_now_passing(self):
        # Mirrors main()'s partial-run call: unseen baseline entries are fed as
        # skipped, so none can be declared fixed.
        from xraytest import ratchet

        baseline = {"a.xr", "b.xr", "c.xr"}
        failed = {"a.xr"}
        v = ratchet.evaluate(failed=failed, baseline=baseline, skipped=baseline - failed)
        self.assertEqual(v.now_passing, [])
        self.assertTrue(v.ok)


class EntryIdentityTest(unittest.TestCase):
    def test_missing_xray_binary_fails_closed(self):
        with tempfile.TemporaryDirectory(prefix="xt_backend_diff.") as tmp:
            missing = Path(tmp) / "missing-xray"
            output = io.StringIO()

            with mock.patch.dict(os.environ, {}, clear=True):
                with contextlib.redirect_stdout(output):
                    status = rbd.main(["run_backend_diff.py", str(missing)])

            self.assertEqual(status, 1)
            self.assertIn("FAIL: xray binary not executable", output.getvalue())
            self.assertIn("0 passed, 1 failed, 0 skipped", output.getvalue())

    def test_missing_case_directory_fails_closed(self):
        with tempfile.TemporaryDirectory(prefix="xt_backend_diff.") as tmp:
            missing_cases = Path(tmp) / "missing-cases"
            output = io.StringIO()

            with mock.patch.object(rbd, "CASE_DIR", missing_cases):
                with mock.patch.dict(os.environ, {}, clear=True):
                    with contextlib.redirect_stdout(output):
                        status = rbd.main(["run_backend_diff.py", sys.executable])

            self.assertEqual(status, 1)
            self.assertIn(
                "governed backend-diff case directory is missing",
                output.getvalue(),
            )

    def test_zero_runnable_cases_fail_closed(self):
        with tempfile.TemporaryDirectory(prefix="xt_backend_diff.") as tmp:
            output = io.StringIO()

            with mock.patch.object(rbd, "CASE_DIR", Path(tmp)):
                with mock.patch.object(rbd, "collect_cases", return_value=[]):
                    with mock.patch.dict(os.environ, {}, clear=True):
                        with contextlib.redirect_stdout(output):
                            status = rbd.main(
                                ["run_backend_diff.py", sys.executable]
                            )

            self.assertEqual(status, 1)
            self.assertIn(
                "backend-diff discovered no runnable cases",
                output.getvalue(),
            )


if __name__ == "__main__":
    unittest.main()
