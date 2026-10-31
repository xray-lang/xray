"""Verify the original testing demo through its actual VM/native catalog."""

from __future__ import annotations

from dataclasses import dataclass, field
import hashlib
import json
from pathlib import Path
from typing import Callable


SOURCE = "demos/07-advanced/testing.xr"
REPORT_KEYS = {"schema", "source", "sourceSha256", "entries", "passed", "skipped", "failed"}
EXPECTED_KEYS = REPORT_KEYS - {"sourceSha256"} | {"sourceCanonicalSha256"}
ENTRY_KEYS = {"name", "function", "role", "timeout"}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_catalog(value: dict, *, expected: bool = False) -> dict:
    keys = EXPECTED_KEYS if expected else REPORT_KEYS
    hash_key = "sourceCanonicalSha256" if expected else "sourceSha256"
    if not isinstance(value, dict) or set(value) != keys or type(value["schema"]) is not int or value["schema"] != 1:
        raise ValueError("test suite requires the exact catalog schema 1")
    if value["source"] != SOURCE or not isinstance(value[hash_key], str) or len(value[hash_key]) != 64:
        raise ValueError("test suite source identity differs")
    if any(c not in "0123456789abcdef" for c in value[hash_key]):
        raise ValueError("test suite source hash is not canonical SHA256")
    entries = value["entries"]
    if not isinstance(entries, list) or len(entries) != 7:
        raise ValueError("test suite requires all seven catalog entries")
    names, functions = set(), set()
    for row in entries:
        if not isinstance(row, dict) or set(row) != (ENTRY_KEYS - {"function"} if expected else ENTRY_KEYS):
            raise ValueError("test suite entry shape differs")
        if not isinstance(row["name"], str) or not row["name"] or row["name"] in names:
            raise ValueError("test suite names must be distinct")
        if any(type(row[key]) is not int for key in (("role", "timeout") if expected else ("function", "role", "timeout"))):
            raise ValueError("test suite IDs, roles and timeouts must be integers")
        if row["role"] not in (1, 2) or row["timeout"] < 0:
            raise ValueError("test suite IDs, roles or timeouts are invalid")
        if not expected:
            if not 0 <= row["function"] <= 0xffffffff or row["function"] in functions:
                raise ValueError("test suite function IDs must be unique uint32 values")
            functions.add(row["function"])
        names.add(row["name"])
    if any(type(value[key]) is not int for key in ("passed", "skipped", "failed")) or (
            value["passed"], value["skipped"], value["failed"]) != (6, 1, 0):
        raise ValueError("test suite must execute six passing tests and skip exactly one")
    if sum(row["role"] == 1 for row in entries) != 6 or sum(row["role"] == 2 for row in entries) != 1:
        raise ValueError("test suite roles do not describe six tests and one skip")
    return value


def expected_catalog(case: dict, root: Path) -> dict | None:
    suite = case.get("suite")
    if suite is None:
        if case["path"] == SOURCE and case["expect"] == "pass":
            raise ValueError("testing demo cannot pass through its initializer without a suite")
        return None
    if case["path"] != SOURCE or case["mode"] not in ("run", "build-run") or not isinstance(suite, dict) or set(suite) != {"schema", "catalog"} or type(suite["schema"]) is not int or suite["schema"] != 1:
        raise ValueError("suite mode is restricted to the original testing demo")
    path = (root / suite["catalog"]).resolve()
    if not path.is_relative_to(root.resolve()) or not path.is_file():
        raise ValueError("test suite catalog is missing or outside the source root")
    expected = validate_catalog(json.loads(path.read_text(encoding="utf-8")), expected=True)
    source = (root / SOURCE).read_bytes()
    if hashlib.sha256(source.replace(b"\r\n", b"\n")).hexdigest() != expected["sourceCanonicalSha256"]:
        raise ValueError("test suite original source bytes differ from the independent catalog")
    del expected["sourceCanonicalSha256"]
    expected["sourceSha256"] = hashlib.sha256(source).hexdigest()
    return expected


@dataclass
class SuiteCommands:
    vm: Path | None = None
    native: Path | None = None
    generated_c: Path | None = None
    verified_vm: dict[str, tuple[dict, str]] = field(default_factory=dict)

    def execute(self, case: dict, root: Path, logs: Path, invoke: Callable) -> dict:
        expected = expected_catalog(case, root)
        if expected is None:
            raise ValueError("test suite is not configured")
        engine = "vm" if case["mode"] == "run" else "native"
        driver = self.vm if engine == "vm" else self.native
        if driver is None or not driver.is_file():
            raise ValueError(f"suite-{engine} requires its real catalog executable")
        report = logs / f"suite-{engine}.json"
        emitted = logs / "suite.c"
        if report.exists() or (engine == "vm" and emitted.exists()):
            raise ValueError("test suite output must be produced by this invocation")
        arguments = [driver, root, root / "stdlib", emitted, report] if engine == "vm" else [driver, report]
        if engine == "native":
            previous = self.verified_vm.get(SOURCE)
            if previous is None:
                raise ValueError("native suite requires the verified VM catalog")
            if self.generated_c is None or not self.generated_c.is_file() or digest(self.generated_c) != previous[1]:
                raise ValueError("native suite was not compiled from the same emitted SourceProduct C")
        result = invoke("suite-" + engine, arguments)
        if result.timed_out:
            raise ValueError(f"suite-{engine} timed out")
        if not result.ok or result.stderr or not report.is_file():
            raise ValueError(f"suite-{engine} did not execute and publish its successful catalog")
        actual = validate_catalog(json.loads(report.read_text(encoding="utf-8")))
        logical = dict(actual, entries=[{key: value for key, value in row.items() if key != "function"}
                                       for row in actual["entries"]])
        if logical != expected:
            raise ValueError(f"suite-{engine} names, roles, timeouts or source differ")
        prefix = "VM" if engine == "vm" else "native"
        lines = [f"{prefix} {row['name']} PASS" for row in expected["entries"] if row["role"] == 1]
        lines.append("original catalog: 6 PASS, 1 SKIP")
        if result.stdout.decode("utf-8").splitlines() != lines:
            raise ValueError(f"suite-{engine} did not report each executed test")
        if engine == "vm":
            if not emitted.is_file() or not emitted.stat().st_size:
                raise ValueError("VM suite did not emit its SourceProduct C")
            generated = digest(emitted)
            self.verified_vm[SOURCE] = (actual, generated)
        else:
            previous = self.verified_vm[SOURCE]
            if actual != previous[0]:
                raise ValueError("VM/native suite catalogs differ")
            generated = previous[1]
        return {"status": "pass", "engine": engine, "executed": True,
                "catalog": actual, "report": str(report), "report_sha256": digest(report),
                "generated_c_sha256": generated, "matches_vm": engine == "native"}
