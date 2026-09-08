#!/usr/bin/env python3
"""Qualify the generated allocation fixture with real Windows C11 providers."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "lib"))
from xraytest import sanitizer  # noqa: E402
import check_xr_program_aot_native as native  # noqa: E402

HARNESS = ROOT / "tests" / "unit" / "aot" / "xr_program_allocation_native_harness.c"
EXPECTED_STDOUT = b"allocation-native: PASS allocations=5 producer_failures=5\n"
EXPECTED_FACTS = {
    "schema": 1,
    "producer": "program-allocation-v1",
    "existential_pack": 1,
    "capturing_pack": 2,
    "callable_copy": 1,
    "allocations": 5,
}
FACT_KEYS = set(EXPECTED_FACTS) | {"entry", "plain_type", "inner_type", "capture_type", "sha256"}


class QualificationError(RuntimeError):
    pass


def strict_object(pairs):
    result = {}
    for name, value in pairs:
        if name in result:
            raise QualificationError(f"duplicate fixture fact: {name}")
        result[name] = value
    return result


def regular_bytes(path: Path, limit: int) -> bytes:
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= limit:
        raise QualificationError(f"missing or invalid fixture file: {path}")
    return path.read_bytes()


def load_fixture(directory: Path) -> tuple[dict, bytes]:
    source = regular_bytes(directory / "allocation.generated.c", 64 * 1024 * 1024)
    text = source.decode("utf-8", errors="strict")
    facts = json.loads(regular_bytes(directory / "allocation.facts.json", 4096),
                       object_pairs_hook=strict_object)
    if not isinstance(facts, dict) or set(facts) != FACT_KEYS:
        raise QualificationError("fixture facts are incomplete or unknown")
    for name, expected in EXPECTED_FACTS.items():
        if type(facts[name]) is not type(expected) or facts[name] != expected:
            raise QualificationError(f"fixture producer/count mismatch: {name}")
    for name in ("entry", "plain_type", "inner_type", "capture_type"):
        value = facts[name]
        lower, upper = (0, 3) if name == "entry" else (16, 65535)
        if type(value) is not int or not lower <= value <= upper:
            raise QualificationError(f"invalid fixture identity: {name}")
    if len({facts[name] for name in ("plain_type", "inner_type", "capture_type")}) != 3:
        raise QualificationError("fixture payload identities are not distinct")
    if facts["sha256"] != hashlib.sha256(source).hexdigest():
        raise QualificationError("fixture C digest does not match its producer facts")
    for name in ("plain_type", "inner_type", "capture_type"):
        if not re.search(rf"\bstruct XrAotType{facts[name]}\s*\{{", text):
            raise QualificationError(f"fixture payload declaration is missing: {name}")
    if not re.search(rf"\bXrAotOutcome\s+xr_aot_fn_{facts['entry']}\([^;]*\)\s*\{{", text):
        raise QualificationError("fixture entry definition is missing")
    if text.count("allocation payload alignment is unsupported") < 4:
        raise QualificationError("typed allocation alignment proofs are missing")
    # Remove comments before checking tokens so whitespace cannot hide a GNU expression.
    tokens = re.sub(r"/\*.*?\*/|//[^\n]*", " ", text, flags=re.DOTALL)
    for forbidden in (r"\(\s*\{", r"\btypeof\b", r"\b__attribute__\b", r"\b__asm__\b",
                      r"\bmax_align_t\b", r"#\s*pragma\b"):
        if re.search(forbidden, tokens):
            raise QualificationError(f"nonportable generated C: {forbidden}")
    return facts, source


def facts_header(facts: dict) -> str:
    return (f"#define XR_FIXTURE_ENTRY xr_aot_fn_{facts['entry']}\n"
            f"#define XR_FIXTURE_PLAIN XrAotType{facts['plain_type']}\n"
            f"#define XR_FIXTURE_INNER XrAotType{facts['inner_type']}\n"
            f"#define XR_FIXTURE_CAPTURE XrAotType{facts['capture_type']}\n"
            f"#define XR_FIXTURE_COPY_CAPTURE xr_aot_copy_{facts['capture_type']}\n")


def run_checked(command: list[str], directory: Path) -> subprocess.CompletedProcess:
    result = subprocess.run(command, cwd=directory, check=False, capture_output=True, timeout=60)
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).decode("utf-8", errors="replace")
        raise QualificationError(f"command failed ({result.returncode}): {command!r}\n{detail}")
    return result


def providers() -> list[tuple[str, str]]:
    if os.name != "nt":
        raise QualificationError("NOT_QUALIFIED: this gate requires a real Windows MSVC provider")

    def log(message: str, *, error: bool = False) -> None:
        print(message, file=sys.stderr if error else sys.stdout)

    if not sanitizer.activate_windows_msvc_environment(log):
        raise QualificationError("NOT_QUALIFIED: MSVC SDK environment is unavailable")
    msvc = shutil.which("cl")
    if not msvc:
        raise QualificationError("NOT_QUALIFIED: required MSVC cl.exe is unavailable")
    result = [("msvc", msvc)]
    clang_candidate = sanitizer.resolve_compiler_command("clang-cl")
    clang = shutil.which(clang_candidate)
    if clang:
        result.append(("clang-cl", clang))
    else:
        print("clang-cl: NOT_RUN (not installed; no clang-cl qualification claimed)")
    return result


def qualify_provider(name: str, compiler: str, directory: Path) -> None:
    executable = directory / f"allocation-{name}.exe"
    command = [compiler, "/nologo", "/TC", "/std:c11", "/W4", "/WX", "/utf-8", "/Od",
               f"/I{directory}", f"/Fo{directory / (name + '.obj')}", f"/Fe{executable}",
               str(HARNESS), "/link", f"/MAP:{executable.with_suffix('.map')}"]
    if name == "clang-cl":
        command.insert(1, "/clang:-pedantic-errors")
    run_checked(command, directory)
    result = run_checked([str(executable)], directory)
    if result.stdout.replace(b"\r\n", b"\n") != EXPECTED_STDOUT or result.stderr:
        raise QualificationError(f"{name}: missing or incorrect native check result")
    symbols, error = native.load_symbol_inventory(executable)
    if symbols is None:
        raise QualificationError(f"{name}: symbol inspection failed: {error}")
    for forbidden in native.FORBIDDEN:
        if re.search(forbidden, symbols, re.IGNORECASE):
            raise QualificationError(f"{name}: forbidden native symbol: {forbidden}")
    print(f"{name}: PASS (typed payloads, five producer failures, boundaries, destroy, symbols)")


def qualify(writer: Path) -> None:
    if not writer.is_file():
        raise QualificationError(f"fixture writer is missing: {writer}")
    selected = providers()
    if [name for name, _ in selected] not in (["msvc"], ["msvc", "clang-cl"]):
        raise QualificationError("required provider selection is invalid")
    with tempfile.TemporaryDirectory(prefix="xr-allocation-native-") as temporary:
        directory = Path(temporary)
        run_checked([str(writer.resolve()), "--output", str(directory / "allocation.generated.c"),
                     "--facts", str(directory / "allocation.facts.json")], directory)
        facts, original = load_fixture(directory)
        (directory / "allocation.facts.h").write_text(facts_header(facts), encoding="utf-8", newline="\n")
        completed = []
        for name, compiler in selected:
            qualify_provider(name, compiler, directory)
            completed.append(name)
            if (directory / "allocation.generated.c").read_bytes() != original:
                raise QualificationError("native qualification changed the generated C fixture")
        if completed != [name for name, _ in selected]:
            raise QualificationError("required provider qualification is incomplete")
    print(f"allocation native qualification: PASS (providers={','.join(completed)})")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--writer", type=Path, required=True, action="append")
    args = parser.parse_args(argv)
    if len(args.writer) != 1:
        parser.error("--writer must be specified exactly once")
    try:
        qualify(args.writer[0])
    except (OSError, UnicodeError, ValueError, QualificationError, subprocess.TimeoutExpired) as error:
        print(f"allocation native qualification: FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
