#!/usr/bin/env python3
"""Capture and verify executable legacy observations for stdlib contracts."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path
from typing import Any

from stdlib_manifest import load_toml


CONTRACT_ROOT = Path("tests/stdlib/contracts")
OBSERVATION_FIELDS = ("case", "outcome", "value", "error", "effects")


def canonical_observation(row: dict[str, Any]) -> dict[str, Any]:
    missing = [field for field in OBSERVATION_FIELDS if field not in row]
    if missing:
        raise ValueError(f"observation is missing fields: {', '.join(missing)}")
    case = row["case"]
    outcome = row["outcome"]
    effects = row["effects"]
    error = row["error"]
    if not isinstance(case, str) or not case:
        raise ValueError("observation case must be a non-empty string")
    if outcome not in {"value", "error"}:
        raise ValueError(f"observation {case!r} has invalid outcome {outcome!r}")
    if error is not None and not isinstance(error, dict):
        raise ValueError(f"observation {case!r} error must be null or an object")
    if not isinstance(effects, dict):
        raise ValueError(f"observation {case!r} effects must be an object")
    return {field: row[field] for field in OBSERVATION_FIELDS}


def parse_observations(raw: bytes, source: str) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    seen: set[str] = set()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ValueError(f"{source}: output is not UTF-8 JSONL: {exc}") from exc
    for lineno, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ValueError(f"{source}:{lineno}: invalid observation JSON: {exc}") from exc
        if not isinstance(value, dict):
            raise ValueError(f"{source}:{lineno}: observation must be an object")
        row = canonical_observation(value)
        case = str(row["case"])
        if case in seen:
            raise ValueError(f"{source}:{lineno}: duplicate observation case {case!r}")
        seen.add(case)
        rows.append(row)
    if not rows:
        raise ValueError(f"{source}: probe emitted no observations")
    return rows


def run_probe(xray: Path, root: Path, probe: Path) -> list[dict[str, Any]]:
    result = subprocess.run(
        [str(xray), "run", str(probe)],
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        stdout = result.stdout.decode("utf-8", errors="replace")
        stderr = result.stderr.decode("utf-8", errors="replace")
        raise RuntimeError(
            f"probe failed ({probe}, exit {result.returncode}):\n"
            f"stdout:\n{stdout}\n"
            f"stderr:\n{stderr}"
        )
    return parse_observations(result.stdout, str(probe))


CHECKED_IN_GENERATED = (
    Path("src/ir/xi_ops_gen.h"),
    Path("src/aot/xi_to_c_dispatch_gen.h"),
    Path("src/aot/xaot_rep_gen.h"),
)


def snapshot_checked_in_generated(checkout: Path) -> dict[Path, bytes]:
    """Snapshot generated tables that are part of the historical revision."""
    return {
        relative: (checkout / relative).read_bytes()
        for relative in CHECKED_IN_GENERATED
        if (checkout / relative).is_file()
    }


def restore_checked_in_generated(checkout: Path, snapshot: dict[Path, bytes]) -> None:
    """Undo configure-time regeneration and make committed tables newest."""
    for relative, contents in snapshot.items():
        generated = checkout / relative
        generated.write_bytes(contents)
        os.utime(generated, None)


def load_contract(root: Path, module: str) -> tuple[Path, dict[str, Any]]:
    directory = root / CONTRACT_ROOT / module
    path = directory / "contract.toml"
    if not path.is_file():
        raise RuntimeError(f"missing stdlib contract: {path}")
    return directory, load_toml(path)


def contract_modules(root: Path) -> list[str]:
    return sorted(path.parent.name for path in (root / CONTRACT_ROOT).glob("*/contract.toml"))


def executable_contract_modules(root: Path) -> list[str]:
    modules: list[str] = []
    for module in contract_modules(root):
        _, contract = load_contract(root, module)
        if contract.get("legacy_oracle") == "executable":
            modules.append(module)
    return modules


def behavior_classifications(contract: dict[str, Any]) -> dict[str, str]:
    return {
        str(row.get("id", "")): str(row.get("classification", ""))
        for row in contract.get("legacy_behavior", [])
    }


def verify_module(root: Path, module: str, xray: Path) -> None:
    directory, contract = load_contract(root, module)
    if contract.get("legacy_oracle") != "executable":
        raise RuntimeError(f"{module}: legacy_oracle is not executable")
    expected_path = root / str(contract["legacy_cases_manifest"])
    expected_raw = expected_path.read_bytes()
    expected = parse_observations(expected_raw, str(expected_path))
    expected_payload = {
        str(row["case"]): canonical_observation(row)
        for row in (
            json.loads(line)
            for line in expected_raw.decode("utf-8").splitlines()
            if line.strip()
        )
        if row.get("classification") == "required"
    }
    probe = directory / "probes/current.xr"
    actual = {str(row["case"]): row for row in run_probe(xray, root, probe)}
    missing = sorted(set(expected_payload) - set(actual))
    if missing:
        raise RuntimeError(f"{module}: current probe missed required cases: {', '.join(missing)}")
    for case, expected_row in expected_payload.items():
        if actual[case] != expected_row:
            raise RuntimeError(
                f"{module}: legacy mismatch for {case}:\n"
                f"  expected {json.dumps(expected_row, sort_keys=True)}\n"
                f"  actual   {json.dumps(actual[case], sort_keys=True)}"
            )
    if not expected:
        raise RuntimeError(f"{module}: executable oracle has no captured observations")
    print(f"PASS legacy {module} ({len(expected_payload)} required observation(s))")


def configure_and_build(checkout: Path, jobs: int) -> Path:
    build = checkout / "build-stdlib-legacy-oracle"
    generated_snapshot = snapshot_checked_in_generated(checkout)
    configure = [
        "cmake",
        "-S",
        str(checkout),
        "-B",
        str(build),
        "-DBUILD_TESTS=OFF",
        "-DXR_BUILD_LSP=OFF",
        "-DXR_BUILD_DAP=OFF",
        "-DXR_BUILD_MCP=OFF",
        "-DXR_BUILD_TEST_MODULES=ON",
    ]
    subprocess.run(configure, cwd=checkout, check=True)
    # Configure can eagerly rewrite these tables before the build graph runs.
    # Restore the exact committed bytes, then make them newer than dependency
    # stamps so the historical binary is not a mixed-version reconstruction.
    restore_checked_in_generated(checkout, generated_snapshot)
    subprocess.run(
        ["cmake", "--build", str(build), "--target", "xray", f"-j{jobs}"],
        cwd=checkout,
        check=True,
    )
    xray = build / "xray"
    if not xray.is_file():
        raise RuntimeError(f"legacy build did not produce {xray}")
    return xray


def capture_group(
    root: Path,
    commit: str,
    modules: list[str],
    output_dir: Path,
    jobs: int,
) -> None:
    temp = Path(tempfile.mkdtemp(prefix="xray-stdlib-legacy-"))
    checkout = temp / "checkout"
    added = False
    try:
        subprocess.run(
            ["git", "worktree", "add", "--detach", str(checkout), commit],
            cwd=root,
            check=True,
        )
        added = True
        # Historical commits intentionally capture their checked-in generated
        # compiler tables. Some old revisions have a newer ops.def but no
        # matching enum update, so regenerating xi_ops_gen.h makes that exact
        # revision unbuildable and no longer represents the committed binary.
        xray = configure_and_build(checkout, jobs)
        for module in modules:
            directory, contract = load_contract(root, module)
            probe = checkout / f".stdlib-legacy-{module}.xr"
            shutil.copyfile(directory / "probes/legacy.xr", probe)
            rows = run_probe(xray, checkout, probe)
            classifications = behavior_classifications(contract)
            output = output_dir / f"{module}.jsonl"
            output.parent.mkdir(parents=True, exist_ok=True)
            with output.open("w", encoding="utf-8") as handle:
                for row in rows:
                    case = str(row["case"])
                    if case not in classifications:
                        raise RuntimeError(f"{module}: unknown legacy behavior case {case!r}")
                    captured = {
                        **row,
                        "classification": classifications[case],
                        "legacy_commit": commit,
                    }
                    handle.write(json.dumps(captured, sort_keys=True, separators=(",", ":")) + "\n")
            print(f"CAPTURED {module} -> {output}")
    finally:
        if added:
            subprocess.run(
                ["git", "worktree", "remove", "--force", str(checkout)],
                cwd=root,
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        shutil.rmtree(temp, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("capture", "verify"))
    parser.add_argument("module", nargs="?")
    parser.add_argument("--root", default=".")
    parser.add_argument("--xray")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--jobs", type=int, default=max(1, min(os.cpu_count() or 1, 8)))
    args = parser.parse_args()
    root = Path(args.root).resolve()
    modules = [args.module] if args.module else executable_contract_modules(root)
    try:
        if args.command == "verify":
            if not args.xray:
                raise RuntimeError("verify requires --xray")
            xray = Path(args.xray).resolve()
            for module in modules:
                verify_module(root, module, xray)
            return 0
        if not args.output_dir:
            raise RuntimeError("capture requires --output-dir")
        groups: dict[str, list[str]] = defaultdict(list)
        for module in modules:
            _, contract = load_contract(root, module)
            groups[str(contract["legacy_commit"])].append(module)
        output_dir = args.output_dir.resolve()
        for commit, group in groups.items():
            capture_group(root, commit, group, output_dir, args.jobs)
        return 0
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"stdlib legacy oracle failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
