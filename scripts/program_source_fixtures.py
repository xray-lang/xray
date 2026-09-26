#!/usr/bin/env python3
"""Own source-test registration and publish active native fixtures."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "tests/unit/program/xr_program_source_cases.json"
SOURCE = ROOT / "tests/unit/program/test_xr_program_source_build.c"
CORE_SPEC_REGISTRY = ROOT / "xisa/core/registry.json"
IDENTIFIER = re.compile(r"[a-z][a-z0-9_]*\Z")
LABEL = re.compile(r"[a-z][a-z0-9-]*\Z")
OPERATION = re.compile(r"core\.[a-z][a-z0-9_.]*\Z")


class FixtureError(ValueError):
    pass


class Once(argparse.Action):
    def __call__(self, parser, namespace, value, option_string=None):
        if getattr(namespace, self.dest, None) is not None:
            parser.error(f"duplicate argument: {option_string}")
        setattr(namespace, self.dest, value)


def _unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise FixtureError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_core_operation_coverage(path: Path = CORE_SPEC_REGISTRY) -> dict[str, dict[str, str]]:
    payload = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object)
    if (not isinstance(payload, dict) or not isinstance(payload.get("operations"), list) or
            not payload["operations"]):
        raise FixtureError("CoreSpec registry requires a non-empty operations array")
    result: dict[str, dict[str, str]] = {}
    for operation in payload["operations"]:
        if not isinstance(operation, dict):
            raise FixtureError("CoreSpec operation record must be an object")
        spelling = operation.get("spelling")
        coverage = operation.get("coverage")
        if (not isinstance(spelling, str) or not OPERATION.fullmatch(spelling) or
                spelling in result or not isinstance(coverage, dict)):
            raise FixtureError(f"invalid or duplicate CoreSpec operation: {spelling!r}")
        backend_statuses: dict[str, str] = {}
        for backend in ("vm", "aot"):
            row = coverage.get(backend)
            status = row.get("status") if isinstance(row, dict) else None
            if not isinstance(status, str):
                raise FixtureError(
                    f"CoreSpec operation {spelling} lacks {backend} coverage status")
            backend_statuses[backend] = status
        result[spelling] = backend_statuses
    return result


def validate_registry(payload: object, source: str,
                      core_operations: dict[str, dict[str, str]] | None = None) -> dict:
    if core_operations is None:
        core_operations = load_core_operation_coverage()
    if not isinstance(payload, dict) or set(payload) != {"schema", "cases"}:
        raise FixtureError("registry requires exactly schema and cases")
    if type(payload["schema"]) is not int or payload["schema"] != 2:
        raise FixtureError("unsupported source fixture registry schema")
    cases = payload["cases"]
    if not isinstance(cases, list) or not cases:
        raise FixtureError("source case registry must not be empty")
    names: set[str] = set()
    fixtures: set[str] = set()
    for case in cases:
        if not isinstance(case, dict) or set(case) != {"name", "fixture"}:
            raise FixtureError("case requires exactly name and fixture")
        name = case["name"]
        if not isinstance(name, str) or not IDENTIFIER.fullmatch(name) or name in names:
            raise FixtureError(f"invalid or duplicate source case: {name!r}")
        names.add(name)
        fixture = case["fixture"]
        if fixture is None:
            continue
        if (not isinstance(fixture, dict) or
                not {"id", "expected_exit", "labels", "backends"} <= set(fixture) or
                not set(fixture) <= {"id", "expected_exit", "labels", "backends",
                                     "expected_stdout_hex", "expected_stderr_hex"}):
            raise FixtureError(f"invalid fixture record for {name}")
        stdout_hex = fixture.get("expected_stdout_hex", "")
        if (not isinstance(stdout_hex, str) or len(stdout_hex) % 2 != 0 or
                not re.fullmatch(r"[0-9a-f]*", stdout_hex)):
            raise FixtureError(f"fixture {name} requires lowercase even-length stdout hex")
        stderr_hex = fixture.get("expected_stderr_hex", "")
        if (not isinstance(stderr_hex, str) or len(stderr_hex) % 2 != 0 or
                not re.fullmatch(r"[0-9a-f]*", stderr_hex)):
            raise FixtureError(f"fixture {name} requires lowercase even-length stderr hex")
        ident = fixture["id"]
        if (not isinstance(ident, str) or not IDENTIFIER.fullmatch(ident)
                or ident == "none" or ident in fixtures):
            raise FixtureError(f"invalid or duplicate fixture id: {ident!r}")
        fixtures.add(ident)
        expected = fixture["expected_exit"]
        if type(expected) is not int or not 0 <= expected <= 255:
            raise FixtureError(f"fixture {ident} requires an exact process exit byte")
        labels = fixture["labels"]
        if (not isinstance(labels, list) or not labels
                or any(not isinstance(label, str) or not LABEL.fullmatch(label) for label in labels)
                or len(labels) != len(set(labels))):
            raise FixtureError(f"fixture {ident} has invalid or duplicate labels")
        backends = fixture["backends"]
        if not isinstance(backends, dict) or set(backends) != {"vm", "aot"}:
            raise FixtureError(f"fixture {ident} requires exact VM and AOT expectations")
        for backend, expectation in backends.items():
            if expectation == "execute":
                continue
            if (not isinstance(expectation, dict) or
                    set(expectation) != {"status", "operation"} or
                    expectation["status"] != "unsupported" or
                    not isinstance(expectation["operation"], str) or
                    not OPERATION.fullmatch(expectation["operation"])):
                raise FixtureError(f"fixture {ident} has invalid {backend} expectation")
            operation = expectation["operation"]
            if operation not in core_operations:
                raise FixtureError(
                    f"fixture {ident} names unknown CoreSpec operation {operation}")
            if core_operations[operation].get(backend) != "NOT_YET_ACTIVE":
                raise FixtureError(
                    f"fixture {ident} requires {operation} to be NOT_YET_ACTIVE for {backend}")
        if backends["vm"] != backends["aot"]:
            raise FixtureError(
                f"fixture {ident} requires symmetric VM and AOT expectations")
    declared = re.findall(r"^\s*TEST\s*\(\s*([a-z][a-z0-9_]*)\s*\)\s*\{",
                          source, re.MULTILINE)
    if len(declared) != len(set(declared)) or set(declared) != names:
        raise FixtureError("manifest source cases differ from the complete TEST declaration census")
    bodies = dict(re.findall(r"^\s*TEST\s*\(\s*([a-z][a-z0-9_]*)\s*\)\s*\{(.*?)^\}",
                             source, re.MULTILINE | re.DOTALL))
    if set(bodies) != names:
        raise FixtureError("source cases require complete, column-zero-delimited test bodies")
    for case in cases:
        bound = set(re.findall(r"\bXR_SOURCE_FIXTURE_([A-Z][A-Z0-9_]*)\b", bodies[case["name"]]))
        expected = {case["fixture"]["id"].upper()} if case["fixture"] else set()
        if bound != expected:
            raise FixtureError(f'manifest fixture differs from source case binding: {case["name"]}')
    referenced = set(re.findall(r"\bXR_SOURCE_FIXTURE_([A-Z][A-Z0-9_]*)\b", source)) - {"NONE"}
    if referenced != {ident.upper() for ident in fixtures}:
        raise FixtureError("manifest fixtures differ from source fixture references")
    return payload


def load_registry(manifest: Path = MANIFEST, source: Path = SOURCE) -> dict:
    payload = json.loads(manifest.read_text(encoding="utf-8"), object_pairs_hook=_unique_object)
    return validate_registry(payload, source.read_text(encoding="utf-8"))


def registry_identity(registry: dict) -> str:
    content = json.dumps(registry, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(content).hexdigest()


def fixture_records(registry: dict) -> list[dict]:
    return [case["fixture"] for case in registry["cases"] if case["fixture"] is not None]


def native_fixture_records(registry: dict) -> list[dict]:
    return [fixture for fixture in fixture_records(registry)
            if fixture["backends"]["aot"] == "execute"]


def pending_fixture_cases(registry: dict) -> list[tuple[dict, dict]]:
    return [(case, case["fixture"]) for case in registry["cases"]
            if case["fixture"] is not None and
            any(expectation != "execute"
                for expectation in case["fixture"]["backends"].values())]


def native_target_names(registry: dict) -> tuple[str, ...]:
    return tuple(f'test_xr_program_{fixture["id"]}_aot_native'
                 for fixture in native_fixture_records(registry))


def pending_test_names(registry: dict) -> tuple[str, ...]:
    return tuple(f'test_xr_program_{fixture["id"]}_backend_pending'
                 for _, fixture in pending_fixture_cases(registry))


def operation_constant(expectation: object) -> str:
    if expectation == "execute":
        return "0u"
    assert isinstance(expectation, dict)
    return "XR_CORE_OP_" + expectation["operation"].replace(".", "_").upper()


def project_registry(registry: dict) -> tuple[bytes, bytes]:
    fixtures = fixture_records(registry)
    native_fixtures = native_fixture_records(registry)
    header = ["/* Generated source-test registry. Do not edit. */",
              "#ifndef XR_PROGRAM_SOURCE_CASES_GEN_H", "#define XR_PROGRAM_SOURCE_CASES_GEN_H",
              "#include <stdint.h>",
              f'#define XR_SOURCE_REGISTRY_ID "{registry_identity(registry)}"',
              f'#define XR_SOURCE_CASE_COUNT {len(registry["cases"])}u',
              "typedef enum XrSourceFixtureId {", "    XR_SOURCE_FIXTURE_NONE = 0,"]
    for fixture in fixtures:
        header.append(f'    XR_SOURCE_FIXTURE_{fixture["id"].upper()},')
    header.extend(["} XrSourceFixtureId;", "",
                   "typedef enum XrSourceBackendKind {",
                   "    XR_SOURCE_BACKEND_VM = 0,",
                   "    XR_SOURCE_BACKEND_AOT,",
                   "} XrSourceBackendKind;", "",
                   "static uint16_t xr_source_fixture_unsupported_operation(",
                   "    XrSourceFixtureId fixture, XrSourceBackendKind backend) {",
                   "    switch (fixture) {"])
    for fixture in fixtures:
        vm_operation = operation_constant(fixture["backends"]["vm"])
        aot_operation = operation_constant(fixture["backends"]["aot"])
        header.extend([
            f'        case XR_SOURCE_FIXTURE_{fixture["id"].upper()}:',
            f"            return backend == XR_SOURCE_BACKEND_VM ? {vm_operation} : {aot_operation};",
        ])
    header.extend(["        case XR_SOURCE_FIXTURE_NONE:",
                   "        default:",
                   "            return 0u;",
                   "    }",
                   "}", "",
                   "#define XR_SOURCE_FIXTURES(X)" + (" \\" if native_fixtures else "")])
    for index, fixture in enumerate(native_fixtures):
        suffix = " \\" if index + 1 < len(native_fixtures) else ""
        header.append(f'    X({fixture["id"]}, XR_SOURCE_FIXTURE_{fixture["id"].upper()}){suffix}')
    header.append("#define XR_SOURCE_CASES(X) \\")
    for index, case in enumerate(registry["cases"]):
        fixture = case["fixture"]
        enum = "XR_SOURCE_FIXTURE_" + (fixture["id"].upper() if fixture else "NONE")
        suffix = " \\" if index + 1 < len(registry["cases"]) else ""
        header.append(f'    X({case["name"]}, {enum}){suffix}')
    header.extend(["#endif", ""])
    cmake = ["# Generated source-native fixture registration. Do not edit."]
    for fixture, target in zip(native_fixtures, native_target_names(registry)):
        labels = ";".join(fixture["labels"])
        stdout_hex = fixture.get("expected_stdout_hex", "")
        stderr_hex = fixture.get("expected_stderr_hex", "")
        cmake.append(f'add_xr_program_source_native_fixture({fixture["id"]} '
                     f'{target} {fixture["expected_exit"]} "{labels}" "{stdout_hex}" "{stderr_hex}")')
    for (case, fixture), test_name in zip(pending_fixture_cases(registry),
                                          pending_test_names(registry)):
        labels = ";".join(fixture["labels"])
        cmake.append(f'add_xr_program_source_pending_fixture({fixture["id"]} '
                     f'{case["name"]} {test_name} "{labels}")')
    cmake.append("")
    return "\n".join(header).encode("utf-8"), "\n".join(cmake).encode("utf-8")


def _read_regular_utf8(path: Path, *, missing_ok: bool = False) -> bytes | None:
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        if missing_ok:
            return None
        raise
    if not stat.S_ISREG(metadata.st_mode) or path.is_symlink():
        raise FixtureError(f"not a regular generated file: {path}")
    content = path.read_bytes()
    content.decode("utf-8", errors="strict")
    return content


def publish_staged(staged: Path, output: Path) -> bool:
    content = _read_regular_utf8(staged)
    if not content:
        raise FixtureError("fixture producer returned no generated C")
    previous = _read_regular_utf8(output, missing_ok=True)
    if previous == content:
        staged.unlink()
        return False
    with staged.open("r+b") as stream:
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(staged, output)
    return True


def check_projection(registry: dict, header: Path, cmake: Path) -> None:
    for path, expected in zip((header, cmake), project_registry(registry)):
        if _read_regular_utf8(path) != expected:
            raise FixtureError(f"source fixture projection is stale or mismatched: {path}")


def write_stable(output: Path, content: bytes) -> bool:
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix=output.name + ".tmp.", dir=output.parent)
    staged = Path(name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(content)
        return publish_staged(staged, output)
    finally:
        staged.unlink(missing_ok=True)


def generate_fixture(registry: dict, fixture_id: str, producer: Path, output: Path) -> bool:
    if fixture_id not in {fixture["id"] for fixture in native_fixture_records(registry)}:
        raise FixtureError(f"unknown fixture id: {fixture_id}")
    if not producer.is_file():
        raise FixtureError(f"fixture producer does not exist: {producer}")
    output = output.absolute()
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix=output.name + ".tmp.", dir=output.parent)
    os.close(descriptor)
    staged = Path(name)
    try:
        command = [str(producer.absolute()), "--emit-fixture", fixture_id,
                   "--registry", registry_identity(registry), "--output", str(staged)]
        result = subprocess.run(command, cwd=output.parent, check=False)
        if result.returncode:
            raise FixtureError(f"fixture {fixture_id} producer failed with exit {result.returncode}")
        return publish_staged(staged, output)
    finally:
        staged.unlink(missing_ok=True)


def run_sharded(registry: dict, producer: Path, jobs: int) -> bool:
    if not producer.is_file():
        raise FixtureError(f"source test producer does not exist: {producer}")
    names = [case["name"] for case in registry["cases"]]
    worker_count = min(jobs or 8, len(names))
    if worker_count < 1:
        raise FixtureError("sharded source tests require at least one worker")

    def run_case(name: str) -> tuple[str, subprocess.CompletedProcess[bytes] | None, str | None, float]:
        case_started = time.monotonic()
        try:
            result = subprocess.run([producer, "--run-case", name], capture_output=True,
                                    check=False, timeout=180)
            return name, result, None, time.monotonic() - case_started
        except subprocess.TimeoutExpired:
            return name, None, "timed out after 180 seconds", time.monotonic() - case_started

    started = time.monotonic()
    with ThreadPoolExecutor(max_workers=worker_count) as pool:
        results = list(pool.map(run_case, names))
    failures = 0
    for name, result, error, _ in results:
        if error is None and result is not None and result.returncode == 0:
            continue
        failures += 1
        print(f"\n=== source shard failed: {name} ===", file=sys.stderr)
        if error is not None:
            print(error, file=sys.stderr)
            continue
        output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
        print(output.rstrip(), file=sys.stderr)
    elapsed = time.monotonic() - started
    passed = len(names) - failures
    print(f"source shards: {passed}/{len(names)} passed with {worker_count} workers "
          f"in {elapsed:.2f}s")
    for name, _, _, duration in sorted(results, key=lambda row: (-row[3], row[0]))[:5]:
        print(f"source shard duration: {name} {duration:.3f}s")
    return failures == 0


class RegistryTests(unittest.TestCase):
    def setUp(self):
        self.source = "TEST(example) {\nXR_SOURCE_FIXTURE_EXAMPLE\n}\nTEST(semantic_only) {\n}\n"
        self.core_operations = {
            "core.class.construct": {"vm": "NOT_YET_ACTIVE", "aot": "NOT_YET_ACTIVE"},
            "core.class.field_load": {"vm": "NOT_YET_ACTIVE", "aot": "NOT_YET_ACTIVE"},
            "core.constant.i64": {"vm": "COMPLETE", "aot": "COMPLETE"},
        }
        self.payload = {"schema": 2, "cases": [
            {"name": "example", "fixture": {"id": "example", "expected_exit": 7,
                                             "labels": ["coroutine"],
                                             "backends": {"vm": "execute", "aot": "execute"}}},
            {"name": "semantic_only", "fixture": None}]}

    def test_live_registry_and_exact_projection_are_complete(self):
        registry = load_registry()
        fixtures = fixture_records(registry)
        native = native_fixture_records(registry)
        pending = pending_fixture_cases(registry)
        self.assertTrue(fixtures)
        self.assertEqual(len(native) + len(pending), len(fixtures))
        self.assertEqual(len(set(native_target_names(registry))), len(native))
        self.assertEqual(len(set(pending_test_names(registry))), len(pending))
        header, cmake = project_registry(registry)
        self.assertEqual(cmake.count(b"add_xr_program_source_native_fixture("), len(native))
        self.assertEqual(cmake.count(b"add_xr_program_source_pending_fixture("), len(pending))
        self.assertEqual(header.count(b"case XR_SOURCE_FIXTURE_"), len(fixtures) + 1)
        for case in registry["cases"]:
            fixture = case["fixture"]
            enum = "XR_SOURCE_FIXTURE_" + (fixture["id"].upper() if fixture else "NONE")
            mapping = f"    X({case['name']}, {enum})".encode("utf-8")
            self.assertEqual(header.count(mapping), 1)
        for fixture in fixtures:
            vm_operation = operation_constant(fixture["backends"]["vm"])
            aot_operation = operation_constant(fixture["backends"]["aot"])
            mapping = (f"        case XR_SOURCE_FIXTURE_{fixture['id'].upper()}:\n"
                       f"            return backend == XR_SOURCE_BACKEND_VM ? "
                       f"{vm_operation} : {aot_operation};").encode("utf-8")
            self.assertEqual(header.count(mapping), 1)
        for fixture, target in zip(native, native_target_names(registry)):
            registration = (f"add_xr_program_source_native_fixture({fixture['id']} "
                            f"{target} {fixture['expected_exit']} ").encode("utf-8")
            self.assertEqual(cmake.count(registration), 1)
        for (case, fixture), test_name in zip(pending, pending_test_names(registry)):
            registration = (f"add_xr_program_source_pending_fixture({fixture['id']} "
                            f"{case['name']} {test_name} ").encode("utf-8")
            self.assertEqual(cmake.count(registration), 1)
        with tempfile.TemporaryDirectory(prefix="xr-source-fixture-projection-") as directory:
            root = Path(directory)
            header_path, cmake_path = root / "cases.h", root / "fixtures.cmake"
            command = ["project", "--header", str(header_path),
                       "--cmake", str(cmake_path)]
            self.assertEqual(main(command), 0)
            before = [(path.stat().st_ino, path.stat().st_mtime_ns)
                      for path in (header_path, cmake_path)]
            self.assertEqual(main(command), 0)
            self.assertEqual(header_path.read_bytes(), header)
            self.assertEqual(cmake_path.read_bytes(), cmake)
            self.assertEqual(before, [(path.stat().st_ino, path.stat().st_mtime_ns)
                                     for path in (header_path, cmake_path)])

    def test_cli_refuses_missing_unknown_duplicate_and_positional_arguments(self):
        with tempfile.TemporaryDirectory(prefix="xr-source-fixture-cli-") as directory:
            output = Path(directory) / "unexpected.c"
            valid = ["generate", "--fixture", "cross_module_coroutine",
                     "--producer", sys.executable, "--output", str(output)]
            invalid = [[], [str(output)], valid[:-2],
                       valid + ["--fixture", "cross_module_coroutine"],
                       valid + ["--unknown", "value"],
                       ["generate", "--fix", "cross_module_coroutine"] + valid[3:]]
            for arguments in invalid:
                errors = io.StringIO()
                with mock.patch.object(sys, "stderr", errors), \
                        self.assertRaises(SystemExit) as raised:
                    parse_arguments(arguments)
                self.assertEqual(raised.exception.code, 2, arguments)
                self.assertFalse(output.exists(), arguments)
                self.assertTrue(errors.getvalue(), arguments)
            errors = io.StringIO()
            unknown = ["generate", "--fixture", "unknown"] + valid[3:]
            with mock.patch.object(sys, "stderr", errors):
                self.assertEqual(main(unknown), 1)
            self.assertIn("unknown fixture id", errors.getvalue())
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_complete_registration_and_identity(self):
        registry = validate_registry(self.payload, self.source, self.core_operations)
        header, cmake = project_registry(registry)
        self.assertIn(b"XR_SOURCE_CASE_COUNT 2u", header)
        self.assertEqual(cmake.count(b"add_xr_program_source_native_fixture("), 1)
        self.assertIn(b"X(semantic_only, XR_SOURCE_FIXTURE_NONE)", header)
        changed = copy.deepcopy(registry)
        changed["cases"][0]["fixture"]["expected_exit"] = 8
        self.assertNotEqual(registry_identity(registry), registry_identity(changed))

        pending = copy.deepcopy(self.payload)
        unsupported = {"status": "unsupported", "operation": "core.class.construct"}
        pending["cases"][0]["fixture"]["backends"] = {
            "vm": copy.deepcopy(unsupported), "aot": copy.deepcopy(unsupported)}
        pending_registry = validate_registry(pending, self.source, self.core_operations)
        pending_header, pending_cmake = project_registry(pending_registry)
        self.assertEqual(len(fixture_records(pending_registry)), 1)
        self.assertEqual(len(native_fixture_records(pending_registry)), 0)
        self.assertIn(b"XR_SOURCE_FIXTURE_EXAMPLE", pending_header)
        self.assertIn(b"X(example, XR_SOURCE_FIXTURE_EXAMPLE)", pending_header)
        self.assertIn(b"XR_CORE_OP_CORE_CLASS_CONSTRUCT", pending_header)
        self.assertNotIn(b"add_xr_program_source_native_fixture(", pending_cmake)
        self.assertIn(b"add_xr_program_source_pending_fixture(example example ", pending_cmake)

    def test_stderr_expectation_is_exact_and_projected(self):
        candidate = copy.deepcopy(self.payload)
        candidate["cases"][0]["fixture"]["expected_stderr_hex"] = "6572726f720a"
        registry = validate_registry(candidate, self.source, self.core_operations)
        _, cmake = project_registry(registry)
        self.assertIn(b'"6572726f720a"', cmake)
        for malformed in (True, "x1", "ABC", "0g", "a"):
            candidate["cases"][0]["fixture"]["expected_stderr_hex"] = malformed
            with self.assertRaises(FixtureError):
                validate_registry(candidate, self.source, self.core_operations)

    def test_missing_duplicate_unknown_and_malformed_records_fail_closed(self):
        mutations = [lambda value: value["cases"].pop(),
                     lambda value: value["cases"].append(copy.deepcopy(value["cases"][0])),
                     lambda value: value["cases"][0].update(name="unknown"),
                     lambda value: value["cases"][0].update(fixture=None),
                     lambda value: value["cases"][1].update(fixture=copy.deepcopy(value["cases"][0]["fixture"])),
                     lambda value: value["cases"][0]["fixture"].update(id="../escape"),
                     lambda value: value["cases"][0]["fixture"].update(expected_exit=True),
                     lambda value: value["cases"][0]["fixture"].update(backends={"vm": "execute"}),
                     lambda value: value["cases"][0]["fixture"]["backends"].update(
                         vm={"status": "unsupported", "operation": "not-an-operation"}),
                     lambda value: value["cases"][0]["fixture"].update(extra="unknown"),
                     lambda value: value.update(schema=1)]
        for mutate in mutations:
            candidate = copy.deepcopy(self.payload)
            mutate(candidate)
            with self.assertRaises(FixtureError):
                validate_registry(candidate, self.source, self.core_operations)
        with self.assertRaises(FixtureError):
            json.loads('{"schema":1,"schema":1}', object_pairs_hook=_unique_object)

    def test_backend_expectations_and_core_spec_operations_fail_closed(self):
        unsupported = {"status": "unsupported", "operation": "core.class.construct"}

        asymmetric = copy.deepcopy(self.payload)
        asymmetric["cases"][0]["fixture"]["backends"] = {
            "vm": "execute", "aot": copy.deepcopy(unsupported)}
        with self.assertRaises(FixtureError):
            validate_registry(asymmetric, self.source, self.core_operations)

        mismatched = copy.deepcopy(self.payload)
        mismatched["cases"][0]["fixture"]["backends"] = {
            "vm": copy.deepcopy(unsupported),
            "aot": {"status": "unsupported", "operation": "core.class.field_load"}}
        with self.assertRaises(FixtureError):
            validate_registry(mismatched, self.source, self.core_operations)

        unknown = copy.deepcopy(self.payload)
        unknown_operation = {"status": "unsupported", "operation": "core.missing.operation"}
        unknown["cases"][0]["fixture"]["backends"] = {
            "vm": copy.deepcopy(unknown_operation), "aot": copy.deepcopy(unknown_operation)}
        with self.assertRaises(FixtureError):
            validate_registry(unknown, self.source, self.core_operations)

        active = copy.deepcopy(self.payload)
        active_operation = {"status": "unsupported", "operation": "core.constant.i64"}
        active["cases"][0]["fixture"]["backends"] = {
            "vm": copy.deepcopy(active_operation), "aot": copy.deepcopy(active_operation)}
        with self.assertRaises(FixtureError):
            validate_registry(active, self.source, self.core_operations)

    def test_source_fixture_binding_fails_closed_when_missing_or_mismatched(self):
        missing = self.source.replace("XR_SOURCE_FIXTURE_EXAMPLE", "")
        mismatched = self.source.replace("XR_SOURCE_FIXTURE_EXAMPLE",
                                         "XR_SOURCE_FIXTURE_WRONG")
        extra = self.source.replace("TEST(semantic_only) {\n}",
                                    "TEST(semantic_only) {\nXR_SOURCE_FIXTURE_EXAMPLE\n}")
        for source in (missing, mismatched, extra):
            with self.assertRaises(FixtureError):
                validate_registry(self.payload, source, self.core_operations)

    def test_core_spec_operation_coverage_loader_rejects_hostile_records(self):
        valid = {"operations": [{
            "spelling": "core.class.construct",
            "coverage": {
                "vm": {"status": "NOT_YET_ACTIVE"},
                "aot": {"status": "NOT_YET_ACTIVE"},
            },
        }]}
        mutations = [
            lambda value: value.update(operations={}),
            lambda value: value.update(operations=[]),
            lambda value: value["operations"].append(copy.deepcopy(value["operations"][0])),
            lambda value: value["operations"][0].update(spelling="not-an-operation"),
            lambda value: value["operations"][0]["coverage"].pop("vm"),
            lambda value: value["operations"][0]["coverage"]["aot"].pop("status"),
        ]
        with tempfile.TemporaryDirectory(prefix="xr-core-operation-coverage-") as directory:
            path = Path(directory) / "registry.json"
            path.write_text(json.dumps(valid), encoding="utf-8")
            self.assertEqual(load_core_operation_coverage(path), {
                "core.class.construct": {
                    "vm": "NOT_YET_ACTIVE", "aot": "NOT_YET_ACTIVE"}})
            for mutate in mutations:
                hostile = copy.deepcopy(valid)
                mutate(hostile)
                path.write_text(json.dumps(hostile), encoding="utf-8")
                with self.assertRaises(FixtureError):
                    load_core_operation_coverage(path)

    def test_exact_stable_publish_and_failures(self):
        with tempfile.TemporaryDirectory(prefix="xr-source-fixture-writer-") as directory:
            root = Path(directory)
            output = root / "output.c"
            self.assertTrue(write_stable(output, b"int main(void) { return 7; }\n"))
            os.utime(output, ns=(1700000000000000000, 1700000000000000000))
            before = output.stat()
            self.assertFalse(write_stable(output, output.read_bytes()))
            after = output.stat()
            self.assertEqual((before.st_ino, before.st_mtime_ns), (after.st_ino, after.st_mtime_ns))
            self.assertTrue(write_stable(output, b"int main(void) { return 8; }\n"))
            changed = output.read_bytes()
            with mock.patch.object(Path, "read_bytes", side_effect=PermissionError("read denied")):
                with self.assertRaises(PermissionError):
                    write_stable(output, b"replacement\n")
            self.assertEqual(output.read_bytes(), changed)
            output.write_bytes(b"\xffinvalid UTF-8")
            with self.assertRaises(UnicodeDecodeError):
                write_stable(output, b"replacement\n")
            self.assertEqual(output.read_bytes(), b"\xffinvalid UTF-8")
            self.assertEqual(list(root.iterdir()), [output])

    def test_projection_check_rejects_missing_and_changed_bytes_without_writing(self):
        registry = validate_registry(self.payload, self.source, self.core_operations)
        with tempfile.TemporaryDirectory(prefix="xr-source-fixture-check-") as directory:
            header, cmake = Path(directory) / "cases.h", Path(directory) / "fixtures.cmake"
            for path, content in zip((header, cmake), project_registry(registry)):
                write_stable(path, content)
            before = [(path.stat().st_ino, path.stat().st_mtime_ns) for path in (header, cmake)]
            check_projection(registry, header, cmake)
            self.assertEqual(before, [(path.stat().st_ino, path.stat().st_mtime_ns)
                                     for path in (header, cmake)])
            for path in (header, cmake):
                original = path.read_bytes()
                path.write_bytes(original + b"changed\n")
                with self.assertRaises(FixtureError):
                    check_projection(registry, header, cmake)
                self.assertEqual(path.read_bytes(), original + b"changed\n")
                path.unlink()
                with self.assertRaises(FileNotFoundError):
                    check_projection(registry, header, cmake)
                self.assertFalse(path.exists())
                path.write_bytes(original)

    def test_one_producer_and_failed_publication(self):
        registry = validate_registry(self.payload, self.source, self.core_operations)
        with tempfile.TemporaryDirectory(prefix="xr-source-fixture-producer-") as directory:
            root = Path(directory)
            producer = root / "producer"
            producer.write_bytes(b"synthetic producer")
            output = root / "output.c"
            output.write_bytes(b"previous\n")

            def run(command, **kwargs):
                self.assertEqual(command[1:3], ["--emit-fixture", "example"])
                self.assertEqual(command[3:5], ["--registry", registry_identity(registry)])
                Path(command[-1]).write_bytes(b"generated\n")
                return subprocess.CompletedProcess(command, 0)

            with mock.patch.object(subprocess, "run", side_effect=run) as execute:
                self.assertTrue(generate_fixture(registry, "example", producer, output))
                self.assertEqual(execute.call_count, 1)
                self.assertFalse(generate_fixture(registry, "example", producer, output))
                with self.assertRaises(FixtureError):
                    generate_fixture(registry, "unknown", producer, output)
                self.assertEqual(execute.call_count, 2)
            with mock.patch.object(subprocess, "run", return_value=subprocess.CompletedProcess([], 1)):
                with self.assertRaises(FixtureError):
                    generate_fixture(registry, "example", producer, output)
            with mock.patch.object(subprocess, "run", return_value=subprocess.CompletedProcess([], 0)):
                with self.assertRaises(FixtureError):
                    generate_fixture(registry, "example", producer, output)
            self.assertEqual(output.read_bytes(), b"generated\n")
            self.assertEqual(sorted(path.name for path in root.iterdir()), ["output.c", "producer"])


class BuildGraphTests(unittest.TestCase):
    def test_source_body_relinks_without_configure_and_unregistered_case_refuses(self):
        sys.path.insert(0, str(ROOT / "tests/lib"))
        from xraytest import sanitizer
        self.assertTrue(sanitizer.activate_windows_msvc_environment(
            lambda message, **kwargs: print(message)))
        unit_cmake = (ROOT / "tests/unit/CMakeLists.txt").read_text(encoding="utf-8")
        setup = unit_cmake.split("set(XR_PROGRAM_SOURCE_FIXTURE_SCRIPT", 1)[1].split(
            "add_executable(test_xr_program_fuzz_entry", 1)[0]
        setup = "set(XR_PROGRAM_SOURCE_FIXTURE_SCRIPT" + setup
        registration = unit_cmake.split("function(add_xr_program_source_native_fixture", 1)[1].split(
            "include(${XR_PROGRAM_SOURCE_FIXTURE_REGISTRATION})", 1)[0]
        registration = "function(add_xr_program_source_native_fixture" + registration
        registration += "include(${XR_PROGRAM_SOURCE_FIXTURE_REGISTRATION})\n"
        with tempfile.TemporaryDirectory(prefix="xr-source-fixture-ninja-") as directory:
            root = Path(directory)
            script = root / "scripts/program_source_fixtures.py"
            source = root / "tests/unit/program/test_xr_program_source_build.c"
            manifest = root / "tests/unit/program/xr_program_source_cases.json"
            support = root / "tests/unit/plan/target_profile_test_fixture.c"
            core_spec = root / "xisa/core/registry.json"
            for path in (script, source, support, core_spec):
                path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(Path(__file__), script)
            payload = {"schema": 2, "cases": [{"name": "example", "fixture": {
                "id": "example", "expected_exit": 7, "labels": ["coroutine"],
                "backends": {"vm": "execute", "aot": "execute"}}}]}
            manifest.write_text(json.dumps(payload), encoding="utf-8")
            core_spec.write_text(json.dumps({"operations": [{
                "spelling": "core.constant.i64",
                "coverage": {
                    "vm": {"status": "COMPLETE"},
                    "aot": {"status": "COMPLETE"},
                },
            }]}), encoding="utf-8")
            source_text = ('#include "xr_program_source_cases.gen.h"\n'
                           '#define TEST(name) static void test_##name(void)\n'
                           'TEST(example) {\n    (void) XR_SOURCE_FIXTURE_EXAMPLE;\n}\n'
                           '#define RUN_SOURCE(name, fixture) test_##name();\n'
                           'int main(void) { XR_SOURCE_CASES(RUN_SOURCE) return 0; }\n')
            source.write_text(source_text, encoding="utf-8")
            support.write_text("int xr_fixture_support;\n", encoding="utf-8")
            (root / "tests/unit/CMakeLists.txt").write_text(setup + registration, encoding="utf-8")
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.21)\nproject(SourceFixtureGraph C)\n'
                'set(CMAKE_C_STANDARD 11)\nenable_testing()\n'
                 f'set(XRAY_PYTHON "{Path(sys.executable).as_posix()}")\n'
                 'file(APPEND "${CMAKE_BINARY_DIR}/configure.events" "configure\\n")\n'
                 'function(add_xray_unit_test target)\n'
                 '  add_executable(${target} ${ARGN})\n'
                 '  add_test(NAME ${target} COMMAND $<TARGET_FILE:${target}>)\nendfunction()\n'
                 'function(add_xray_bootstrap_executable target)\n'
                 '  add_executable(${target} ${ARGN})\nendfunction()\n'
                 'function(add_xray_bootstrap_unit_test target)\n'
                 '  add_xray_unit_test(${target} ${ARGN})\nendfunction()\n'
                 'function(xr_enable_pure_aot_symbol_map target)\nendfunction()\n'
                'add_subdirectory(tests/unit)\n', encoding="utf-8")
            build = root / "build"

            def command(arguments, *, success=True):
                result = subprocess.run(arguments, capture_output=True, check=False)
                combined = (result.stdout + result.stderr).decode("utf-8", errors="replace")
                self.assertEqual(result.returncode == 0, success, combined)
                return combined

            command(["cmake", "-G", "Ninja", "-S", str(root), "-B", str(build),
                     "-DCMAKE_BUILD_TYPE=Release"])
            invocation = ["cmake", "--build", str(build), "-j", "1", "--target",
                          "test_xr_program_source_build"]
            verified = "source fixtures: case census and projections verified"
            self.assertEqual(command(invocation).count(verified), 1)
            configure_events = (build / "configure.events").read_bytes()
            self.assertNotIn(verified, command(invocation))
            changed = source_text.replace("(void) XR_SOURCE_FIXTURE_EXAMPLE;",
                                          "(void) XR_SOURCE_FIXTURE_EXAMPLE;\n    (void) 0;")
            source.write_text(changed, encoding="utf-8")
            self.assertEqual(command(invocation).count(verified), 1)
            self.assertEqual((build / "configure.events").read_bytes(), configure_events)
            source.write_text(changed + "TEST(unregistered) {\n}\n", encoding="utf-8")
            for _ in range(2):
                refused = command(invocation, success=False)
                self.assertIn("complete TEST declaration census", refused)
                self.assertNotIn(verified, refused)
            self.assertEqual((build / "configure.events").read_bytes(), configure_events)
            source.write_text(changed, encoding="utf-8")
            self.assertEqual(command(invocation).count(verified), 1)
            self.assertNotIn(verified, command(invocation))
            self.assertEqual((build / "configure.events").read_bytes(), configure_events)
            producer = build / "tests/unit/test_xr_program_source_build"
            if os.name == "nt":
                producer = producer.with_suffix(".exe")
            sharded = command([sys.executable, str(script), "run-sharded", "--producer",
                               str(producer),
                               "--jobs", "2"])
            self.assertIn("source shards: 1/1 passed with 1 workers", sharded)
            self.assertRegex(sharded, r"source shard duration: example [0-9]+\.[0-9]{3}s")


def parse_arguments(arguments: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    commands = parser.add_subparsers(dest="command", required=True)
    for mode in ("project", "check"):
        projection = commands.add_parser(mode, allow_abbrev=False)
        projection.add_argument("--header", type=Path, required=True, action=Once)
        projection.add_argument("--cmake", type=Path, required=True, action=Once)
    generate = commands.add_parser("generate", allow_abbrev=False)
    generate.add_argument("--fixture", required=True, action=Once)
    generate.add_argument("--producer", type=Path, required=True, action=Once)
    generate.add_argument("--output", type=Path, required=True, action=Once)
    sharded = commands.add_parser("run-sharded", allow_abbrev=False)
    sharded.add_argument("--producer", type=Path, required=True, action=Once)
    sharded.add_argument("--jobs", type=int, action=Once)
    commands.add_parser("self-test", allow_abbrev=False)
    commands.add_parser("self-test-build", allow_abbrev=False)
    return parser.parse_args(arguments)


def main(arguments: list[str] | None = None) -> int:
    args = parse_arguments(sys.argv[1:] if arguments is None else arguments)
    if args.command in ("self-test", "self-test-build"):
        test_case = RegistryTests if args.command == "self-test" else BuildGraphTests
        result = unittest.TextTestRunner(verbosity=2).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(test_case))
        return 0 if result.wasSuccessful() else 1
    try:
        registry = load_registry()
        if args.command in ("project", "check"):
            if args.header.resolve() == args.cmake.resolve():
                raise FixtureError("registry projections require distinct output paths")
            if args.command == "project":
                header, cmake = project_registry(registry)
                write_stable(args.header, header)
                write_stable(args.cmake, cmake)
            else:
                check_projection(registry, args.header, args.cmake)
                print("source fixtures: case census and projections verified")
        elif args.command == "generate":
            generate_fixture(registry, args.fixture, args.producer, args.output)
        else:
            return 0 if run_sharded(registry, args.producer, args.jobs) else 1
    except (FixtureError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"source fixtures: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
