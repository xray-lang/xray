"""Keep String feedback opt-in and preserve its exact products and outputs."""

import argparse
import hashlib
import importlib.util
import json
import pathlib
import statistics
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


GATE_BACKENDS = (("Xray VM", "CPython", 1.0), ("Xray native", "C -O2", 1.3))
GATE_FORMS = ("", *(f" {form}" for form, _, _ in BASELINE.STRING_APPEND_FORMS))


def paired_samples(count, engine, reference, actual_times, reference_times):
    records = []
    for sample, (actual, control) in enumerate(zip(actual_times, reference_times, strict=True)):
        pair = [(engine, actual), (reference, control)]
        for measured, seconds in reversed(pair) if sample % 2 else pair:
            records.append({"label": f"String append {count}", "engine": measured,
                            "sample": sample, "pair_engine": engine, "seconds": seconds, "status": "PASS"})
    return records


def passing_samples(blocks):
    records = []
    for count in BASELINE.STRING_SIZES:
        for backend, reference, limit in GATE_BACKENDS:
            for form in GATE_FORMS:
                records.extend(paired_samples(count, backend + form, reference,
                                              [10 * limit] * (2 * blocks), [10.0] * (2 * blocks)))
    return records


def gate_row(result, count, engine):
    return next(row for row in result["rows"] if (row["count"], row["engine"]) == (count, engine))


def benchmark_engines(counts):
    names = ["CPython", "C -O2", *(backend + form for backend, _, _ in GATE_BACKENDS for form in GATE_FORMS)]
    return {f"String append {count}": {name: [name, str(count)] for name in names} for count in counts}


class StringBaselineTests(unittest.TestCase):
    def test_default_benchmarks_exclude_string(self):
        self.assertEqual(BASELINE.DEFAULT_BENCHES,
                         ("fib(32)", "loop 10M", "loop 100M", "mandelbrot", "print(1)",
                          "import regex; print(1)"))
        self.assertEqual(BASELINE.BENCH_LABELS["string-append"], "String append")

    def test_workloads_append_and_print_length(self):
        self.assertIn('text+="x"', BASELINE.STRING_APPEND_XR)
        self.assertIn('text=text+"x"', BASELINE.STRING_APPEND_ASSIGN_XR)
        self.assertEqual(BASELINE.STRING_APPEND_XR,
                         BASELINE.STRING_APPEND_ASSIGN_XR.replace('text=text+"x"', 'text+="x"'))
        self.assertIn('text += "x"', BASELINE.STRING_APPEND_PY)
        self.assertIn("for _ in range(count):", BASELINE.STRING_APPEND_PY)
        self.assertNotIn("while", BASELINE.STRING_APPEND_PY)
        self.assertIn('b"\\n"', BASELINE.STRING_APPEND_PY)
        self.assertIn("realloc(text->data, next)", BASELINE.STRING_APPEND_C)
        self.assertNotIn("noinline", BASELINE.STRING_APPEND_C)
        self.assertNotIn("volatile", BASELINE.STRING_APPEND_C)
        self.assertIn('printf("%zu\\n", text.length)', BASELINE.STRING_APPEND_C)

    def test_assignment_ratios_compare_each_backend_to_its_compound_form(self):
        medians = {
            "String append 1": {"Xray VM": 4, "CPython": 2},
            "String append 17": {"Xray VM": 4, "CPython": 2, "Xray VM assignment": 6,
                                 "Xray native": 2, "C -O2": 1, "Xray native assignment": 1},
        }
        self.assertEqual(BASELINE.string_ratios(medians), {
            "String append 1": {"vm_to_cpython": 2},
            "String append 17": {"vm_to_cpython": 2, "native_to_c": 2,
                                 "assignment_vm_to_compound": 1.5, "assignment_native_to_compound": 0.5},
        })

    def test_string_gate_runs_adjacent_pairs_in_both_orders(self):
        engines = benchmark_engines((1, 1_000_000, *BASELINE.STRING_SIZES))
        controls = {backend + form: control for backend, control, _ in GATE_BACKENDS for form in GATE_FORMS}
        jobs = list(BASELINE.string_gate_jobs(engines, 2))
        self.assertEqual(len(jobs), len(engines) * len(controls) * 8)
        samples = {(label, pair): [] for label in engines for pair in controls}
        for start in range(0, len(jobs), 4):
            block = jobs[start:start + 4]
            label, _, _, sample, pair = block[0]
            control = controls[pair]
            self.assertEqual(sample, len(samples[label, pair]))
            self.assertEqual(block, [
                (label, pair, engines[label][pair], sample, pair),
                (label, control, engines[label][control], sample, pair),
                (label, control, engines[label][control], sample + 1, pair),
                (label, pair, engines[label][pair], sample + 1, pair),
            ])
            samples[label, pair].extend((sample, sample + 1))
        for key, observed in samples.items():
            with self.subTest(pair=key):
                self.assertEqual(observed, [0, 1, 2, 3])

    def test_balanced_block_median_rejects_independent_timing_bias(self):
        count = BASELINE.STRING_SIZES[0]
        engine = "Xray native local const"
        actual = [12.0, 13.0, 10.0, 10.0, 6.0, 8.0]
        control = [1.0, 19.0, 2.0, 18.0, 4.0, 4.0]
        old_actual = statistics.quantiles(actual, n=4, method="inclusive")[0]
        old_control = statistics.quantiles(control, n=4, method="inclusive")[0]
        self.assertGreater(old_actual / old_control, 1.3)
        records = [r for r in passing_samples(3) if (r["label"], r["pair_engine"]) !=
                   (f"String append {count}", engine)]
        records.extend(paired_samples(count, engine, "C -O2", actual, control))
        result = BASELINE.string_performance_gate(records, 3)
        self.assertEqual(result["status"], "PASS")
        row = gate_row(result, count, engine)
        self.assertEqual(row["balanced_block_ratios"], [1.25, 1.0, 1.75])
        self.assertEqual(row["ratio"], 1.25)

    def test_string_gate_checks_each_form_against_its_own_reference(self):
        count = BASELINE.STRING_SIZES[0]
        label = f"String append {count}"
        engine = "Xray native local const"
        records = passing_samples(2)
        for row in records:
            if (row["label"], row["pair_engine"]) == (label, engine):
                row["seconds"] = 26.0 if row["engine"] == engine else 20.0
        records.extend(paired_samples(1_000_000, engine, "C -O2", [20.0] * 4, [1.0] * 4))
        result = BASELINE.string_performance_gate(records, 2)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(len(result["rows"]), 36)
        limits = {(count, backend + form): (reference, limit) for count in BASELINE.STRING_SIZES
                  for backend, reference, limit in GATE_BACKENDS for form in GATE_FORMS}
        self.assertEqual({(r["count"], r["engine"]): (r["reference"], r["limit"]) for r in result["rows"]}, limits)
        for row in result["rows"]:
            self.assertEqual(row["ratio"], row["limit"])
            self.assertEqual(row["balanced_block_ratios"], [row["limit"]] * 2)

    def test_string_gate_still_rejects_persistent_regressions(self):
        count = BASELINE.STRING_SIZES[0]
        label = f"String append {count}"
        records = passing_samples(3)
        slow = {"Xray VM local const": 12.0, "Xray native grouped": 14.0}
        for row in records:
            if row["label"] == label and row["engine"] in slow:
                row["seconds"] = slow[row["engine"]]
        with mock.patch("builtins.print"):
            result = BASELINE.string_performance_gate(records, 3)
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual({(row["count"], row["engine"]): row["ratio"] for row in result["rows"]
                          if row["status"] == "FAIL"},
                         {(count, "Xray VM local const"): 1.2, (count, "Xray native grouped"): 1.4})

    def test_string_gate_requires_complete_valid_samples(self):
        count = BASELINE.STRING_SIZES[0]
        engine = "Xray native local const"
        records = passing_samples(2)
        index = next(i for i, row in enumerate(records) if row["label"] == f"String append {count}"
                     and row["pair_engine"] == engine and row["engine"] == "C -O2" and row["sample"] == 0)
        reference = records[index]
        others = records[:index] + records[index + 1:]
        overflow = [dict(row, seconds=1e308) if row["label"] == f"String append {count}"
                    and row["engine"] == engine and row["sample"] < 2 else row for row in records]
        failures = {"missing": others, "duplicate": [*records, reference.copy()],
                    "out of range": [*others, dict(reference, sample=4)],
                    "wrong reference": [*others, dict(reference, engine="CPython")],
                    "failed": [*others, dict(reference, status="FAIL")],
                    "missing duration": [*others, dict(reference, seconds=None)],
                    "overflow": overflow,
                    "zero": [*others, dict(reference, seconds=0.0)],
                    "negative": [*others, dict(reference, seconds=-1.0)],
                    "infinite": [*others, dict(reference, seconds=float("inf"))],
                    "nan": [*others, dict(reference, seconds=float("nan"))]}
        for invalid, samples in failures.items():
            with self.subTest(invalid=invalid), mock.patch("builtins.print"):
                result = BASELINE.string_performance_gate(samples, 2)
                self.assertEqual(result["status"], "FAIL")
                row = gate_row(result, count, engine)
                self.assertEqual(row["status"], "FAIL")
                self.assertIsNone(row["ratio"])
                self.assertEqual(gate_row(result, count, "Xray native")["status"], "PASS")

    def test_gate_bench_retains_the_pair_identity_for_every_invocation(self):
        count = BASELINE.STRING_SIZES[0]
        label = f"String append {count}"
        engines = benchmark_engines((count,))
        jobs = list(BASELINE.string_gate_jobs(engines, 2))
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            output, errors = work / "program.stdout", work / "program.stderr"
            output.write_bytes(f"{count}\n".encode())
            errors.write_bytes(b"")
            args = argparse.Namespace(prepared_bench=None, work=work, samples=2, timeout=30, gate_string=True,
                                      string_bench={"expected_stdout": {label: output.read_bytes().hex()}, "artifacts": {}})
            row = {"exit": 0, "stdout": str(output), "stderr": str(errors), "seconds": 1.0}
            with mock.patch.object(BASELINE, "bench_programs", return_value=(engines, {})), \
                    mock.patch.object(BASELINE, "string_command", side_effect=lambda *_: row.copy()) as command, \
                    mock.patch("builtins.print"):
                BASELINE.bench(args, ROOT)
            records = json.loads((work / "string-samples.json").read_text(encoding="utf-8"))
            receipts = (work / "string-samples.jsonl").read_text(encoding="utf-8").splitlines()
            self.assertEqual([json.loads(line) for line in receipts], records)
            self.assertEqual([call.args[0] for call in command.call_args_list], [job[2] for job in jobs])
            self.assertEqual(len({call.args[2] for call in command.call_args_list}), len(jobs))
            self.assertEqual([(r["label"], r["engine"], r["sample"], r["pair_engine"]) for r in records],
                             [(label, engine, sample, pair) for label, engine, _, sample, pair in jobs])
            self.assertTrue(all(row["status"] == "PASS" for row in records))

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
