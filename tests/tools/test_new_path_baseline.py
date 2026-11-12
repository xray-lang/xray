"""Keep String feedback opt-in and preserve its exact products and outputs."""

import argparse
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location("new_path_baseline", ROOT / "scripts/new_path_baseline.py")
BASELINE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BASELINE)


class StringBaselineTests(unittest.TestCase):
    def test_default_benchmarks_exclude_string(self):
        self.assertEqual(BASELINE.DEFAULT_BENCHES,
                         ("fib(32)", "loop 10M", "loop 100M", "mandelbrot", "print(1)",
                          "import regex; print(1)"))
        self.assertEqual(BASELINE.BENCH_LABELS["string-append"], "String append")

    def test_workloads_append_and_print_length(self):
        self.assertIn('text+="x"', BASELINE.STRING_APPEND_XR)
        self.assertIn('text += "x"', BASELINE.STRING_APPEND_PY)
        self.assertIn('b"\\n"', BASELINE.STRING_APPEND_PY)
        self.assertIn("realloc(text->data, next)", BASELINE.STRING_APPEND_C)
        self.assertNotIn("noinline", BASELINE.STRING_APPEND_C)
        self.assertNotIn("volatile", BASELINE.STRING_APPEND_C)
        self.assertIn('printf("%zu\\n", text.length)', BASELINE.STRING_APPEND_C)

    def test_windows_reference_uses_optimized_static_crt(self):
        command = BASELINE.string_reference_command("clang-cl.exe", pathlib.Path("ref.c"), pathlib.Path("ref.exe"))
        for flag in ("/O2", "/Ob2", "/MT", "/std:c11", "/clang:-ffp-contract=off"):
            self.assertIn(flag, command)
        self.assertEqual(command[-2:], ["/Foref.obj", "/Feref.exe"])

    def test_timeout_retains_streams_after_drain(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = pathlib.Path(directory) / "timeout"
            with mock.patch("xray_process.run_child", side_effect=subprocess.TimeoutExpired(["test"], 30)):
                row = BASELINE.string_command(["test"], {}, prefix, 60)
            self.assertEqual(row["exit"], 124)
            self.assertEqual(row["limit_seconds"], 30)
            self.assertEqual(row["state"], "TERMINAL_DRAINED")
            self.assertTrue(pathlib.Path(row["stdout"]).is_file())
            self.assertTrue(pathlib.Path(row["stderr"]).is_file())

    def test_prepared_product_change_rejected_before_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            product = work / "product"
            product.write_bytes(b"original")
            prepared = work / "prepared.json"
            prepared.write_text(json.dumps({
                "script_sha256": hashlib.sha256(pathlib.Path(BASELINE.__file__).read_bytes()).hexdigest(),
                "artifacts": {str(product): hashlib.sha256(b"original").hexdigest()},
                "engines": {}, "environment": {}, "string_bench": {}}), encoding="utf-8")
            product.write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "input or product changed"):
                BASELINE.bench(argparse.Namespace(prepared_bench=prepared), ROOT)

    def test_wrong_lf_output_retains_failed_sample(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            output, errors = work / "out", work / "err"
            output.write_bytes(b"17\r\n")
            errors.write_bytes(b"")
            args = argparse.Namespace(prepared_bench=None, work=work, samples=1, timeout=30,
                                      string_bench={"expected_stdout": {"String append 17": b"17\n".hex()}})
            row = {"exit": 0, "stdout": str(output), "stderr": str(errors), "seconds": 0.1}
            with mock.patch.object(BASELINE, "bench_programs", return_value=(
                    {"String append 17": {"CPython": ["test"]}}, {})), \
                    mock.patch.object(BASELINE, "string_command", return_value=row):
                with self.assertRaises(SystemExit):
                    BASELINE.bench(args, ROOT)
            samples = json.loads((work / "string-samples.json").read_text(encoding="utf-8"))
            self.assertEqual(samples[0]["status"], "FAIL")
            self.assertEqual(output.read_bytes(), b"17\r\n")


if __name__ == "__main__":
    unittest.main()
