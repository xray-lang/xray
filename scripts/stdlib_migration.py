#!/usr/bin/env python3
"""Validate and execute task-196 stdlib migration contracts."""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Any

from stdlib_manifest import load_manifest, load_toml


REQUIRED_EQUIVALENCE = {"value", "error", "effect", "complexity"}
LEGACY_CLASSIFICATIONS = {"required", "bug", "accidental", "removed"}
LEGACY_ORACLE_MODES = {"classification_only", "executable"}
DIFF_BACKENDS = {"vm", "aot"}
CONTRACT_ROOT = Path("tests/stdlib/contracts")


def read_jsonl(path: Path) -> tuple[list[dict[str, Any]], list[str]]:
    rows: list[dict[str, Any]] = []
    errors: list[str] = []
    if not path.is_file():
        return rows, [f"missing JSONL corpus: {path}"]
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        try:
            row = json.loads(raw)
        except json.JSONDecodeError as exc:
            errors.append(f"{path}:{lineno}: invalid JSON: {exc}")
            continue
        if not isinstance(row, dict):
            errors.append(f"{path}:{lineno}: row must be a JSON object")
            continue
        rows.append(row)
    return rows, errors


def load_contract(root: Path, module: str) -> tuple[Path, dict[str, Any]]:
    path = root / CONTRACT_ROOT / module / "contract.toml"
    if not path.is_file():
        raise RuntimeError(f"missing stdlib migration contract: {path}")
    return path, load_toml(path)


def validate_contract(root: Path, module: str) -> tuple[list[str], dict[str, Any]]:
    path, contract = load_contract(root, module)
    errors: list[str] = []
    if contract.get("schema") != 1:
        errors.append(f"{path}: schema must be 1")
    if contract.get("module") != module:
        errors.append(f"{path}: module must be {module!r}")
    legacy = str(contract.get("legacy_commit", ""))
    if not re.fullmatch(r"[0-9a-f]{40}", legacy):
        errors.append(f"{path}: legacy_commit must be a full commit id")
    elif subprocess.run(
        ["git", "cat-file", "-e", f"{legacy}^{{commit}}"], cwd=root, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    ).returncode:
        errors.append(f"{path}: legacy_commit is not available in this repository: {legacy}")
    legacy_oracle = str(contract.get("legacy_oracle", ""))
    if legacy_oracle not in LEGACY_ORACLE_MODES:
        errors.append(
            f"{path}: legacy_oracle must be one of {', '.join(sorted(LEGACY_ORACLE_MODES))}"
        )
    if legacy_oracle == "executable":
        legacy_manifest = contract.get("legacy_cases_manifest")
        legacy_manifest_path = root / str(legacy_manifest or "")
        if not legacy_manifest or not legacy_manifest_path.is_file():
            errors.append(
                f"{path}: executable legacy oracle requires legacy_cases_manifest"
            )
        else:
            legacy_rows, legacy_errors = read_jsonl(legacy_manifest_path)
            errors.extend(legacy_errors)
            behavior_classes = {
                str(row.get("id", "")): str(row.get("classification", ""))
                for row in contract.get("legacy_behavior", ())
            }
            legacy_cases: set[str] = set()
            for index, row in enumerate(legacy_rows, 1):
                case = str(row.get("case", ""))
                if case in legacy_cases:
                    errors.append(f"{legacy_manifest_path}: duplicate legacy case {case!r}")
                legacy_cases.add(case)
                if case not in behavior_classes:
                    errors.append(f"{legacy_manifest_path}: unknown legacy behavior {case!r}")
                if row.get("classification") != behavior_classes.get(case):
                    errors.append(
                        f"{legacy_manifest_path}: row {index} classification does not match contract"
                    )
                if row.get("legacy_commit") != legacy:
                    errors.append(
                        f"{legacy_manifest_path}: row {index} legacy_commit does not match contract"
                    )
                missing = [
                    field
                    for field in ("case", "outcome", "value", "error", "effects")
                    if field not in row
                ]
                if missing or row.get("outcome") not in {"value", "error"} or not isinstance(
                    row.get("effects"), dict
                ):
                    errors.append(f"{legacy_manifest_path}: row {index} is not canonical observation")
            required = {
                case for case, classification in behavior_classes.items() if classification == "required"
            }
            missing_required = sorted(required - legacy_cases)
            if missing_required:
                errors.append(
                    f"{legacy_manifest_path}: missing required legacy behaviors: "
                    f"{', '.join(missing_required)}"
                )
        for probe_name in ("legacy.xr", "current.xr"):
            probe = path.parent / "probes" / probe_name
            if not probe.is_file():
                errors.append(f"{path}: executable legacy oracle requires {probe}")
    equivalence = set(contract.get("equivalence", ()))
    if equivalence != REQUIRED_EQUIVALENCE:
        errors.append(
            f"{path}: equivalence must be exactly {', '.join(sorted(REQUIRED_EQUIVALENCE))}"
        )
    backends = contract.get("backends", ["vm", "aot"])
    if (
        not isinstance(backends, list)
        or not backends
        or len(set(backends)) != len(backends)
        or not set(backends) <= DIFF_BACKENDS
    ):
        errors.append(f"{path}: backends must be a non-empty unique subset of vm,aot")
    manifest_rel = contract.get("diff_cases_manifest")
    manifest_path = root / str(manifest_rel or "")
    if not manifest_rel or not manifest_path.is_file():
        errors.append(f"{path}: diff_cases_manifest does not exist: {manifest_rel}")
    else:
        for lineno, raw in enumerate(manifest_path.read_text(encoding="utf-8").splitlines(), 1):
            case = raw.strip()
            if not case or case.startswith("#"):
                continue
            if not (root / case).is_file():
                errors.append(f"{manifest_path}:{lineno}: diff case does not exist: {case}")
    cases_path = path.parent / "cases.jsonl"
    rows, row_errors = read_jsonl(cases_path)
    errors.extend(row_errors)
    seen: set[str] = set()
    for index, row in enumerate(rows, 1):
        case_id = str(row.get("case", ""))
        if not case_id or case_id in seen:
            errors.append(f"{cases_path}: row {index} has missing or duplicate case id {case_id!r}")
        seen.add(case_id)
        row_equivalence = set(row.get("equivalence", ()))
        if not row_equivalence or not row_equivalence <= REQUIRED_EQUIVALENCE:
            errors.append(f"{cases_path}: row {index} has invalid equivalence categories")
        if not row.get("oracle"):
            errors.append(f"{cases_path}: row {index} must name an independent oracle")
    required_lists = ("legacy_public_surface", "intentional_semantic_changes", "known_legacy_bugs")
    for field in required_lists:
        if not isinstance(contract.get(field), list):
            errors.append(f"{path}: {field} must be an explicit list")
    behaviors = contract.get("legacy_behavior", ())
    if not behaviors:
        errors.append(f"{path}: at least one legacy_behavior classification is required")
    behavior_ids: set[str] = set()
    for behavior in behaviors:
        behavior_id = str(behavior.get("id", ""))
        if not behavior_id or behavior_id in behavior_ids:
            errors.append(f"{path}: legacy_behavior id is missing or duplicated: {behavior_id!r}")
        behavior_ids.add(behavior_id)
        if behavior.get("classification") not in LEGACY_CLASSIFICATIONS:
            errors.append(
                f"{path}: legacy_behavior {behavior_id!r} has invalid classification "
                f"{behavior.get('classification')!r}"
            )
        if not behavior.get("new_behavior"):
            errors.append(f"{path}: legacy_behavior {behavior_id!r} must state new_behavior")
    return errors, contract


def contract_modules(root: Path) -> list[str]:
    base = root / CONTRACT_ROOT
    return sorted(path.parent.name for path in base.glob("*/contract.toml"))


def find_xray(root: Path, value: str | None) -> Path:
    candidates = [Path(value)] if value else []
    candidates.extend((root / "build/xray", root / "build-release/xray"))
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    raise RuntimeError("xray executable not found; pass --xray or build the xray target")


def run_diff(root: Path, contract: dict[str, Any], xray: Path) -> int:
    backends = contract.get("backends", ["vm", "aot"])
    if backends == ["vm"]:
        return run_vm_cases(root, contract, xray)
    env = os.environ.copy()
    env["XRAY_DIFF_CASES_FILE"] = str(root / str(contract["diff_cases_manifest"]))
    env["XRAY_DIFF_EXTRA_CASES_FILE"] = ""
    env["XRAY_DIFF_BACKENDS"] = ",".join(contract.get("backends", ["vm", "aot"]))
    # The Python runner is the canonical fast path used by the shell wrapper,
    # and is directly executable on every supported host (including Windows,
    # where invoking a .sh file through CreateProcess fails with WinError 193).
    if os.name == "nt":
        env.setdefault("PYTHONUTF8", "1")
    return subprocess.run(
        [sys.executable, str(root / "tests/diff/run_backend_diff_fast.py"), str(xray)],
        cwd=root,
        env=env,
    ).returncode


def run_vm_cases(root: Path, contract: dict[str, Any], xray: Path) -> int:
    """Execute an explicitly VM-only contract without silently skipping it."""
    manifest = root / str(contract["diff_cases_manifest"])
    for lineno, raw in enumerate(manifest.read_text(encoding="utf-8").splitlines(), 1):
        case = raw.strip()
        if not case or case.startswith("#"):
            continue
        case_path = root / case
        expected_path = Path(f"{case_path}.expected")
        if not expected_path.is_file():
            print(
                f"{manifest}:{lineno}: VM-only contract case requires {expected_path.name}",
                file=sys.stderr,
            )
            return 1
        args_path = Path(f"{case_path}.args")
        extra_args: list[str] = []
        if args_path.is_file():
            lines = args_path.read_text(encoding="utf-8").splitlines()
            extra_args = shlex.split(lines[0]) if lines else []
        stdin_path = Path(f"{case_path}.stdin")
        stdin = stdin_path.read_bytes() if stdin_path.is_file() else None
        result = subprocess.run(
            [str(xray), "run", str(case_path), *extra_args],
            cwd=root,
            input=stdin,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        expected = expected_path.read_bytes()
        if result.returncode != 0 or result.stdout != expected:
            print(f"FAIL vm {case}", file=sys.stderr)
            print(f"  exit: {result.returncode}", file=sys.stderr)
            if result.stdout != expected:
                print(f"  expected stdout: {expected!r}", file=sys.stderr)
                print(f"  actual stdout:   {result.stdout!r}", file=sys.stderr)
            if result.stderr:
                print(result.stderr.decode("utf-8", errors="replace"), file=sys.stderr)
            return 1
        print(f"PASS vm {case}")
    return 0


def run_legacy_oracle(root: Path, module: str, xray: Path) -> int:
    return subprocess.run(
        [
            sys.executable,
            str(root / "scripts/stdlib_legacy_oracle.py"),
            "verify",
            module,
            "--root",
            str(root),
            "--xray",
            str(xray),
        ],
        cwd=root,
    ).returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("check", "verify"))
    parser.add_argument("module", nargs="?")
    parser.add_argument("--root", default=".")
    parser.add_argument("--xray")
    parser.add_argument("--metadata-only", action="store_true")
    args = parser.parse_args()
    root = Path(args.root).resolve()
    modules = [args.module] if args.module else contract_modules(root)
    if not modules:
        print("no stdlib migration contracts found", file=sys.stderr)
        return 1
    boundary = load_manifest(root)
    errors: list[str] = []
    contracts: dict[str, dict[str, Any]] = {}
    for module in modules:
        if module not in boundary.by_name:
            errors.append(f"migration contract module is absent from stdlib boundary: {module}")
            continue
        module_errors, contract = validate_contract(root, module)
        errors.extend(module_errors)
        contracts[module] = contract
    if errors:
        print("stdlib migration contract check failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    if args.command == "verify" and not args.metadata_only:
        try:
            xray = find_xray(root, args.xray)
        except RuntimeError as exc:
            print(str(exc), file=sys.stderr)
            return 1
        for module in modules:
            mode = contracts[module]["legacy_oracle"]
            print(f"== stdlib backend convergence: {module} (legacy_oracle={mode}) ==")
            if mode == "executable" and run_legacy_oracle(root, module, xray):
                return 1
            if run_diff(root, contracts[module], xray):
                return 1
    executable = sum(1 for contract in contracts.values() if contract["legacy_oracle"] == "executable")
    print(
        f"OK: {len(modules)} stdlib contract(s) are consistent; "
        f"{executable} have executable legacy oracles"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
