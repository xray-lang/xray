#!/usr/bin/env python3
"""Counterexamples for explicit frontend receipts, not a G13/runtime waiver."""
import contextlib
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import check_spec_frontend_admission as gate


def receipt(role="CHECK", code="print(1)"):
    case = dict(id="case", language="cn", role=role, mode="parse" if role == "PARSE" else "check",
                required_for_task342=True, source=dict(path="spec.md", fence_id="example"),
                body_sha256=gate.sha(code.encode()), responsibility="Actual fixed frontend receipt",
                expected=dict(exit=0 if role in ("CHECK", "PARSE") else 1))
    if role in ("TRUE_ERROR", "KNOWN_LIMITATION"):
        case["expected"]["first_diagnostic"] = dict(path="@fixture", line=2, column=3, code="E0327", message="error: E0327: rejected")
    if role == "KNOWN_LIMITATION":
        case["normative_oracle"] = "This remains a legal positive whose expected admission is not implemented"
    return case


def result(exit=0, stderr="", stdout=""):
    return dict(exit=exit, stderr=stderr, stdout=stdout, binary_sha256_before="same", binary_sha256_after="same")


class FrontendReceipts(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="spec frontend file identity ")
        self.addCleanup(directory.cleanup)
        self.fixture = Path(directory.name) / "fixture with a long name.xr"
        self.fixture.write_text("print(1)\n", encoding="utf-8")
        self.fixture.with_name("foreign.xr").write_bytes(self.fixture.read_bytes())

    def test_exact_first_diagnostic(self):
        fixture = self.fixture
        expected = f"{fixture}:2:3: error: E0327: rejected\n"
        self.assertEqual(gate.judge(receipt("TRUE_ERROR"), result(1, expected), fixture)[0], "TRUE_ERROR_PASS")
        for changed in (expected.replace(":2:3:", ":3:3:"), expected.replace(":2:3:", ":2:4:"),
                        expected.replace("E0327", "E0326"),
                        expected.replace("rejected", "different"), expected + expected,
                        expected.replace(str(fixture), str(fixture.with_name("foreign.xr")))):
            self.assertEqual(gate.judge(receipt("TRUE_ERROR"), result(1, changed), fixture)[0], "FIRST_DIAGNOSTIC_FAIL")

    def test_no_error_code_is_an_exact_fact(self):
        case = receipt("TRUE_ERROR");case["expected"]["first_diagnostic"].update(code=None, message="error: actual unnumbered diagnostic")
        fixture = self.fixture
        self.assertEqual(gate.judge(case, result(1, f"{fixture}:2:3: error: actual unnumbered diagnostic\n"), fixture)[0], "TRUE_ERROR_PASS")
        self.assertEqual(gate.judge(case, result(1, f"{fixture}:2:3: error: E0327: actual unnumbered diagnostic\n"), fixture)[0], "FIRST_DIAGNOSTIC_FAIL")

    def test_crash_timeout_and_binary_change_never_match(self):
        fixture = self.fixture;text = f"{fixture}:2:3: error: E0327: rejected\n"
        for exit in (-11, -1, 0xC0000005, 2):
            self.assertEqual(gate.judge(receipt("TRUE_ERROR"), result(exit, text), fixture)[0], "TOOL_FAIL")
        for extra in (dict(timeout_seconds=30), dict(tool_error="UTF8 error"), dict(binary_sha256_after="changed")):
            row = result(1, text);row.update(extra)
            self.assertEqual(gate.judge(receipt("TRUE_ERROR"), row, fixture)[0], "TOOL_FAIL")

    def test_limitation_has_no_normative_pass_and_improvement_requires_receipt(self):
        fixture = self.fixture;case = receipt("KNOWN_LIMITATION")
        self.assertEqual(gate.judge(case, result(1, f"{fixture}:2:3: error: E0327: rejected\n"), fixture)[0], "KNOWN_LIMITATION_STABLE")
        self.assertEqual(gate.judge(case, result(), fixture)[0], "LIMITATION_CHANGED")
        del case["normative_oracle"]
        with self.assertRaises(ValueError):gate.validate_case(case)

    def test_real_file_alias_preserves_exact_diagnostic(self):
        alias = self.fixture.with_name("another fixture name.xr")
        os.link(self.fixture, alias)
        for actual, expected in ((alias, self.fixture), (self.fixture, alias)):
            text = f"{actual}:2:3: error: E0327: rejected\n"
            status, first = gate.judge(receipt("TRUE_ERROR"), result(1, text), expected)
            self.assertEqual(status, "TRUE_ERROR_PASS")
            self.assertEqual(first, receipt("TRUE_ERROR")["expected"]["first_diagnostic"])
        foreign = self.fixture.with_name("foreign.xr")
        self.assertEqual(foreign.read_bytes(), self.fixture.read_bytes())
        text = f"{foreign}:2:3: error: E0327: rejected\n"
        self.assertEqual(gate.judge(receipt("TRUE_ERROR"), result(1, text), self.fixture)[0], "FIRST_DIAGNOSTIC_FAIL")

    def test_missing_file_cannot_be_the_expected_fixture(self):
        text = f"{self.fixture}:2:3: error: E0327: rejected\n"
        self.fixture.unlink()
        self.assertEqual(gate.judge(receipt("TRUE_ERROR"), result(1, text), self.fixture)[0], "FIRST_DIAGNOSTIC_FAIL")

    @unittest.skipUnless(sys.platform == "win32", "Windows short file names")
    def test_windows_short_path_preserves_exact_diagnostic(self):
        import ctypes
        from ctypes import wintypes
        short_path = ctypes.WinDLL("kernel32", use_last_error=True).GetShortPathNameW
        short_path.argtypes = (wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD)
        short_path.restype = wintypes.DWORD
        needed = short_path(str(self.fixture), None, 0)
        self.assertGreater(needed, 0, ctypes.get_last_error())
        buffer = ctypes.create_unicode_buffer(needed)
        written = short_path(str(self.fixture), buffer, needed)
        self.assertTrue(0 < written < needed, ctypes.get_last_error())
        alias = Path(buffer.value)
        if os.path.normcase(str(alias)) == os.path.normcase(str(self.fixture)):
            self.skipTest("The fixture volume does not expose an 8.3 alias")
        for actual, expected in ((alias, self.fixture), (self.fixture, alias)):
            text = f"{actual}:2:3: error: E0327: rejected\n"
            status, first = gate.judge(receipt("TRUE_ERROR"), result(1, text), expected)
            self.assertEqual(status, "TRUE_ERROR_PASS")
            self.assertEqual(first, receipt("TRUE_ERROR")["expected"]["first_diagnostic"])

    def test_only_user_approved_dependency_can_defer(self):
        case = receipt();case.update(role="DEFERRED", dependency="Task338", required_for_task342=False)
        case["source"] = dict(path="spec/source/sections/015-14-built-in-type-methods.md", fence_id="json-boundary-and-path")
        gate.validate_case(case)
        for exit in (0, 1):
            self.assertEqual(gate.judge(case, result(exit), Path("case.xr"))[0], "DEFERRED_UNACCEPTED")
        case["dependency"] = "other"
        with self.assertRaises(ValueError):gate.validate_case(case)
        case["dependency"] = "Task338";case["source"]["fence_id"] = "unrelated"
        with self.assertRaises(ValueError):gate.validate_case(case)

    def test_actual_mode_is_not_inferred_from_source_comments_or_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp);code = 'print("error[E9999]") // main() and error comments grant no mode'
            (root / "spec.md").write_text('```xray @id=example @run\n' + code + '\n```\n```output\nanything\n```\n', encoding="utf-8")
            case = receipt(code=code)
            with mock.patch.object(gate.g13, "classify", side_effect=AssertionError("must not invoke heuristic")):
                actual, origin = gate.resolve_case(root, case, {})
            self.assertEqual(actual, code);self.assertIn("@run", origin["normative_markers"])
            self.assertEqual(gate.judge(case, result(), Path("case.xr"))[0], "CHECK_PASS")

    def test_changed_body_and_ambiguous_source_fail_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp);path = root / "spec.md"
            path.write_text('```xray @id=example @check\nprint(2)\n```\n', encoding="utf-8")
            with self.assertRaises(ValueError):gate.resolve_case(root, receipt(), {})
            path.write_text(('```xray @id=example @check\nprint(1)\n```\n') * 2, encoding="utf-8")
            with self.assertRaises(ValueError):gate.resolve_case(root, receipt(), {})

    def test_preflight_failure_invokes_no_product(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp);manifest = root / "cases.json"
            (root / "spec.md").write_text('```xray @id=example @check\nprint(2)\n```\n', encoding="utf-8")
            case = receipt();case["receipts"] = [dict(receipt_id="protected")]
            manifest.write_text(json.dumps(dict(schema=1, fixed_source_commit="fixed", cases=[case], protected_receipts=dict(cn=["protected"]))), encoding="utf-8")
            with mock.patch.object(gate, "command_result", side_effect=AssertionError("must not invoke product")), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(gate.main(["--xray", "absent", "--manifest", str(manifest), "--language", "cn", "--root", str(root)]), 1)

    def test_protected_original_responsibility_cannot_be_deleted(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp);manifest = root / "cases.json"
            (root / "spec.md").write_text('```xray @id=example @check\nprint(1)\n```\n', encoding="utf-8")
            case = receipt();case["receipts"] = [dict(receipt_id="kept")]
            manifest.write_text(json.dumps(dict(schema=1, fixed_source_commit="fixed", cases=[case], protected_receipts=dict(cn=["kept", "lost"]))), encoding="utf-8")
            with mock.patch.object(gate, "command_result", side_effect=AssertionError("must not invoke partial scope")), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(gate.main(["--xray", "absent", "--manifest", str(manifest), "--language", "cn", "--root", str(root)]), 1)

    def test_missing_or_contradictory_oracle_is_not_accepted(self):
        for mutate in (lambda c:c.pop("mode"), lambda c:c["expected"].update(exit=1), lambda c:c.update(role="default")):
            case = receipt();mutate(case)
            with self.assertRaises(ValueError):gate.validate_case(case)
        case = receipt("TRUE_ERROR");case["expected"].pop("first_diagnostic")
        with self.assertRaises(ValueError):gate.validate_case(case)

    def test_shipped_manifest_retains_every_fixed_receipt_and_never_calls_limitation_reject(self):
        manifest = json.loads((ROOT / "tests/meta/spec_frontend_admission_cases.json").read_text(encoding="utf-8"))
        for case in manifest["cases"]:gate.validate_case(case)
        for language, expected in (("cn", 73), ("en", 67)):
            preserved = [origin for case in manifest["cases"] if case["language"] == language for origin in case["receipts"] if origin["kind"] == "FIXED_343_PROTECTED_FRONTEND"]
            self.assertEqual(len(preserved), expected)
        config = next(case for case in manifest["cases"] if case["role"] == "KNOWN_LIMITATION" and "nullable/default" in case["normative_oracle"])
        self.assertIn("legal", config["normative_oracle"])


if __name__ == "__main__":
    unittest.main()
