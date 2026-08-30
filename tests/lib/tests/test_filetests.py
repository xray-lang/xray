"""Unit tests for the AOT filetest runner's directive language and helpers.

The load-bearing assertions: the C-syntax include set must reach include/ (the
gap that hid behind a warm cache), and the link-mode normalizer must strip the
16-hex monomorphization suffix so a shape assertion is not pinned to a content
hash. Both are testable here without a compiler.
"""

import contextlib
import io
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from _support import bootstrap_xraytest, load_module

bootstrap_xraytest()

_AOT_DIR = Path(__file__).resolve().parents[2] / "aot"
sys.path.insert(0, str(_AOT_DIR))


expectlib = load_module("filetest_expect_under_test", _AOT_DIR / "filetest_expect.py")
runner = load_module("run_aot_filetests_under_test", _AOT_DIR / "run_aot_filetests.py")


class ParseTest(unittest.TestCase):
    def _parse(self, text):
        tmp = tempfile.mkdtemp(prefix="xt_exp.")
        p = Path(tmp) / "c.expect"
        p.write_text(text, encoding="utf-8")
        return expectlib.parse(p)

    def test_status_defaults_to_pass(self):
        self.assertEqual(self._parse("contains=x\n").status, "pass")

    def test_status_and_args_extracted(self):
        exp = self._parse("status=fail\nargs=--target foo\ncontains=y\n")
        self.assertEqual(exp.status, "fail")
        self.assertEqual(exp.args, "--target foo")

    def test_comments_and_blanks_ignored(self):
        exp = self._parse("# c\n\ncontains=z\n")
        self.assertEqual(len(exp.directives), 1)

    def test_missing_equals_is_parse_error(self):
        exp = self._parse("contains x\n")
        self.assertIsNotNone(exp.parse_error)

    def test_wants_c_syntax(self):
        self.assertTrue(self._parse("c_syntax=pass\n").wants_c_syntax)
        self.assertFalse(self._parse("contains=x\n").wants_c_syntax)

    def test_closed_grammar_rejects_unknown_empty_and_invalid_values(self):
        invalid = (
            "unknown=value\n",
            "contains=\n",
            "status=maybe\n",
            "c_syntax=fail\n",
            "product_status=pass\n",
            "artifact=present\n",
        )
        for text in invalid:
            with self.subTest(text=text):
                self.assertIsNotNone(self._parse(text).parse_error)

    def test_duplicate_controls_and_product_literals_fail_closed(self):
        invalid = (
            "status=pass\nstatus=pass\n",
            "args=--one\nargs=--two\n",
            "product_status=fail\nproduct_status=fail\n",
            "product_status=fail\nproduct_contains=code\nproduct_contains=code\nartifact=absent\n",
            "product_status=fail\nproduct_contains=code\nartifact=absent\nartifact=absent\n",
        )
        for text in invalid:
            with self.subTest(text=text):
                self.assertIsNotNone(self._parse(text).parse_error)

    def test_repeatable_assertions_preserve_distinct_and_legacy_duplicate_rows(self):
        exp = self._parse("contains=one\ncontains=two\ncontains=one\n")
        self.assertIsNone(exp.parse_error)

    def test_product_failure_requires_diagnostic_and_absent_artifact(self):
        invalid = (
            "product_contains=code\n",
            "artifact=absent\n",
            "product_status=fail\nartifact=absent\n",
            "product_status=fail\nproduct_contains=code\n",
            "status=fail\nproduct_status=fail\nproduct_contains=code\nartifact=absent\n",
        )
        for text in invalid:
            with self.subTest(text=text):
                self.assertIsNotNone(self._parse(text).parse_error)

    def test_absent_product_artifact_rejects_every_generated_c_directive(self):
        for key in sorted(expectlib._C_KEYS | {"c_syntax"}):
            value = "pass" if key == "c_syntax" else "needle"
            text = (
                "product_status=fail\n"
                "product_contains=code\n"
                "artifact=absent\n"
                f"{key}={value}\n"
            )
            with self.subTest(key=key):
                self.assertIsNotNone(self._parse(text).parse_error)

    def test_product_failure_properties_and_all_diagnostics(self):
        exp = self._parse(
            "product_status=fail\n"
            "product_contains=XR_TARGET_1003\n"
            "product_contains=no authority\n"
            "artifact=absent\n"
        )
        self.assertIsNone(exp.parse_error)
        self.assertTrue(exp.wants_product_failure)
        self.assertTrue(exp.wants_absent_artifact)
        self.assertEqual(["XR_TARGET_1003", "no authority"], exp.product_contains)
        self.assertTrue(expectlib.check_product(exp, "XR_TARGET_1003: no authority").ok)
        self.assertFalse(expectlib.check_product(exp, "XR_TARGET_1003 only").ok)


class CheckTest(unittest.TestCase):
    def _exp(self, *directives):
        tmp = tempfile.mkdtemp(prefix="xt_chk.")
        p = Path(tmp) / "c.expect"
        p.write_text("\n".join(directives) + "\n", encoding="utf-8")
        return expectlib.parse(p)

    def test_contains_pass_and_fail(self):
        self.assertTrue(expectlib.check(self._exp("contains=foo"), "a foo b", "").ok)
        out = expectlib.check(self._exp("contains=foo"), "no match", "")
        self.assertFalse(out.ok)
        self.assertIn("missing: foo", out.reason)

    def test_not_contains(self):
        self.assertTrue(expectlib.check(self._exp("not_contains=x"), "abc", "").ok)
        self.assertFalse(expectlib.check(self._exp("not_contains=x"), "axc", "").ok)

    def test_regex(self):
        self.assertTrue(expectlib.check(self._exp("regex=fo+"), "foo", "").ok)
        self.assertFalse(expectlib.check(self._exp("regex=^z"), "foo", "").ok)

    def test_regex_anchors_bind_per_line_like_grep(self):
        # grep -E matches line-wise; ^...$ means "some line looks like this".
        # The anchored pattern must hit a line in the MIDDLE of the text --
        # missing MULTILINE made every anchored expect fail against real dumps.
        body = "int a;\nstatic size_t helper_7(void) {\nint b;\n"
        self.assertTrue(
            expectlib.check(
                self._exp(r"c_regex=^static size_t helper_[0-9]+\(void\) \{$"), "", body
            ).ok
        )
        self.assertTrue(
            expectlib.check(self._exp(r"regex=^int b;$"), body, "").ok
        )
        # A pattern matching only a line PREFIX with $ must still fail.
        self.assertFalse(
            expectlib.check(self._exp(r"regex=^int$"), body, "").ok
        )

    def test_c_contains_targets_c_not_dump(self):
        # c_contains checks the generated C, not the dump.
        self.assertTrue(expectlib.check(self._exp("c_contains=int64_t"), "dump", "int64_t v;").ok)
        self.assertFalse(expectlib.check(self._exp("c_contains=int64_t"), "int64_t", "no c").ok)

    def test_c_count_exact(self):
        self.assertTrue(expectlib.check(self._exp("c_count=2:xx"), "", "xx yy xx").ok)
        out = expectlib.check(self._exp("c_count=2:xx"), "", "xx only")
        self.assertFalse(out.ok)
        self.assertIn("expected 2, got 1", out.reason)

    def test_c_count_bad_directive(self):
        self.assertFalse(expectlib.check(self._exp("c_count=notnum:x"), "", "x").ok)

    def test_skip_directive(self):
        out = expectlib.check(self._exp("skip=not ready"), "", "")
        self.assertTrue(out.skip)
        self.assertEqual(out.reason, "not ready")

    def test_first_failure_wins(self):
        out = expectlib.check(self._exp("contains=a", "contains=b"), "only a", "")
        self.assertIn("missing: b", out.reason)

    def test_args_and_status_are_not_checks(self):
        self.assertTrue(expectlib.check(self._exp("args=--x", "status=pass", "contains=a"), "a", "").ok)


class TargetTripleTest(unittest.TestCase):
    def test_space_form(self):
        self.assertEqual(expectlib.target_triple("--target riscv64 -O2"), "riscv64")

    def test_equals_form(self):
        self.assertEqual(expectlib.target_triple("--target=thumbv7"), "thumbv7")

    def test_absent(self):
        self.assertEqual(expectlib.target_triple("-O2 -c"), "")


class NormalizeLinkTest(unittest.TestCase):
    def test_strips_16_hex_monomorphization_suffix(self):
        text = "xrt_native_foo_0123456789abcdef_Point x;"
        self.assertEqual(expectlib.normalize_link_c(text), "xrt_native_foo_Point x;")

    def test_leaves_non_hex_alone(self):
        text = "xrt_native_foo_Point x;"
        self.assertEqual(expectlib.normalize_link_c(text), text)

    def test_shorter_hex_not_stripped(self):
        # Only exactly-16 hex runs are the mono suffix.
        text = "foo_012345_bar"
        self.assertEqual(expectlib.normalize_link_c(text), text)


class RunnerHelpersTest(unittest.TestCase):
    @staticmethod
    def _config(root: Path, timeout: float | None = 7) -> object:
        return runner.Config(
            xray=root / "xray",
            mode="link",
            selected_modes=["link"],
            verbose=False,
            keep_tmp=False,
            jobs=1,
            cache_dir=root / "cache",
            sanitizer=False,
            disable_run_cache=False,
            baseline=root / "baseline.txt",
            case_timeout=timeout,
        )

    @staticmethod
    def _proc_result(
        returncode: int = 1,
        stdout: bytes = b"",
        stderr: bytes = b"XR_TARGET_1003: no authority\n",
        timed_out: bool = False,
    ) -> object:
        return runner.proc.ProcResult(
            argv=("xray",),
            returncode=returncode,
            stdout=stdout,
            stderr=stderr,
            timed_out=timed_out,
        )

    def _run_product_case(
        self,
        *,
        result: object | None = None,
        timeout: float | None = 7,
        contract_result: object | None = None,
        prepare_artifact=None,
        dump_side_effect=None,
    ):
        temp = tempfile.TemporaryDirectory(prefix="xt_filetest_product.")
        root = Path(temp.name)
        xr_file = root / "case.xr"
        xr_file.write_text("print(1)\n", encoding="utf-8")
        xr_file.with_suffix(".expect").write_text(
            "product_status=fail\n"
            "product_contains=XR_TARGET_1003\n"
            "product_contains=no authority\n"
            "artifact=absent\n",
            encoding="utf-8",
        )
        if contract_result is not None:
            xr_file.with_name("case.contract.toml").write_text("version = 1\n", encoding="utf-8")
        out_c = root / "case.c"
        if prepare_artifact is not None:
            prepare_artifact(out_c, root)
        ws = mock.Mock()
        ws.path.return_value = out_c
        run_result = result or self._proc_result()
        run_verify_patch = mock.patch.object(
            runner,
            "run_verify",
            return_value=contract_result or self._proc_result(returncode=0, stderr=b""),
        )
        if dump_side_effect is None:
            run_dump_patch = mock.patch.object(runner, "run_dump", return_value=run_result)
        else:
            run_dump_patch = mock.patch.object(runner, "run_dump", side_effect=dump_side_effect)
        cache_patch = mock.patch.object(runner, "_dump_and_c_paths")
        verify = run_verify_patch.start()
        dump = run_dump_patch.start()
        cached = cache_patch.start()
        try:
            verdict = runner.run_one_case(self._config(root, timeout), "link", xr_file, ws)
        finally:
            run_verify_patch.stop()
            run_dump_patch.stop()
            cache_patch.stop()
            temp.cleanup()
        return verdict, verify, dump, cached

    def test_syntax_include_reaches_include_dir(self):
        # The regression that motivated task 258: the C syntax check must carry
        # -Iinclude, where xray_value_abi.h lives.
        self.assertIn("include", runner.SYNTAX_INCLUDE_DIRS)

    def test_host_c_syntax_uses_the_syntax_compiler_capability(self):
        with tempfile.TemporaryDirectory(prefix="xt_filetest_syntax.") as tmp:
            root = Path(tmp)
            generated = root / "generated.c"
            generated.write_text("int value;\n", encoding="utf-8")
            config = runner.Config(
                xray=root / "xray",
                mode="cgen",
                selected_modes=["cgen"],
                verbose=False,
                keep_tmp=False,
                jobs=1,
                cache_dir=root / "cache",
                sanitizer=False,
                disable_run_cache=True,
                baseline=root / "baseline.txt",
                case_timeout=7,
            )
            compiler = mock.Mock()
            compiler.syntax_check_argv.return_value = ["cl", "/Zs", str(generated)]
            process = mock.Mock(ok=True, timed_out=False)
            ws = mock.Mock()
            ws.path.return_value = root / "generated.obj"
            with mock.patch.object(
                runner.toolchain, "find_c_syntax_compiler", return_value=compiler
            ) as find_compiler, mock.patch.object(
                runner.proc, "run", return_value=process
            ) as run:
                outcome = runner.compile_c_syntax(config, generated, [], ws, "generated")

        self.assertTrue(outcome.ok)
        find_compiler.assert_called_once_with()
        compiler.syntax_check_argv.assert_called_once()
        run.assert_called_once_with(["cl", "/Zs", str(generated)], timeout=7)

    def test_all_modes_present(self):
        self.assertEqual(runner.ALL_MODES,
                         ["rep", "layout", "abi", "boundary", "container", "link", "cgen"])

    def test_collected_predicate(self):
        self.assertTrue(runner.is_collected(Path("a/b.xr")))
        self.assertFalse(runner.is_collected(Path("a/_helper.xr")))
        self.assertFalse(runner.is_collected(Path("a/notes.md")))

    def test_safe_case_name_sanitizes(self):
        self.assertEqual(runner.safe_case_name("a/b c.xr"), "a_b_c")

    def test_configure_jobs_auto_capped(self):
        self.assertGreaterEqual(runner.configure_jobs("auto"), 1)
        self.assertEqual(runner.configure_jobs("3"), 3)

    def test_case_directory_identity_is_hashed_once_per_directory(self):
        with tempfile.TemporaryDirectory(prefix="xt_filetest_keys.") as tmp:
            root = Path(tmp)
            first = root / "first"
            second = root / "second"
            first.mkdir()
            second.mkdir()
            config = runner.Config(
                xray=root / "xray",
                mode="all",
                selected_modes=["link", "cgen"],
                verbose=False,
                keep_tmp=False,
                jobs=4,
                cache_dir=root / "cache",
                sanitizer=False,
                disable_run_cache=False,
                baseline=root / "baseline.txt",
                case_timeout=1,
            )
            cases = [
                ("link", first / "a.xr"),
                ("link", first / "b.xr"),
                ("cgen", second / "c.xr"),
            ]
            with mock.patch.object(
                runner.cache, "dir_key", side_effect=lambda path: f"key:{path.name}"
            ) as dir_key:
                runner.prepare_case_directory_keys(config, cases)

        self.assertEqual(dir_key.call_count, 2)
        self.assertEqual(
            config.case_directory_keys,
            {first: "key:first", second: "key:second"},
        )

    def test_disabled_run_cache_does_not_hash_case_directories(self):
        with tempfile.TemporaryDirectory(prefix="xt_filetest_no_keys.") as tmp:
            root = Path(tmp)
            config = runner.Config(
                xray=root / "xray",
                mode="rep",
                selected_modes=["rep"],
                verbose=False,
                keep_tmp=False,
                jobs=1,
                cache_dir=root / "cache",
                sanitizer=False,
                disable_run_cache=True,
                baseline=root / "baseline.txt",
                case_timeout=1,
            )
            with mock.patch.object(runner.cache, "dir_key") as dir_key:
                runner.prepare_case_directory_keys(config, [("rep", root / "rep" / "a.xr")])

        dir_key.assert_not_called()
        self.assertEqual(config.case_directory_keys, {})

    def test_pre_measurement_failure_is_nonzero(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = runner._fail_before_measurement(
                "dump capability unavailable", 17, reason="unsupported option"
            )
        self.assertEqual(status, 1)
        self.assertIn("ERROR: dump capability unavailable", output.getvalue())
        self.assertIn("Cases not run: 17", output.getvalue())
        self.assertIn("0 passed, 1 failed, 0 skipped", output.getvalue())

    def test_main_missing_binary_fails_closed(self):
        with tempfile.TemporaryDirectory(prefix="xt_filetest_missing.") as tmp:
            missing = Path(tmp) / "definitely-missing-xray"
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                status = runner.main(["run_aot_filetests.py", "--xray", str(missing)])
        self.assertEqual(status, 1)
        self.assertIn("ERROR: xray binary not found or not executable", output.getvalue())

    def test_main_failed_dump_probe_fails_closed(self):
        with tempfile.TemporaryDirectory(prefix="xt_filetest_probe.") as tmp:
            fake_xray = Path(tmp) / "xray"
            fake_xray.write_text("not invoked\n", encoding="utf-8")
            fake_xray.chmod(fake_xray.stat().st_mode | stat.S_IXUSR)
            output = io.StringIO()
            with mock.patch.object(
                runner, "probe_dump_support", return_value=(False, "unsupported option\n")
            ), contextlib.redirect_stdout(output):
                status = runner.main(["run_aot_filetests.py", "--xray", str(fake_xray)])
        self.assertEqual(status, 1)
        self.assertIn("ERROR: xray build --native --dump-xaot-plan probe failed", output.getvalue())
        self.assertIn("Reason: unsupported option", output.getvalue())

    def test_expected_product_failure_bypasses_success_cache(self):
        verdict, _verify, dump, cached = self._run_product_case()
        self.assertEqual(runner.Status.PASS.value, verdict.status)
        dump.assert_called_once()
        cached.assert_not_called()

    def test_product_diagnostic_comes_only_from_product_command(self):
        verify_result = self._proc_result(
            returncode=0,
            stderr=b"XR_TARGET_1003: no authority\n",
        )
        product_result = self._proc_result(stderr=b"different failure\n")
        verdict, verify, _dump, _cached = self._run_product_case(
            result=product_result,
            contract_result=verify_result,
        )
        self.assertEqual(runner.Status.FAIL.value, verdict.status)
        self.assertIn("missing product output", verdict.detail)
        verify.assert_called_once()

    def test_product_timeout_unexpected_success_and_missing_diagnostic_are_red(self):
        results = (
            self._proc_result(returncode=-9, timed_out=True),
            self._proc_result(returncode=0, stderr=b""),
            self._proc_result(stderr=b"wrong failure\n"),
        )
        expected = ("timed out", "unexpectedly succeeded", "missing product output")
        for result, detail in zip(results, expected):
            with self.subTest(detail=detail):
                verdict, _verify, _dump, cached = self._run_product_case(result=result)
                self.assertEqual(runner.Status.FAIL.value, verdict.status)
                self.assertIn(detail, verdict.detail)
                cached.assert_not_called()

    def test_product_failure_requires_finite_timeout_before_execution(self):
        verdict, _verify, dump, cached = self._run_product_case(timeout=None)
        self.assertEqual(runner.Status.FAIL.value, verdict.status)
        self.assertIn("finite case timeout", verdict.detail)
        dump.assert_not_called()
        cached.assert_not_called()

    def test_parse_failure_starts_no_compiler_process(self):
        with tempfile.TemporaryDirectory(prefix="xt_filetest_parse_red.") as tmp:
            root = Path(tmp)
            xr_file = root / "case.xr"
            xr_file.write_text("print(1)\n", encoding="utf-8")
            xr_file.with_suffix(".expect").write_text(
                "product_status=fail\nartifact=absent\n", encoding="utf-8"
            )
            ws = mock.Mock()
            with mock.patch.object(runner, "run_verify") as verify, mock.patch.object(
                runner, "run_dump"
            ) as dump, mock.patch.object(runner, "_dump_and_c_paths") as cached:
                verdict = runner.run_one_case(
                    self._config(root), "link", xr_file, ws
                )
        self.assertEqual(runner.Status.FAIL.value, verdict.status)
        self.assertIn("requires at least one product_contains", verdict.detail)
        verify.assert_not_called()
        dump.assert_not_called()
        cached.assert_not_called()

    def test_absent_artifact_rejects_files_directories_and_symlinks(self):
        def regular(path, _root):
            path.write_text("partial", encoding="utf-8")

        def directory(path, _root):
            path.mkdir()

        def live_symlink(path, root):
            target = root / "target"
            target.write_text("target", encoding="utf-8")
            path.symlink_to(target)

        def dangling_symlink(path, root):
            path.symlink_to(root / "missing-target")

        for kind, setup in (
            ("regular file", regular),
            ("directory", directory),
            ("symbolic link", live_symlink),
            ("symbolic link", dangling_symlink),
        ):
            with self.subTest(kind=kind, setup=setup.__name__):
                verdict, _verify, dump, _cached = self._run_product_case(
                    prepare_artifact=setup
                )
                self.assertEqual(runner.Status.FAIL.value, verdict.status)
                self.assertIn(kind, verdict.detail)
                dump.assert_not_called()

    def test_product_artifact_created_by_failed_command_is_red(self):
        def create_artifact(*_args, **_kwargs):
            out_c = _args[1]
            out_c.write_text("partial", encoding="utf-8")
            return self._proc_result()

        verdict, _verify, _dump, _cached = self._run_product_case(
            dump_side_effect=create_artifact
        )
        self.assertEqual(runner.Status.FAIL.value, verdict.status)
        self.assertIn("regular file", verdict.detail)

    def test_artifact_lstat_race_fails_closed(self):
        path = Path("raced-product.c")
        with mock.patch.object(runner.os.path, "lexists", return_value=True), mock.patch.object(
            runner.os, "lstat", side_effect=FileNotFoundError("raced")
        ):
            reason = runner._artifact_absence_error(path)
        self.assertIn("changed while checking absence", reason)

    def test_report_rejects_all_skipped_measurement(self):
        with tempfile.TemporaryDirectory(prefix="xt_filetest_zero.") as tmp:
            config = runner.Config(
                xray=Path(tmp) / "xray",
                mode="rep",
                selected_modes=["rep"],
                verbose=False,
                keep_tmp=False,
                jobs=1,
                cache_dir=Path(tmp) / "cache",
                sanitizer=False,
                disable_run_cache=True,
                baseline=Path(tmp) / "baseline.txt",
                case_timeout=1,
            )
            verdicts = [
                runner.CaseVerdict("pending.xr", "rep", runner.Status.SKIP.value, "pending")
            ]
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                status = runner._report_and_ratchet(config, verdicts)
        self.assertEqual(status, 1)
        self.assertIn("no measured verdicts", output.getvalue())

    def test_report_accepts_at_least_one_measured_verdict(self):
        with tempfile.TemporaryDirectory(prefix="xt_filetest_measured.") as tmp:
            config = runner.Config(
                xray=Path(tmp) / "xray",
                mode="rep",
                selected_modes=["rep"],
                verbose=False,
                keep_tmp=False,
                jobs=1,
                cache_dir=Path(tmp) / "cache",
                sanitizer=False,
                disable_run_cache=True,
                baseline=Path(tmp) / "baseline.txt",
                case_timeout=1,
            )
            verdicts = [
                runner.CaseVerdict("measured.xr", "rep", runner.Status.PASS.value),
                runner.CaseVerdict("pending.xr", "rep", runner.Status.SKIP.value, "pending"),
            ]
            with contextlib.redirect_stdout(io.StringIO()):
                status = runner._report_and_ratchet(config, verdicts)
        self.assertEqual(status, 0)


if __name__ == "__main__":
    unittest.main()
