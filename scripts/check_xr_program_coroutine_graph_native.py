#!/usr/bin/env python3
"""Qualify both canonical coroutine graphs with real Windows C11 compilers."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import check_xr_program_aot_native as native
from check_xr_program_allocation_native import (
    QualificationError, providers, regular_bytes, run_checked, strict_object,
)

ROOT = Path(__file__).resolve().parents[1]
HARNESS = ROOT / "tests/unit/aot/xr_program_coroutine_graph_native_harness.c"
EXPECTED = {
    "cleanup": {"cases": 4, "blocks": 10, "provider_calls": 2, "conditionals": 3, "owner_drops": 4},
    "branch": {"cases": 2, "blocks": 4, "provider_calls": 0, "conditionals": 1, "owner_drops": 0},
}
COMMON = {"schema": 1, "producer": "program-coroutine-graphs-v1", "functions": 1,
          "entry": 0, "yields": 1, "self_edges": 1}
EXPECTED_STDOUT = {
    "cleanup": (b"cleanup mode=resume trace=71 outcome=return\n"
                b"cleanup mode=resume-refusal trace=71 outcome=trap7\n"
                b"cleanup mode=cancel trace=72 outcome=cancel\n"
                b"cleanup mode=cancel-refusal trace=72 outcome=trap7\n"
                b"coroutine-graph-native: PASS scenario=cleanup cases=4\n"),
    "branch": (b"branch mode=resume outcome=return value=11\n"
               b"branch mode=cancel outcome=cancel value=0\n"
               b"coroutine-graph-native: PASS scenario=branch cases=2\n"),
}


def load_fixture(directory: Path, scenario: str) -> tuple[dict, bytes]:
    if scenario not in EXPECTED:
        raise QualificationError(f"unknown coroutine graph: {scenario}")
    source = regular_bytes(directory / "graph.generated.c", 64 * 1024 * 1024)
    facts = json.loads(regular_bytes(directory / "graph.facts.json", 4096),
                       object_pairs_hook=strict_object)
    expected = {**COMMON, **EXPECTED[scenario], "scenario": scenario}
    if not isinstance(facts, dict) or set(facts) != set(expected) | {"execution_id", "sha256"}:
        raise QualificationError("coroutine graph facts are incomplete or unknown")
    for name, value in expected.items():
        if type(facts[name]) is not type(value) or facts[name] != value:
            raise QualificationError(f"coroutine graph producer/count mismatch: {name}")
    if not isinstance(facts["execution_id"], str) or not re.fullmatch(r"[0-9a-f]{64}", facts["execution_id"]) \
            or facts["execution_id"] == "0" * 64:
        raise QualificationError("invalid coroutine graph execution identity")
    if facts["sha256"] != hashlib.sha256(source).hexdigest():
        raise QualificationError("coroutine graph C digest differs from its producer facts")
    text = source.decode("utf-8", errors="strict")
    for declaration in (r"\bXrAotOutcome\s+xr_aot_fn_0_step\([^;]*\)\s*\{",
                        r"\bconst\s+XrBackendNativeDescriptor\s+xr_aot_entry_coroutine_descriptor\s*=",
                        r"\btypedef\s+struct\s+XrAotEntryCoroutineFrame\s*\{"):
        if not re.search(declaration, text):
            raise QualificationError("generated coroutine graph entry/descriptor is missing")
    tokens = re.sub(r"/\*.*?\*/|//[^\n]*", " ", text, flags=re.DOTALL)
    for forbidden in (r"\(\s*\{", r"\btypeof\b", r"\b__attribute__\b", r"\b__asm__\b",
                      r"#\s*pragma\b", *native.FORBIDDEN):
        if re.search(forbidden, tokens, flags=re.IGNORECASE):
            raise QualificationError(f"nonportable or executor-dependent generated C: {forbidden}")
    if re.search(r"\b(?:malloc|calloc|realloc|free|xr_aot_alloc)\s*\(", tokens):
        raise QualificationError("graph acquired heap resources not covered by its native harness")
    return facts, source


def facts_header(facts: dict) -> str:
    identity = ", ".join(f"UINT8_C({value})" for value in bytes.fromhex(facts["execution_id"]))
    return (f"#define XR_GRAPH_CLEANUP {int(facts['scenario'] == 'cleanup')}\n"
            f"#define XR_GRAPH_CASES {facts['cases']}\n"
            f"#define XR_GRAPH_EXECUTION_ID {{{identity}}}\n")


def stable_file_identity(path: Path) -> tuple[int, int, int]:
    info = path.stat()
    return info.st_mtime_ns, info.st_dev, info.st_ino


def generate_fixture(writer: Path, scenario: str, directory: Path) -> tuple[dict, bytes]:
    command = [str(writer.resolve()), "--scenario", scenario,
               "--output", str(directory / "graph.generated.c"),
               "--facts", str(directory / "graph.facts.json")]
    result = run_checked(command, directory)
    if result.stdout or result.stderr:
        raise QualificationError("fixture writer produced unexpected diagnostic output")
    facts, source = load_fixture(directory, scenario)
    paths = (directory / "graph.generated.c", directory / "graph.facts.json")
    identities = [stable_file_identity(path) for path in paths]
    result = run_checked(command, directory)
    if result.stdout or result.stderr or load_fixture(directory, scenario) != (facts, source):
        raise QualificationError("repeated fixture generation changed output or failed")
    if identities != [stable_file_identity(path) for path in paths]:
        raise QualificationError("identical fixture generation rewrote an output")
    (directory / "graph.facts.h").write_text(facts_header(facts), encoding="utf-8", newline="\n")
    return facts, source


def qualify_provider(name: str, compiler: str, scenario: str, directory: Path) -> None:
    executable = directory / f"graph-{name}.exe"
    command = [compiler, "/nologo", "/TC", "/std:c11", "/W4", "/WX", "/utf-8", "/Od",
               f"/I{directory}", f"/Fo{directory / (name + '.obj')}", f"/Fe{executable}",
               str(HARNESS), "/link", f"/MAP:{executable.with_suffix('.map')}"]
    if name == "clang-cl":
        command.insert(1, "/clang:-pedantic-errors")
    run_checked(command, directory)
    result = run_checked([str(executable)], directory)
    if result.stdout.replace(b"\r\n", b"\n") != EXPECTED_STDOUT[scenario] or result.stderr:
        raise QualificationError(f"{name}/{scenario}: missing or incorrect native result/trace")
    symbols, error = native.load_symbol_inventory(executable)
    if symbols is None:
        raise QualificationError(f"{name}/{scenario}: symbol inspection failed: {error}")
    for forbidden in native.FORBIDDEN:
        if re.search(forbidden, symbols, re.IGNORECASE):
            raise QualificationError(f"{name}/{scenario}: forbidden native symbol: {forbidden}")
    print(f"{name}/{scenario}: PASS (cases={EXPECTED[scenario]['cases']}, results, traces, disposal, symbols)")


def qualify(writer: Path) -> None:
    if not writer.is_file():
        raise QualificationError(f"fixture writer is missing: {writer}")
    selected = providers()
    if [name for name, _ in selected] not in (["msvc"], ["msvc", "clang-cl"]):
        raise QualificationError("required provider selection is incomplete or duplicated")
    completed = []
    execution_ids = set()
    with tempfile.TemporaryDirectory(prefix="xr-coroutine-graph-native-") as temporary:
        for scenario in EXPECTED:
            directory = Path(temporary) / scenario
            directory.mkdir()
            facts, original = generate_fixture(writer, scenario, directory)
            if facts["execution_id"] in execution_ids:
                raise QualificationError("distinct graph scenarios reuse one execution identity")
            execution_ids.add(facts["execution_id"])
            for name, compiler in selected:
                qualify_provider(name, compiler, scenario, directory)
                if load_fixture(directory, scenario) != (facts, original):
                    raise QualificationError("native qualification changed its generated graph")
                completed.append((scenario, name))
    required = [(scenario, name) for scenario in EXPECTED for name, _ in selected]
    if completed != required:
        raise QualificationError("coroutine graph qualification is incomplete")
    print(f"coroutine graph native qualification: PASS (graphs=2, cases=6/provider, providers={len(selected)})")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--writer", type=Path, required=True, action="append")
    args = parser.parse_args(argv)
    if len(args.writer) != 1:
        parser.error("--writer must be specified exactly once")
    try:
        qualify(args.writer[0])
    except (OSError, UnicodeError, ValueError, QualificationError, subprocess.TimeoutExpired) as error:
        print(f"coroutine graph native qualification: FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
