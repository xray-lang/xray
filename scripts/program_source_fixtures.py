#!/usr/bin/env python3
"""Own source-test registration and publish one successful native fixture."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "tests/unit/program/xr_program_source_cases.json"
SOURCE = ROOT / "tests/unit/program/test_xr_program_source_build.c"
IDENTIFIER = re.compile(r"[a-z][a-z0-9_]*\Z")
LABEL = re.compile(r"[a-z][a-z0-9-]*\Z")


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


def validate_registry(payload: object, source: str) -> dict:
    if not isinstance(payload, dict) or set(payload) != {"schema", "cases"}:
        raise FixtureError("registry requires exactly schema and cases")
    if type(payload["schema"]) is not int or payload["schema"] != 1:
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
        if not isinstance(fixture, dict) or set(fixture) != {"id", "expected_exit", "labels"}:
            raise FixtureError(f"invalid fixture record for {name}")
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


def native_target_names(registry: dict) -> tuple[str, ...]:
    return tuple(f'test_xr_program_{fixture["id"]}_aot_native'
                 for fixture in fixture_records(registry))


def project_registry(registry: dict) -> tuple[bytes, bytes]:
    fixtures = fixture_records(registry)
    header = ["/* Generated source-test registry. Do not edit. */",
              "#ifndef XR_PROGRAM_SOURCE_CASES_GEN_H", "#define XR_PROGRAM_SOURCE_CASES_GEN_H",
              f'#define XR_SOURCE_REGISTRY_ID "{registry_identity(registry)}"',
              f'#define XR_SOURCE_CASE_COUNT {len(registry["cases"])}u',
              "typedef enum XrSourceFixtureId {", "    XR_SOURCE_FIXTURE_NONE = 0,"]
    for fixture in fixtures:
        header.append(f'    XR_SOURCE_FIXTURE_{fixture["id"].upper()},')
    header.extend(["} XrSourceFixtureId;",
                   "#define XR_SOURCE_FIXTURES(X)" + (" \\" if fixtures else "")])
    for index, fixture in enumerate(fixtures):
        suffix = " \\" if index + 1 < len(fixtures) else ""
        header.append(f'    X({fixture["id"]}, XR_SOURCE_FIXTURE_{fixture["id"].upper()}){suffix}')
    header.append("#define XR_SOURCE_CASES(X) \\")
    for index, case in enumerate(registry["cases"]):
        fixture = case["fixture"]
        enum = "XR_SOURCE_FIXTURE_" + (fixture["id"].upper() if fixture else "NONE")
        suffix = " \\" if index + 1 < len(registry["cases"]) else ""
        header.append(f'    X({case["name"]}, {enum}){suffix}')
    header.extend(["#endif", ""])
    cmake = ["# Generated source-native fixture registration. Do not edit."]
    for fixture, target in zip(fixtures, native_target_names(registry)):
        labels = ";".join(fixture["labels"])
        cmake.append(f'add_xr_program_source_native_fixture({fixture["id"]} '
                     f'{target} {fixture["expected_exit"]} "{labels}")')
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
    if fixture_id not in {fixture["id"] for fixture in fixture_records(registry)}:
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


class RegistryTests(unittest.TestCase):
    def setUp(self):
        self.source = "TEST(example) {\nXR_SOURCE_FIXTURE_EXAMPLE\n}\nTEST(semantic_only) {\n}\n"
        self.payload = {"schema": 1, "cases": [
            {"name": "example", "fixture": {"id": "example", "expected_exit": 7,
                                             "labels": ["coroutine"]}},
            {"name": "semantic_only", "fixture": None}]}

    def test_live_registry_and_exact_projection_are_complete(self):
        registry = load_registry()
        self.assertEqual(len(registry["cases"]), 24)
        self.assertEqual(len(fixture_records(registry)), 16)
        header, cmake = project_registry(registry)
        self.assertEqual(header.count(b"    X(source_owner_"), 24)
        self.assertEqual(cmake.count(b"add_xr_program_source_native_fixture("), 16)
        with tempfile.TemporaryDirectory(prefix="xr-source-fixture-projection-") as directory:
            root = Path(directory)
            header_path, cmake_path = root / "cases.h", root / "fixtures.cmake"
            command = [sys.executable, str(Path(__file__).resolve()), "project",
                       "--header", str(header_path), "--cmake", str(cmake_path)]
            subprocess.run(command, check=True, capture_output=True)
            before = [(path.stat().st_ino, path.stat().st_mtime_ns)
                      for path in (header_path, cmake_path)]
            subprocess.run(command, check=True, capture_output=True)
            self.assertEqual(header_path.read_bytes(), header)
            self.assertEqual(cmake_path.read_bytes(), cmake)
            self.assertEqual(before, [(path.stat().st_ino, path.stat().st_mtime_ns)
                                     for path in (header_path, cmake_path)])

    def test_cli_refuses_missing_unknown_duplicate_and_positional_arguments(self):
        with tempfile.TemporaryDirectory(prefix="xr-source-fixture-cli-") as directory:
            output = Path(directory) / "unexpected.c"
            script = [sys.executable, str(Path(__file__).resolve())]
            valid = ["generate", "--fixture", "cross_module_coroutine",
                     "--producer", sys.executable, "--output", str(output)]
            invalid = [[], [str(output)], valid[:-2],
                       valid + ["--fixture", "cross_module_coroutine"],
                       valid + ["--unknown", "value"],
                       ["generate", "--fixture", "unknown"] + valid[3:],
                       ["generate", "--fix", "cross_module_coroutine"] + valid[3:]]
            for arguments in invalid:
                result = subprocess.run(script + arguments, capture_output=True, check=False)
                self.assertNotEqual(result.returncode, 0, arguments)
                self.assertFalse(output.exists(), arguments)
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_complete_registration_and_identity(self):
        registry = validate_registry(self.payload, self.source)
        header, cmake = project_registry(registry)
        self.assertIn(b"XR_SOURCE_CASE_COUNT 2u", header)
        self.assertEqual(cmake.count(b"add_xr_program_source_native_fixture("), 1)
        self.assertIn(b"X(semantic_only, XR_SOURCE_FIXTURE_NONE)", header)
        changed = copy.deepcopy(registry)
        changed["cases"][0]["fixture"]["expected_exit"] = 8
        self.assertNotEqual(registry_identity(registry), registry_identity(changed))

    def test_missing_duplicate_unknown_and_malformed_records_fail_closed(self):
        mutations = [lambda value: value["cases"].pop(),
                     lambda value: value["cases"].append(copy.deepcopy(value["cases"][0])),
                     lambda value: value["cases"][0].update(name="unknown"),
                     lambda value: value["cases"][0].update(fixture=None),
                     lambda value: value["cases"][1].update(fixture=copy.deepcopy(value["cases"][0]["fixture"])),
                     lambda value: value["cases"][0]["fixture"].update(id="../escape"),
                     lambda value: value["cases"][0]["fixture"].update(expected_exit=True),
                     lambda value: value["cases"][0]["fixture"].update(extra="unknown"),
                     lambda value: value.update(schema=2)]
        for mutate in mutations:
            candidate = copy.deepcopy(self.payload)
            mutate(candidate)
            with self.assertRaises(FixtureError):
                validate_registry(candidate, self.source)
        with self.assertRaises(FixtureError):
            json.loads('{"schema":1,"schema":1}', object_pairs_hook=_unique_object)

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
        registry = validate_registry(self.payload, self.source)
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
        registry = validate_registry(self.payload, self.source)
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
            for path in (script, source, support):
                path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(Path(__file__), script)
            payload = {"schema": 1, "cases": [{"name": "example", "fixture": {
                "id": "example", "expected_exit": 7, "labels": ["coroutine"]}}]}
            manifest.write_text(json.dumps(payload), encoding="utf-8")
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


def main() -> int:
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
    commands.add_parser("self-test", allow_abbrev=False)
    commands.add_parser("self-test-build", allow_abbrev=False)
    args = parser.parse_args()
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
        else:
            generate_fixture(registry, args.fixture, args.producer, args.output)
    except (FixtureError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"source fixtures: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
