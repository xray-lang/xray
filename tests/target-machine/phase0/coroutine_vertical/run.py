#!/usr/bin/env python3
"""Exercise the existing compiler through a complete coroutine vertical slice."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
import tomllib
from pathlib import Path
from typing import Any


if sys.version_info < (3, 11):
    raise SystemExit("coroutine vertical runner requires Python 3.11 or newer")


SCHEMA = 1
RUNNER = "coroutine-vertical-calibration/1"


def capture(command: list[str], cwd: Path, timeout: int = 300) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=timeout, check=False, env={**os.environ, "NO_COLOR": "1"}
    )


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def safe_id(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]", "_", value)


def compiler_identity(root: Path, xray: Path) -> dict[str, Any]:
    version_result = capture([str(xray), "--version", "--json"], root, 30)
    git_result = capture(["git", "rev-parse", "HEAD"], root, 30)
    status_result = capture(["git", "status", "--porcelain=v1"], root, 30)
    if version_result.returncode != 0 or git_result.returncode != 0 or status_result.returncode != 0:
        raise RuntimeError("cannot establish compiler/source identity")
    version = json.loads(version_result.stdout)
    return {
        "git_commit": git_result.stdout.strip(),
        "git_dirty": bool(status_result.stdout.strip()),
        "compiler_version": version,
        "binary_sha256": digest(xray),
    }


def extract_state_tables(generated_c: str) -> list[list[int]]:
    tables = []
    for match in re.finditer(r"switch\s*\(f->state\)\s*\{(.*?)\n\s*\}", generated_c, re.S):
        states = [int(value) for value in re.findall(r"case\s+([0-9]+)\s*:", match.group(1))]
        if states:
            tables.append(states)
    return tables


def verify_generated_shape(generated_c: str, plan_dump: str,
                           required: set[str]) -> tuple[dict[str, Any], list[str]]:
    errors: list[str] = []
    tables = extract_state_tables(generated_c)
    if not tables:
        errors.append("XR_CORO_4000: no entry state dispatch")
    for states in tables:
        if states != list(range(min(states), max(states) + 1)) or states[0] != 0:
            errors.append(f"XR_CORO_4000: sparse state table {states}")

    frame_count = len(re.findall(r"typedef\s+struct\s+[A-Za-z0-9_]+_aot_frame\s*\{", generated_c))
    root_tables = len(re.findall(r"\.root_count\s*=\s*[0-9]+", generated_c))
    drop_tables = len(re.findall(r"\.release_count\s*=\s*[0-9]+", generated_c))
    trace_functions = len(re.findall(r"\.trace_roots\s*=\s*[A-Za-z0-9_]+", generated_c))
    release_functions = len(re.findall(r"\.release_frame\s*=\s*[A-Za-z0-9_]+", generated_c))
    if root_tables < frame_count or trace_functions < frame_count:
        errors.append("XR_CORO_4002: incomplete root table")
    if drop_tables < frame_count or release_functions < frame_count:
        errors.append("XR_CORO_4002: incomplete drop table")
    if "root=resumable_frame" not in plan_dump:
        errors.append("XR_CORO_4000: entry plan is not a resumable frame")

    facts = {
        "frame_count": frame_count,
        "state_tables": tables,
        "root_table_count": root_tables,
        "drop_table_count": drop_tables,
        "trace_function_count": trace_functions,
        "release_function_count": release_functions,
        "cleanup_table": bool(
            "cleanup_entries" in generated_c
            or "xr_aot_scope_exit" in generated_c
            or re.search(r"\.run_pending_cleanup\s*=\s*(?!NULL)[A-Za-z0-9_]+", generated_c)
        ),
        "child_edge": bool(re.search(r"child|continuation|_call_.*_frame", generated_c)),
        "select_edge": "select" in generated_c.lower(),
        "typed_slot": bool(re.search(r"XrSlotRef|xr_slot_|int64_t\s+v[0-9]+", generated_c)),
        "error_edge": "error" in generated_c.lower(),
        "cancel_edge": "cancel" in generated_c.lower(),
        "module_root": "entry-plan" in plan_dump and "root=resumable_frame" in plan_dump,
    }
    checks = {
        "cleanup": facts["cleanup_table"],
        "child": facts["child_edge"],
        "select": facts["select_edge"],
        "typed-slot": facts["typed_slot"],
        "error": facts["error_edge"],
        "cancel": facts["cancel_edge"],
        "module-root": facts["module_root"],
        "state": bool(tables),
        "root": root_tables >= frame_count and trace_functions >= frame_count,
        "drop": drop_tables >= frame_count and release_functions >= frame_count,
    }
    for requirement in sorted(required):
        if not checks.get(requirement, False):
            errors.append(f"missing required generated fact: {requirement}")
    return facts, errors


def verify_model(model: dict[str, Any]) -> list[str]:
    errors = []
    states = model["states"]
    if states != list(range(0, len(states))):
        errors.append("XR_CORO_4000")
    for value in model["live_across"]:
        if value not in model["spills"]:
            errors.append("XR_CORO_4001")
        if value.startswith("ref:") and value not in model["roots"]:
            errors.append("XR_CORO_4002")
        if value.startswith("owned:") and value not in model["drops"]:
            errors.append("XR_CORO_4002")
    if any(child not in model["valid_children"] for child in model["child_edges"]):
        errors.append("XR_CORO_4003")
    return sorted(set(errors))


def mutation_evidence() -> list[dict[str, Any]]:
    base = {
        "states": [0, 1, 2],
        "live_across": ["ref:message", "owned:array"],
        "spills": {"ref:message": 0, "owned:array": 8},
        "roots": ["ref:message"],
        "drops": ["owned:array"],
        "valid_children": ["child:worker"],
        "child_edges": ["child:worker"],
    }
    mutations = []

    def mutated(name: str, change: Any, expected: str) -> None:
        model = json.loads(json.dumps(base))
        change(model)
        diagnostics = verify_model(model)
        mutations.append({
            "name": name, "expected_diagnostic": expected,
            "diagnostics": diagnostics, "detected": expected in diagnostics,
        })

    assert verify_model(base) == []
    mutated("sparse-state", lambda row: row.update(states=[0, 2]), "XR_CORO_4000")
    mutated("missing-spill", lambda row: row["spills"].pop("owned:array"), "XR_CORO_4001")
    mutated("missing-root", lambda row: row.update(roots=[]), "XR_CORO_4002")
    mutated("missing-drop", lambda row: row.update(drops=[]), "XR_CORO_4002")
    mutated("invalid-child", lambda row: row.update(child_edges=["child:missing"]), "XR_CORO_4003")
    return mutations


def run_case(root: Path, xray: Path, output: Path, case: dict[str, Any]) -> dict[str, Any]:
    case_id = safe_id(case["id"])
    source = root / case["source"]
    generated = output / f"{case_id}.c"
    binary = output / f"{case_id}.bin"
    plan_log = output / f"{case_id}.plan.log"
    compile_log = output / f"{case_id}.compile.log"

    started = time.perf_counter()
    cgen = capture(
        [str(xray), "build", "--native", "-c", "-o", str(generated),
         "--dump-xaot-plan", str(source)], root
    )
    plan_log.write_text(cgen.stdout, encoding="utf-8")
    compile_log.write_text(cgen.stderr, encoding="utf-8")
    errors = []
    if cgen.returncode != 0:
        errors.append(f"generated-C build returned {cgen.returncode}")

    native_build = capture(
        [str(xray), "build", "--native", "-o", str(binary), str(source)], root
    )
    compile_log.write_text(
        compile_log.read_text(encoding="utf-8") + native_build.stdout + native_build.stderr,
        encoding="utf-8",
    )
    if native_build.returncode != 0:
        errors.append(f"native build returned {native_build.returncode}")

    vm_run = capture([str(xray), "run", str(source)], root, 120)
    native_run = capture([str(binary)], root, 120) if binary.is_file() else None
    expected = case["expected_stdout"]
    if vm_run.returncode != 0 or vm_run.stdout != expected:
        errors.append("VM result does not satisfy independent exact oracle")
    if native_run is None or native_run.returncode != 0 or native_run.stdout != expected:
        errors.append("AOT result does not satisfy independent exact oracle")

    facts: dict[str, Any] = {}
    if generated.is_file():
        facts, shape_errors = verify_generated_shape(
            generated.read_text(encoding="utf-8"), cgen.stdout,
            set(case.get("required_facts", [])),
        )
        errors.extend(shape_errors)
    return {
        "id": case["id"],
        "source": case["source"],
        "source_sha256": digest(source),
        "status": "passed" if not errors else "failed",
        "errors": errors,
        "required_facts": case.get("required_facts", []),
        "facts": facts,
        "vm": {"returncode": vm_run.returncode, "stdout": vm_run.stdout, "stderr": vm_run.stderr},
        "aot": {
            "returncode": native_run.returncode if native_run else None,
            "stdout": native_run.stdout if native_run else "",
            "stderr": native_run.stderr if native_run else "binary missing",
        },
        "generated_c_sha256": digest(generated) if generated.is_file() else None,
        "plan_log_sha256": digest(plan_log),
        "duration_seconds": round(time.perf_counter() - started, 6),
    }


def self_test() -> int:
    mutations = mutation_evidence()
    assert all(row["detected"] for row in mutations)
    sample = "switch (f->state) {\ncase 0: break;\ncase 1: goto S1;\n}\n"
    assert extract_state_tables(sample) == [[0, 1]]
    print("coroutine vertical runner self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    parser.add_argument("--xray", default="build/xray")
    parser.add_argument("--output-dir", default="build/target-machine/phase0/coroutine-vertical")
    parser.add_argument("--report")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    root = Path(args.root).resolve()
    xray = (root / args.xray).resolve() if not Path(args.xray).is_absolute() else Path(args.xray)
    output = (root / args.output_dir).resolve() if not Path(args.output_dir).is_absolute() else Path(args.output_dir)
    output.mkdir(parents=True, exist_ok=True)
    manifest_path = Path(__file__).with_name("manifest.toml")
    manifest = tomllib.loads(manifest_path.read_text(encoding="utf-8"))
    cases = []
    for entry in manifest.get("case", []):
        print(f"[coroutine-vertical] {entry['id']}", flush=True)
        cases.append(run_case(root, xray, output, entry))
    mutations = mutation_evidence()
    passed = all(row["status"] == "passed" for row in cases) and all(
        row["detected"] for row in mutations
    )
    report = {
        "schema": SCHEMA,
        "runner": RUNNER,
        "source": compiler_identity(root, xray),
        "oracle": manifest["oracle"],
        "result": "passed" if passed else "failed",
        "cases": cases,
        "negative_mutations": mutations,
    }
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    report_path = output / "report.json"
    report_path.write_text(rendered, encoding="utf-8")
    if args.report:
        destination = (root / args.report).resolve() if not Path(args.report).is_absolute() else Path(args.report)
        destination.write_text(rendered, encoding="utf-8")
    print(f"coroutine vertical calibration: {report['result'].upper()} ({report_path})")
    if not passed:
        for case in cases:
            for error in case["errors"]:
                print(f"  {case['id']}: {error}", file=sys.stderr)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
