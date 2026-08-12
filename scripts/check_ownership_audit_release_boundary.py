#!/usr/bin/env python3
"""Keep ownership-audit instrumentation out of release runtime archives."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
AUDIT_SOURCE = "src/runtime/ownership/xr_ownership_audit.c"
AUDIT_SOURCE_VAR = "RUNTIME_OWNERSHIP_AUDIT_SRC"
AUDIT_TARGET = "xray_ownership_audit"
AUDIT_TEST_TARGET = "test_ownership_audit"


@dataclass(frozen=True)
class Command:
    path: str
    line: int
    name: str
    args: str


def _strip_comments(text: str) -> str:
    """Remove line comments without treating a hash inside a string as syntax."""

    output: list[str] = []
    quoted = False
    escaped = False
    comment = False
    for char in text:
        if comment:
            if char == "\n":
                comment = False
                output.append(char)
            else:
                output.append(" ")
            continue
        if quoted:
            output.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
            continue
        if char == '"':
            quoted = True
            output.append(char)
        elif char == "#":
            comment = True
            output.append(" ")
        else:
            output.append(char)
    return "".join(output)


def _commands(path: str, text: str) -> list[Command]:
    cleaned = _strip_comments(text)
    commands: list[Command] = []
    pattern = re.compile(r"(?im)^\s*([A-Za-z_]\w*)\s*\(")
    cursor = 0
    while match := pattern.search(cleaned, cursor):
        depth = 1
        quoted = False
        escaped = False
        index = match.end()
        while index < len(cleaned) and depth:
            char = cleaned[index]
            if quoted:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    quoted = False
            elif char == '"':
                quoted = True
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            index += 1
        line = cleaned.count("\n", 0, match.start()) + 1
        if depth:
            commands.append(Command(path, line, "<parse-error>", ""))
            break
        commands.append(
            Command(path, line, match.group(1).lower(), cleaned[match.end() : index - 1])
        )
        cursor = index
    return commands


def _words(args: str) -> list[str]:
    return re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"|([^\s]+)', args)


def _flat_words(args: str) -> list[str]:
    return [quoted or bare for quoted, bare in _words(args)]


def _location(command: Command) -> str:
    return f"{command.path}:{command.line}"


def verify(texts: dict[str, str]) -> list[str]:
    commands = [
        command
        for path, text in texts.items()
        for command in _commands(path.replace("\\", "/"), text)
    ]
    errors: list[str] = []

    parse_errors = [command for command in commands if command.name == "<parse-error>"]
    for command in parse_errors:
        errors.append(f"{_location(command)}: unterminated CMake command")

    source_refs: list[Command] = []
    variable_refs: list[Command] = []
    target_refs: list[Command] = []
    recursive_runtime_globs: list[Command] = []
    ownership_globs: list[Command] = []
    for command in commands:
        normalized = command.args.replace("\\", "/")
        if AUDIT_SOURCE in normalized:
            source_refs.append(command)
        if re.search(rf"\b{AUDIT_SOURCE_VAR}\b", normalized):
            variable_refs.append(command)
        if re.search(rf"\b{AUDIT_TARGET}\b", normalized):
            target_refs.append(command)
        if command.name == "file" and re.search(r"\bGLOB_RECURSE\b", normalized, re.I):
            tooling_only = re.match(
                r"\s*GLOB_RECURSE\s+ALL_SOURCE_FILES\b", normalized, re.I
            )
            if not tooling_only and re.search(
                r"(?:^|[\s\"/])src/(?:runtime/)?\*", normalized
            ):
                recursive_runtime_globs.append(command)
        if command.name in {"file", "aux_source_directory"} and \
                "src/runtime/ownership" in normalized:
            ownership_globs.append(command)

    expected_source_refs = {
        ("CMakeLists.txt", "set"),
        ("tests/unit/CMakeLists.txt", "add_executable"),
    }
    actual_source_refs = {(command.path, command.name) for command in source_refs}
    if actual_source_refs != expected_source_refs or len(source_refs) != 2:
        locations = ", ".join(_location(command) for command in source_refs) or "none"
        errors.append(f"audit source references must be exactly the manifest and focused test; got {locations}")

    expected_variable_refs = {
        ("CMakeLists.txt", "set"),
        ("CMakeLists.txt", "add_library"),
    }
    actual_variable_refs = {(command.path, command.name) for command in variable_refs}
    if actual_variable_refs != expected_variable_refs or len(variable_refs) != 2:
        locations = ", ".join(_location(command) for command in variable_refs) or "none"
        errors.append(f"audit source manifest has an unexpected consumer: {locations}")

    expected_target_refs = {
        ("CMakeLists.txt", "add_library"),
        ("CMakeLists.txt", "target_include_directories"),
    }
    actual_target_refs = {(command.path, command.name) for command in target_refs}
    if actual_target_refs != expected_target_refs or len(target_refs) != 2:
        locations = ", ".join(_location(command) for command in target_refs) or "none"
        errors.append(f"audit target must not be linked or depended on: {locations}")

    library = next(
        (
            command
            for command in commands
            if command.path == "CMakeLists.txt"
            and command.name == "add_library"
            and AUDIT_TARGET in _flat_words(command.args)
        ),
        None,
    )
    if library is None:
        errors.append("missing explicit ownership-audit library target")
    else:
        words = _flat_words(library.args)
        if words[:3] != [AUDIT_TARGET, "STATIC", "EXCLUDE_FROM_ALL"] or \
                f"${{{AUDIT_SOURCE_VAR}}}" not in words:
            errors.append(
                f"{_location(library)}: audit library must remain STATIC EXCLUDE_FROM_ALL "
                "and consume only its explicit source manifest"
            )

    focused_test = next(
        (
            command
            for command in commands
            if command.path == "tests/unit/CMakeLists.txt"
            and command.name == "add_executable"
            and _flat_words(command.args)[:1] == [AUDIT_TEST_TARGET]
        ),
        None,
    )
    if focused_test is None or AUDIT_SOURCE not in focused_test.args.replace("\\", "/"):
        errors.append("focused ownership-audit test must compile the diagnostic source explicitly")

    if recursive_runtime_globs:
        locations = ", ".join(_location(command) for command in recursive_runtime_globs)
        errors.append(f"recursive runtime glob could absorb diagnostic sources: {locations}")
    if ownership_globs:
        locations = ", ".join(_location(command) for command in ownership_globs)
        errors.append(f"ownership-directory glob could absorb the audit source: {locations}")

    return errors


def _fixture() -> dict[str, str]:
    return {
        "CMakeLists.txt": f'''
set({AUDIT_SOURCE_VAR} "{AUDIT_SOURCE}")
add_library({AUDIT_TARGET} STATIC EXCLUDE_FROM_ALL ${{{AUDIT_SOURCE_VAR}}})
target_include_directories({AUDIT_TARGET} PUBLIC src)
''',
        "tests/unit/CMakeLists.txt": f'''
add_executable({AUDIT_TEST_TARGET} test.c ${{CMAKE_SOURCE_DIR}}/{AUDIT_SOURCE})
''',
    }


def self_test() -> list[str]:
    failures: list[str] = []
    clean = _fixture()
    if errors := verify(clean):
        failures.append(f"clean fixture rejected: {'; '.join(errors)}")

    mutations = {
        "release-source-injection":
            f"\ntarget_sources(xray_vm PRIVATE {AUDIT_SOURCE})\n",
        "audit-target-link":
            f"\ntarget_link_libraries(xray_vm PRIVATE {AUDIT_TARGET})\n",
        "recursive-runtime-glob":
            '\nfile(GLOB_RECURSE RUNTIME_ALL "src/runtime/*.c")\n',
    }
    for name, suffix in mutations.items():
        mutated = dict(clean)
        mutated["CMakeLists.txt"] += suffix
        if not verify(mutated):
            failures.append(f"mutation was not rejected: {name}")

    missing_exclusion = dict(clean)
    missing_exclusion["CMakeLists.txt"] = missing_exclusion["CMakeLists.txt"].replace(
        " STATIC EXCLUDE_FROM_ALL ", " STATIC "
    )
    if not verify(missing_exclusion):
        failures.append("mutation was not rejected: missing-exclude-from-all")
    return failures


def project_texts() -> dict[str, str]:
    paths = sorted(ROOT.rglob("CMakeLists.txt")) + sorted(ROOT.rglob("*.cmake"))
    return {
        path.relative_to(ROOT).as_posix(): path.read_text(encoding="utf-8")
        for path in paths
        if not any(part.startswith("build") for part in path.relative_to(ROOT).parts)
    }


def main() -> int:
    errors = [f"self-test: {error}" for error in self_test()]
    errors.extend(verify(project_texts()))
    if errors:
        print("ownership audit release boundary: FAIL", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("ownership audit release boundary: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
