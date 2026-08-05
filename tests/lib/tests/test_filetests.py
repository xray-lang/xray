"""Unit tests for the AOT filetest runner's directive language and helpers.

The load-bearing assertions: the C-syntax include set must reach include/ (the
gap that hid behind a warm cache), and the link-mode normalizer must strip the
16-hex monomorphization suffix so a shape assertion is not pinned to a content
hash. Both are testable here without a compiler.
"""

import sys
import tempfile
import unittest
from pathlib import Path

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
    def test_syntax_include_reaches_include_dir(self):
        # The regression that motivated task 258: the C syntax check must carry
        # -Iinclude, where xray_value_abi.h lives.
        self.assertIn("include", runner.SYNTAX_INCLUDE_DIRS)

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


if __name__ == "__main__":
    unittest.main()
