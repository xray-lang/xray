"""Regression tests for a smoke gate that must never hide a product failure."""
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import run_smoke as smoke


def result(code=0, stdout=b"1\n", stderr=b"", timed_out=False):
    return smoke.proc.ProcResult(("xray",), code, stdout, stderr, timed_out)


class VerdictTests(unittest.TestCase):
    def setUp(self):
        self.case = {"id": "example", "path": "example.xr", "mode": "run",
                     "expect": "pass", "exit_code": 0, "stdout": "one.txt"}

    def verdict(self, actual, diagnostic="", stage="run"):
        return smoke.classify(self.case, actual, diagnostic, b"1\n", stage)[0]

    def gap(self):
        self.case.update(expect="gap", exit_code=1,
                         gap={"feature": "F04", "first_diagnostic": "unsupported"})

    def test_pass_requires_exact_output_and_empty_stderr(self):
        self.assertEqual(self.verdict(result()), "pass")
        self.assertEqual(self.verdict(result(stdout=b"2\n")), "fail")
        self.assertEqual(self.verdict(result(stderr=b"warning\n")), "fail")

    def test_gap_success_requires_promotion_even_without_an_oracle(self):
        self.gap()
        self.assertEqual(self.verdict(result(stdout=b"new output\n")), "fail")

    def test_gap_requires_the_same_rejection(self):
        self.gap()
        self.assertEqual(self.verdict(result(1, b"", b"unsupported\n"), "unsupported"), "gap")
        self.assertEqual(self.verdict(result(1, b"", b"different\n"), "different"), "fail")
        self.assertEqual(self.verdict(result(1, b"leaked\n"), "unsupported"), "fail")

    def test_timeout_crash_and_missing_binary_are_not_feature_gaps(self):
        self.gap()
        for actual in (result(1, b"", timed_out=True), result(-11, b""),
                       result(3221225477, b""), result(127, b"")):
            with self.subTest(actual=actual):
                self.assertEqual(self.verdict(actual, "unsupported"), "fail")

    def test_native_gap_cannot_hide_a_runtime_failure(self):
        self.gap()
        self.case["mode"] = "build-run"
        actual = result(1, b"", b"unsupported\n")
        self.assertEqual(self.verdict(actual, "unsupported", "build"), "gap")
        self.assertEqual(self.verdict(actual, "unsupported", "run"), "fail")


class InventoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "one.txt").write_bytes(b"1\n")
        self.source = smoke.Source("demos/hello.xr", "demos", "run")
        self.sources = {self.source.path: self.source}
        self.case = {"id": "hello", "path": self.source.path, "mode": "run",
                     "expect": "pass", "exit_code": 0, "stdout": "one.txt"}
        self.native = dict(self.case, id="native-hello", mode="build-run", expect="gap", exit_code=1,
                           gap={"feature": "Task328 P2", "first_diagnostic": "unsupported"})

    def load(self, cases, sources=None):
        path = self.root / "cases.json"
        path.write_text(json.dumps({"schema": 1, "cases": cases}), encoding="utf-8")
        return smoke.load_cases(path, self.root, self.sources if sources is None else sources)

    def test_every_vm_pass_requires_native_companion(self):
        self.assertEqual(len(self.load([self.case, self.native])), 2)
        with self.assertRaisesRegex(ValueError, "native companions differ"):
            self.load([self.case])

    def test_removing_cases_or_duplicating_ids_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "unregistered sources"):
            self.load([])
        with self.assertRaisesRegex(ValueError, "duplicate case id"):
            self.load([self.case, self.case, self.native])

    def test_native_output_cannot_drift_from_vm(self):
        (self.root / "different.txt").write_bytes(b"2\n")
        self.native["stdout"] = "different.txt"
        with self.assertRaisesRegex(ValueError, "same stdout oracle"):
            self.load([self.case, self.native])

    def test_network_demo_cannot_be_run(self):
        network = smoke.Source(self.source.path, "demos", "check")
        with self.assertRaisesRegex(ValueError, "check-only"):
            self.load([self.case], {network.path: network})

    def test_deferral_is_limited_to_the_pending_native_path(self):
        self.native.update(exit_code=None, gap={"feature": "Task328 P2",
                           "first_diagnostic": None, "deferred": "pending native path"})
        self.assertEqual(len(self.load([self.case, self.native])), 2)
        with patch.object(smoke.proc, "run") as run:
            row = smoke.execute(self.native, self.source, self.root / "xray", self.root,
                                self.root, self.root, 1)
        run.assert_not_called()
        self.assertEqual(row["status"], "gap")
        self.assertFalse(row["executed"])
        self.case.update(expect="gap", exit_code=None, gap=dict(self.native["gap"]))
        with self.assertRaisesRegex(ValueError, "only the pending native path"):
            self.load([self.case, self.native])

    def test_discovery_keeps_both_readmes_and_multifile_modules(self):
        (self.root / "demos").mkdir()
        for name in ("README.md", "README_CN.md"):
            (self.root / name).write_text("```xray\nprint(1)\n```\n```xr\nprint(2)\n```\n", encoding="utf-8")
        for name in ("io", "_probe", "codegen", "json"):
            directory = self.root / "stdlib" / name
            directory.mkdir(parents=True)
            (directory / "other.xr").write_text("", encoding="utf-8")
            if name != "json":
                (directory / f"{name}.xr").write_text("", encoding="utf-8")
        discovered = smoke.discover(self.root)
        self.assertEqual(len(discovered), 5)
        self.assertEqual(discovered["stdlib/io/io.xr"].text, "import io\nprint(1)\n")
        self.assertIn("README_CN.md#xray-2", discovered)
        with self.assertRaisesRegex(ValueError, "unterminated"):
            smoke.readme_blocks("```xray\nprint(1)\n")

    def test_readme_fence_variants_cannot_escape_inventory(self):
        text = "   ~~~~xray\n   print(1)\n   ~~~~~\n````xr\nprint(2)\n````\n"
        self.assertEqual(smoke.readme_blocks(text), ["print(1)\n", "print(2)\n"])
        self.assertEqual(smoke.readme_blocks("````text\n```xray\nprint(3)\n```\n````\n"), [])
        (self.root / "demos").mkdir()
        (self.root / "stdlib").mkdir()
        (self.root / "README.md").write_text(text, encoding="utf-8")
        (self.root / "README_CN.md").write_text("", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "unregistered sources"):
            self.load([], smoke.discover(self.root))


class NativeExecutionTests(unittest.TestCase):
    def test_build_log_is_separate_and_missing_artifact_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "expected.txt").write_bytes(b"1\n")
            spec = smoke.Source("example.xr", "readme", "run", "print(1)\n")
            case = {"id": "native", "path": spec.path, "mode": "build-run", "expect": "pass",
                    "exit_code": 0, "stdout": "expected.txt"}
            def build(command, **kwargs):
                if "build" in command:
                    Path(command[-1]).write_bytes(b"fake executable")
                    return result(stdout=b"Generated: program\n")
                return result()
            with patch.object(smoke.proc, "run", side_effect=build):
                row = smoke.execute(case, spec, root / "xray", root, root, root, 1)
            self.assertEqual(row["status"], "pass")
            self.assertEqual((root / "build.stdout").read_bytes(), b"Generated: program\n")
            self.assertEqual((root / "run.stdout").read_bytes(), b"1\n")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "expected.txt").write_bytes(b"1\n")
            with patch.object(smoke.proc, "run", return_value=result(stdout=b"Generated\n")):
                row = smoke.execute(case, spec, root / "xray", root, root, root, 1)
            self.assertEqual(row["status"], "fail")
            self.assertIn("no executable", row["reason"])


if __name__ == "__main__":
    unittest.main()
