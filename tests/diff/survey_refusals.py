#!/usr/bin/env python3
"""Generate the current-binary live refusal/root-cause manifest.

The differential runner remains the sole owner of case discovery and the
pass/fail/refused/skip verdict. This generator enables its source-side refusal
evidence, retains the complete refused build log, and freezes the identity of
the clean source tree, matching compiler, toolchain provider, profile and
artifact that produced the measurement.

The output is qualification evidence, not a baseline or waiver. A dirty tree,
stale binary, missing case, differential failure, unparsable refusal, missing
toolchain provider or empty selection fails closed.
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

import run_backend_diff as backend_diff


ROOT = Path(__file__).resolve().parents[2]
SCHEMA = 1
KIND = "xray-live-refusal-root-cause"
GENERATOR = "tests/diff/survey_refusals.py"
SURVEY = re.compile(
    r"^\[refusal-survey\] owner=([a-z0-9-]+) family=([^\s]+)(?:\s+(.*))?$"
)
DIAGNOSTIC = re.compile(r"\b(XR_[A-Z0-9_]+):\s*([^\r\n]+)")
OWNER_VALUES = {
    "semantic-plan-verifier",
    "target-plan-builder",
    "aot-representation-refinement",
}


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_identity_owner() -> Any:
    path = ROOT / "tests/target-machine/phase0/run_baseline.py"
    spec = importlib.util.spec_from_file_location("xray_target_machine_identity", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load compiler identity owner: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def canonical_path(path: Path) -> str:
    return backend_diff.canonical_path_text(path.resolve().relative_to(ROOT.resolve()))


def file_row(path: Path) -> dict[str, Any]:
    return {
        "path": canonical_path(path),
        "sha256": sha256_file(path),
        "size_bytes": path.stat().st_size,
    }


def optional_file_row(path: Path) -> dict[str, Any] | None:
    return file_row(path) if path.is_file() else None


def read_baseline(path: Path) -> set[str]:
    if not path.is_file():
        raise RuntimeError(f"governed baseline missing: {canonical_path(path)}")
    rows: set[str] = set()
    for raw in path.read_text(encoding="utf-8", errors="strict").splitlines():
        value = raw.split("#", 1)[0].strip().replace("\\", "/")
        if not value:
            continue
        if value in rows:
            raise RuntimeError(f"duplicate governed baseline row: {value}")
        rows.add(value)
    return rows


def case_input(case: Path, refusal_baseline: set[str], divergence_baseline: set[str]) -> dict[str, Any]:
    expected = Path(str(case) + ".expected")
    args = case.with_suffix(".args")
    stdin = case.with_suffix(".stdin")
    project = case.parent / "xray.toml"
    name = canonical_path(case)
    aot_reject = backend_diff.read_first_directive(case, "// diff-aot-reject: ", 5)
    diff_backends = backend_diff.read_first_directive(case, "// diff-backends: ", 5)
    oracle: dict[str, Any]
    if aot_reject:
        oracle = {
            "kind": "vm-plus-native-rejection",
            "asset": optional_file_row(expected),
            "native_diagnostic": aot_reject,
        }
    elif expected.is_file():
        oracle = {"kind": "checked-in-stdout", "asset": file_row(expected)}
    else:
        oracle = {"kind": "differential-vm", "asset": None}
    return {
        "path": name,
        "source": file_row(case),
        "args": optional_file_row(args),
        "stdin": optional_file_row(stdin),
        "project": optional_file_row(project),
        "oracle": oracle,
        "diff_backends": [value.strip() for value in diff_backends.split(",") if value.strip()],
        "listed_refusal": name in refusal_baseline,
        "listed_divergence": name in divergence_baseline,
    }


def blocking_fact(family: str, detail: str) -> str:
    """Derive the stable blocking fact from source-emitted authority facts."""
    parts = [family]
    message = re.search(
        r"(XR_[A-Z0-9_]+: [a-z][^\n]*?)(?:\s+[a-z][a-z0-9-]*=|$)", detail
    )
    if message:
        parts.append(message.group(1).strip())
    for label, pattern in (
        ("selector", r"\bselector=([^\s]+)"),
        ("definer-opcode", r"\bdefiner-opcode=(\d+)"),
        ("use-opcode", r"\buse-opcode=(\d+)"),
        ("opcode", r"(?<!-)\bopcode=(\d+)"),
        ("value-machine", r"\bvalue-machine=(\d+)"),
        ("value-shape", r"\bvalue-shape=(\d+)"),
        ("value-flags", r"\bvalue-flags=(\d+)"),
        ("parameter-ordinal", r"\bparameter-ordinal=(\d+)"),
        ("storage-mask", r"\bstorage-mask=(\d+)"),
        ("operand-mode", r"\boperand-mode=(\d+)"),
        ("parameter-mode", r"\bparameter-mode=(\d+)"),
        ("operand-transfer", r"\boperand-transfer=(\d+)"),
        ("parameter-transfer", r"\bparameter-transfer=(\d+)"),
        ("operand-ownership", r"\boperand-ownership=(\d+)"),
        ("parameter-ownership", r"\bparameter-ownership=(\d+)"),
        ("operand-access", r"\boperand-access=(\d+)"),
        ("operand-role", r"\boperand-role=(\d+)"),
        ("expected-role", r"\bexpected-role=(\d+)"),
        ("type-match", r"(?<!-)\btype-match=(\d+)"),
        ("ordinal-match", r"\bordinal-match=(\d+)"),
        ("contract-flag", r"\bcontract-flag=(\d+)"),
        ("addressable", r"\baddressable=(\d+)"),
        ("operand-count", r"\boperand-count=(\d+)"),
        ("parameter-count", r"\bparameter-count=(\d+)"),
        ("result-type-match", r"\bresult-type-match=(\d+)"),
        ("method", r"\bmethod=(\d+)"),
    ):
        match = re.search(pattern, detail)
        if match:
            parts.append(f"{label}={match.group(1)}")
    return "|".join(parts)


def parse_refusals(log: bytes) -> tuple[list[dict[str, str]], dict[str, str] | None]:
    rows: list[dict[str, str]] = []
    seen: set[tuple[str, str, str]] = set()
    text = log.decode("utf-8", errors="backslashreplace")
    for line in text.splitlines():
        match = SURVEY.fullmatch(line.strip())
        if not match:
            continue
        owner, family, detail = match.group(1), match.group(2), match.group(3) or ""
        if owner not in OWNER_VALUES:
            raise RuntimeError(f"unknown refusal owner {owner!r}")
        key = (owner, family, blocking_fact(family, detail))
        if key in seen:
            continue
        seen.add(key)
        rows.append({
            "owner": owner,
            "family": family,
            "blocking_fact": key[2],
        })
    diagnostic_match = DIAGNOSTIC.search(text)
    diagnostic = None
    if diagnostic_match:
        diagnostic = {
            "code": diagnostic_match.group(1),
            "message": diagnostic_match.group(2).strip(),
        }
    return rows, diagnostic


def toolchain_identity(binary: Path, timeout: int) -> dict[str, Any]:
    command = [
        str(binary), "toolchain", "doctor", "--target", "native",
        "--profile", "hosted", "--json",
    ]
    try:
        completed = subprocess.run(
            command, cwd=ROOT, text=True, encoding="utf-8", errors="strict",
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout, check=False,
        )
    except (OSError, subprocess.SubprocessError) as error:
        raise RuntimeError(f"toolchain doctor failed: {error}") from error
    if completed.returncode != 0:
        raise RuntimeError(
            f"toolchain doctor returned {completed.returncode}: "
            f"{(completed.stdout + completed.stderr).strip()}"
        )
    try:
        data = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"toolchain doctor output is not JSON: {error}") from error
    request = data.get("request", {})
    selection = data.get("selection", {})
    if data.get("schema") != 1 or request.get("target") != "native" \
            or request.get("profile") != "hosted" or not request.get("normalizedTarget") \
            or not selection.get("ready") or selection.get("fallbackUsed") \
            or not selection.get("provider") or not selection.get("runtimeArtifact"):
        raise RuntimeError("toolchain doctor did not prove an exact ready native hosted provider")
    probe = data.get("probe", {})
    return {
        "schema": data["schema"],
        "xray": data.get("xray"),
        "request": request,
        "selection": selection,
        "capabilities": data.get("capabilities"),
        "probe": {"fingerprint": probe.get("fingerprint")},
    }


def qualified_compiler_identity(identity_owner: Any, build: Path) -> dict[str, Any]:
    identity = identity_owner.compiler_identity(
        ROOT, build, {"residue": {"source_root_globs": []}}
    )
    values = identity_owner.cmake_identity_values(build)
    source = Path(values.get("CMAKE_HOME_DIRECTORY", "")).resolve()
    if values.get("CMAKE_GENERATOR") != "Ninja" \
            or values.get("CMAKE_BUILD_TYPE") != "Release" \
            or values.get("XRAY_STDLIB_VM_FASTPATHS") != "OFF" \
            or source != ROOT.resolve() \
            or identity.get("version", {}).get("buildProfile") != "Release":
        raise RuntimeError("compiler is not the exact source-root Ninja Release fastpaths-off build")
    identity["build"] = {
        "generator": "Ninja",
        "build_type": "Release",
        "stdlib_vm_fastpaths": "OFF",
        "source_root": "${SOURCE_ROOT}",
    }
    return identity


def build_config(build: Path, binary: Path, timeout: int) -> backend_diff.RunnerConfig:
    cache_root = build / "live-refusal-root-cause-cache"
    return backend_diff.RunnerConfig(
        xray=binary,
        backends=["vm", "aot"],
        jobs=1,
        aot_opt="0",
        aot_cache=cache_root / "aot-objects",
        aot_bin_cache=cache_root / "aot-binaries",
        embed_bin_cache=cache_root / "embed-binaries",
        diff_stderr=False,
        xi_opt="",
        case_timeout=float(timeout),
    )


def survey_case(order: int, case: Path, config: backend_diff.RunnerConfig) -> backend_diff.CaseResult:
    return backend_diff.run_case(config, order, case)


def generate(build: Path, output: Path, jobs: int, timeout: int) -> tuple[dict[str, Any], int]:
    log_root = output.with_name(output.name + ".logs")
    if output.exists() or log_root.exists():
        raise RuntimeError("output manifest and log directory must not already exist")
    identity_owner = load_identity_owner()
    identity = qualified_compiler_identity(identity_owner, build)
    binary = identity_owner.compiler_binary_path(build)
    provider = toolchain_identity(binary, timeout)

    extra_cases = str(backend_diff.SCRIPT_DIR / "coro_regression_cases.txt")
    cases = [
        path for path in backend_diff.collect_cases("", extra_cases)
        if path.is_file() and not path.name.startswith("_")
    ]
    if not cases:
        raise RuntimeError("governed backend-diff discovery returned zero runnable cases")
    canonical_cases = [canonical_path(path) for path in cases]
    if len(canonical_cases) != len(set(canonical_cases)):
        raise RuntimeError("governed backend-diff discovery contains duplicate cases")

    refusal_path = ROOT / "tests/diff/known_failures_not_comparable.txt"
    divergence_path = ROOT / "tests/diff/known_failures.txt"
    refusal_baseline = read_baseline(refusal_path)
    divergence_baseline = read_baseline(divergence_path)
    inputs = [case_input(case, refusal_baseline, divergence_baseline) for case in cases]
    missing_refusal_inputs = refusal_baseline - set(canonical_cases)
    if missing_refusal_inputs:
        raise RuntimeError(
            "refusal baseline contains cases outside governed discovery: "
            + ", ".join(sorted(missing_refusal_inputs))
        )

    config = build_config(build, binary, timeout)
    previous = os.environ.get("XRAY_COLLECT_ALL_REFUSALS")
    os.environ["XRAY_COLLECT_ALL_REFUSALS"] = "1"
    try:
        results: list[backend_diff.CaseResult] = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
            futures = [pool.submit(survey_case, index, case, config) for index, case in enumerate(cases)]
            for index, future in enumerate(futures, start=1):
                results.append(future.result())
                if index % 50 == 0:
                    print(f"  measured {index}/{len(cases)}", file=sys.stderr)
    finally:
        if previous is None:
            os.environ.pop("XRAY_COLLECT_ALL_REFUSALS", None)
        else:
            os.environ["XRAY_COLLECT_ALL_REFUSALS"] = previous

    log_root.mkdir(parents=True, exist_ok=True)
    manifest_results: list[dict[str, Any]] = []
    root_cases: dict[tuple[str, str, str], list[str]] = collections.defaultdict(list)
    invalid = False
    input_by_case = {row["path"]: row for row in inputs}
    for result in sorted(results, key=lambda row: row.order):
        outcome = result.status
        if result.status == "pass" \
                and input_by_case[result.name]["oracle"]["kind"] == "vm-plus-native-rejection":
            outcome = "expected-rejection"
        row: dict[str, Any] = {
            "case": result.name,
            "outcome": outcome,
            "first_refusal": None,
            "refusals": [],
            "diagnostic": None,
            "build_log": None,
        }
        if result.status == "refused":
            log = result.refusal_build_logs.get("aot", b"")
            if not log:
                invalid = True
            digest = sha256_bytes(log)
            log_name = f"{result.order:04d}-{digest[:16]}.log"
            log_path = log_root / log_name
            log_path.write_bytes(log)
            refusals, diagnostic = parse_refusals(log)
            if not refusals:
                invalid = True
            row["refusals"] = refusals
            row["first_refusal"] = refusals[0] if refusals else None
            row["diagnostic"] = diagnostic
            row["build_log"] = {
                "path": log_path.relative_to(output.parent).as_posix(),
                "sha256": digest,
                "size_bytes": len(log),
            }
            for refusal in refusals:
                key = (refusal["owner"], refusal["family"], refusal["blocking_fact"])
                root_cases[key].append(result.name)
        elif result.status == "fail":
            invalid = True
            row["diagnostic"] = {"code": "DIFFERENTIAL_FAILURE", "message": result.output}
        governed = input_by_case[result.name]
        manifest_results.append(row)

    counts = collections.Counter(row["outcome"] for row in manifest_results)
    refused_names = {row["case"] for row in manifest_results if row["outcome"] == "refused"}
    skipped_names = {row["case"] for row in manifest_results if row["outcome"] == "skip"}
    new_refusals = sorted(refused_names - refusal_baseline)
    resolved_refusals = sorted(refusal_baseline - refused_names - skipped_names)
    if new_refusals or resolved_refusals:
        invalid = True
    roots = [
        {
            "owner": owner,
            "family": family,
            "blocking_fact": fact,
            "case_count": len(case_names),
            "cases": sorted(case_names),
        }
        for (owner, family, fact), case_names in sorted(root_cases.items())
    ]
    manifest = {
        "schema": SCHEMA,
        "kind": KIND,
        "generator": GENERATOR,
        "status": "failed" if invalid else "passed",
        "identity": identity,
        "coverage": {
            "listed_refusal_count": len(refusal_baseline),
            "observed_refusal_count": len(refused_names),
            "new_refusals": new_refusals,
            "resolved_refusals": resolved_refusals,
        },
        "measurement": {
            "profile": "hosted",
            "artifact": "native-executable",
            "host_c_optimization": "0",
            "xi_optimization": "pipeline-default",
            "backends": ["vm", "aot"],
            "collect_all_refusals": True,
            "toolchain": provider,
        },
        "inputs": {
            "case_roots": ["tests/diff/cases"],
            "case_manifests": [file_row(backend_diff.SCRIPT_DIR / "coro_regression_cases.txt")],
            "case_count": len(inputs),
            "cases": inputs,
            "divergence_baseline": file_row(divergence_path),
            "refusal_baseline": file_row(refusal_path),
        },
        "results": manifest_results,
        "root_causes": roots,
        "summary": {
            "case_count": len(manifest_results),
            "comparable_count": counts["pass"],
            "expected_rejection_count": counts["expected-rejection"],
            "refused_count": counts["refused"],
            "skipped_count": counts["skip"],
            "failed_count": counts["fail"],
            "root_cause_count": len(roots),
        },
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest, 1 if invalid else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    parser.add_argument("--timeout", type=int, default=180)
    options = parser.parse_args()
    if options.jobs < 1 or options.timeout < 1:
        parser.error("--jobs and --timeout must be positive")
    try:
        manifest, status = generate(
            options.build.resolve(), options.output.resolve(), options.jobs, options.timeout
        )
    except (OSError, RuntimeError, ValueError) as error:
        print(f"live refusal manifest generation failed: {error}", file=sys.stderr)
        return 1
    summary = manifest["summary"]
    print(
        "live refusal manifest: "
        f"{manifest['status']} cases={summary['case_count']} "
        f"comparable={summary['comparable_count']} refused={summary['refused_count']} "
        f"expected-rejection={summary['expected_rejection_count']} "
        f"skipped={summary['skipped_count']} failed={summary['failed_count']} "
        f"new-refusals={len(manifest['coverage']['new_refusals'])} "
        f"resolved-refusals={len(manifest['coverage']['resolved_refusals'])}"
    )
    for label, field in (
        ("new refusal", "new_refusals"),
        ("listed refusal now building", "resolved_refusals"),
    ):
        for case in manifest["coverage"][field]:
            print(f"  {label}: {case}")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
