"""Reject incomplete crypto catalog evidence without running a compiler."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import xray_crypto_suite as crypto

ROOT = Path(__file__).resolve().parents[2]


def complete_catalog():
    return {"schema": 1, "suites": [
        {"source": source, "sourceSha256": "1" * 64, "entries": [
            {"name": name, "function": i, "role": 1, "timeout": 0,
             "executed": True, "passed": True} for i, name in enumerate(names)]}
        for source, names in crypto.ORIGINAL],
        "discovered": 67, "executed": 67, "passed": 67, "skipped": 0, "failed": 0}


class CryptoCatalogTests(unittest.TestCase):
    def test_complete_catalog_and_program_local_ids(self):
        # The same integer may identify different tests in distinct Programs.
        self.assertEqual(crypto.validate_catalog(complete_catalog())["passed"], 67)

    def test_requires_every_original_source_and_name(self):
        for key in ("suites", "name", "role", "timeout", "duplicate", "function", "extra"):
            value = complete_catalog()
            row = value["suites"][0]["entries"][0]
            if key == "suites":
                value["suites"].pop()
            elif key == "duplicate":
                value["suites"][0]["entries"][1]["function"] = row["function"]
            elif key == "extra":
                row["unexpected"] = 0
            else:
                row[key] = {"name": "substitute", "role": 2, "timeout": 1, "function": True}[key]
            with self.subTest(key=key), self.assertRaises(ValueError):
                crypto.validate_catalog(value)

    def test_counts_and_actual_execution_are_strict(self):
        for key, replacement in (("discovered", 66), ("executed", 0), ("passed", True),
                                 ("skipped", 67), ("failed", 1)):
            value = complete_catalog()
            value[key] = replacement
            with self.subTest(key=key), self.assertRaises(ValueError):
                crypto.validate_catalog(value)
        for key in ("executed", "passed"):
            value = complete_catalog()
            value["suites"][6]["entries"][6][key] = 1
            with self.subTest(key=key), self.assertRaises(ValueError):
                crypto.validate_catalog(value)

    def test_expected_catalog_binds_current_whole_sources(self):
        case = {"path": crypto.SOURCE, "mode": "run", "expect": "pass",
                "crypto_suite": {"schema": 1, "catalog": "tests/smoke/expected/crypto-catalog.json"}}
        self.assertEqual(len(crypto.expected_catalog(case, ROOT)["suites"]), 7)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = [source for source, _ in crypto.ORIGINAL] + [case["crypto_suite"]["catalog"]]
            for name in files:
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((ROOT / name).read_bytes())
            changed = root / crypto.ORIGINAL[0][0]
            changed.write_bytes(changed.read_bytes() + b"\n// changed input\n")
            with self.assertRaises(ValueError):
                crypto.expected_catalog(case, root)

    def test_progress_needs_actual_child_and_all_c_projections(self):
        case = {"path": crypto.SOURCE, "mode": "run", "crypto_suite": {"schema": 1}}
        row = {"executed": True, "commands": [{"stage": "crypto-suite-vm", "exit_code": 0, "timed_out": False}],
               "crypto_suite": {"status": "pass", "engine": "vm", "executed": True,
                                "catalog": complete_catalog(),
                                "generated_c_sha256": {f"crypto_suite_{i}.c": "a" * 64 for i in range(7)}}}
        self.assertTrue(crypto.passed(case, row))
        for key in ("commands", "executed", "hashes", "catalog", "native"):
            broken = deepcopy(row)
            if key == "commands":
                broken["commands"] = []
            elif key == "executed":
                broken["crypto_suite"]["executed"] = False
            elif key == "hashes":
                broken["crypto_suite"]["generated_c_sha256"].pop("crypto_suite_6.c")
            elif key == "catalog":
                broken["crypto_suite"]["catalog"]["suites"].pop()
            else:
                broken["crypto_suite"]["engine"] = "native"
            with self.subTest(key=key):
                self.assertFalse(crypto.passed(case, broken))

    def test_native_rejects_mismatched_compiled_c_before_execution(self):
        case = {"path": crypto.SOURCE, "mode": "build-run", "expect": "pass",
                "crypto_suite": {"schema": 1, "catalog": "tests/smoke/expected/crypto-catalog.json"}}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            driver = root / "native.exe"
            driver.write_bytes(b"unit protocol placeholder, never executed")
            compiled = root / "compiled"
            compiled.mkdir()
            hashes = {}
            for i in range(7):
                file = compiled / f"crypto_suite_{i}.c"
                file.write_bytes(b"same SourceProduct C")
                hashes[file.name] = hashlib.sha256(file.read_bytes()).hexdigest()
            (compiled / "crypto_suite_6.c").write_bytes(b"different product")
            commands = crypto.CryptoCommands(native=driver, generated_dir=compiled,
                                             verified_vm=(complete_catalog(), hashes))
            calls = []
            with self.assertRaises(ValueError):
                commands.execute(case, ROOT, root, lambda *args: calls.append(args))
            self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
