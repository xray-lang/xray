#!/usr/bin/env python3
"""Verify a current-binary live refusal/root-cause manifest fail closed."""

from __future__ import annotations

import argparse
import collections
import copy
import hashlib
import importlib.util
import json
import re
import sys
import tempfile
from pathlib import Path
from typing import Any


SCHEMA = 1
KIND = "xray-live-refusal-root-cause"
GENERATOR = "tests/diff/survey_refusals.py"
OWNERS = {
    "semantic-plan-verifier",
    "target-plan-builder",
    "aot-representation-refinement",
}
SHA256 = re.compile(r"[0-9a-f]{64}")
SURVEY_LINE = re.compile(
    r"^\[refusal-survey\]\s+owner=([a-z0-9-]+)\s+family=([^\s]+)(?:\s+(.*))?$"
)


def load_module(name: str, path: Path) -> Any:
    if str(path.parent) not in sys.path:
        sys.path.insert(0, str(path.parent))
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load governed owner: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def digest_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def digest_file(path: Path) -> str:
    return digest_bytes(path.read_bytes())


def canonical(root: Path, path: Path) -> str:
    return path.resolve().relative_to(root.resolve()).as_posix()


def file_identity(root: Path, path: Path) -> dict[str, Any]:
    return {
        "path": canonical(root, path),
        "sha256": digest_file(path),
        "size_bytes": path.stat().st_size,
    }


def optional_identity(root: Path, path: Path) -> dict[str, Any] | None:
    return file_identity(root, path) if path.is_file() else None


def baseline_rows(path: Path) -> set[str]:
    rows: set[str] = set()
    for raw in path.read_text(encoding="utf-8", errors="strict").splitlines():
        value = raw.split("#", 1)[0].strip().replace("\\", "/")
        if value:
            if value in rows:
                raise RuntimeError(f"duplicate baseline row: {value}")
            rows.add(value)
    return rows


def expected_inputs(root: Path, diff: Any) -> dict[str, Any]:
    refusal_path = root / "tests/diff/known_failures_not_comparable.txt"
    divergence_path = root / "tests/diff/known_failures.txt"
    refusal = baseline_rows(refusal_path)
    divergence = baseline_rows(divergence_path)
    extra_cases = str(diff.SCRIPT_DIR / "coro_regression_cases.txt")
    cases = [
        path for path in diff.collect_cases("", extra_cases)
        if path.is_file() and not path.name.startswith("_")
    ]
    rows = []
    for case in cases:
        name = canonical(root, case)
        expected = Path(str(case) + ".expected")
        rejection = diff.read_first_directive(case, "// diff-aot-reject: ", 5)
        backends = diff.read_first_directive(case, "// diff-backends: ", 5)
        if rejection:
            oracle = {
                "kind": "vm-plus-native-rejection",
                "asset": optional_identity(root, expected),
                "native_diagnostic": rejection,
            }
        elif expected.is_file():
            oracle = {"kind": "checked-in-stdout", "asset": file_identity(root, expected)}
        else:
            oracle = {"kind": "differential-vm", "asset": None}
        rows.append({
            "path": name,
            "source": file_identity(root, case),
            "args": optional_identity(root, case.with_suffix(".args")),
            "stdin": optional_identity(root, case.with_suffix(".stdin")),
            "project": optional_identity(root, case.parent / "xray.toml"),
            "oracle": oracle,
            "diff_backends": [value.strip() for value in backends.split(",") if value.strip()],
            "listed_refusal": name in refusal,
            "listed_divergence": name in divergence,
        })
    return {
        "case_roots": ["tests/diff/cases"],
        "case_manifests": [file_identity(root, diff.SCRIPT_DIR / "coro_regression_cases.txt")],
        "case_count": len(rows),
        "cases": rows,
        "divergence_baseline": file_identity(root, divergence_path),
        "refusal_baseline": file_identity(root, refusal_path),
    }


def fact_from_detail(family: str, detail: str) -> str:
    """Verifier-side reconstruction; intentionally not imported from generator."""
    facts = [family]
    diagnostic = re.search(
        r"(XR_[A-Z0-9_]+: [a-z][^\n]*?)(?:\s+[a-z][a-z0-9-]*=|$)", detail
    )
    if diagnostic:
        facts.append(diagnostic.group(1).strip())
    patterns = (
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
    )
    for label, pattern in patterns:
        match = re.search(pattern, detail)
        if match:
            facts.append(f"{label}={match.group(1)}")
    return "|".join(facts)


def refusal_rows(log: bytes) -> list[dict[str, str]]:
    rows = []
    seen = set()
    for raw in log.decode("utf-8", errors="backslashreplace").splitlines():
        match = SURVEY_LINE.fullmatch(raw.strip())
        if not match:
            continue
        owner, family, detail = match.group(1), match.group(2), match.group(3) or ""
        key = (owner, family, fact_from_detail(family, detail))
        if key in seen:
            continue
        seen.add(key)
        rows.append({"owner": owner, "family": family, "blocking_fact": key[2]})
    return rows


def stable_toolchain(data: dict[str, Any]) -> dict[str, Any]:
    probe = data.get("probe", {})
    return {
        "schema": data.get("schema"),
        "xray": data.get("xray"),
        "request": data.get("request"),
        "selection": data.get("selection"),
        "capabilities": data.get("capabilities"),
        "probe": {"fingerprint": probe.get("fingerprint")},
    }


def exact_keys(value: Any, keys: set[str], where: str, errors: list[str]) -> bool:
    if not isinstance(value, dict):
        errors.append(f"{where} must be an object")
        return False
    if set(value) != keys:
        errors.append(f"{where} fields are not the exact schema")
        return False
    return True


def validate_core(manifest: dict[str, Any], manifest_path: Path) -> list[str]:
    errors: list[str] = []
    top = {
        "schema", "kind", "generator", "status", "identity", "coverage", "measurement",
        "inputs", "results", "root_causes", "summary",
    }
    if not exact_keys(manifest, top, "manifest", errors):
        return errors
    if manifest.get("schema") != SCHEMA or manifest.get("kind") != KIND \
            or manifest.get("generator") != GENERATOR:
        errors.append("manifest schema/kind/generator mismatch")
    if manifest.get("status") != "passed":
        errors.append("manifest status must be passed")
    measurement = manifest.get("measurement", {})
    if not exact_keys(
        measurement,
        {
            "profile", "artifact", "host_c_optimization", "xi_optimization", "backends",
            "collect_all_refusals", "toolchain",
        },
        "measurement", errors,
    ):
        return errors
    if measurement.get("profile") != "hosted" or measurement.get("artifact") != "native-executable" \
            or measurement.get("host_c_optimization") != "0" \
            or measurement.get("xi_optimization") != "pipeline-default" \
            or measurement.get("backends") != ["vm", "aot"] \
            or measurement.get("collect_all_refusals") is not True:
        errors.append("measurement is not the exact hosted native VM/AOT refusal census")
    selection = measurement.get("toolchain", {}).get("selection", {})
    request = measurement.get("toolchain", {}).get("request", {})
    if request.get("target") != "native" or request.get("profile") != "hosted" \
            or not request.get("normalizedTarget") or not selection.get("ready") \
            or selection.get("fallbackUsed") is not False or not selection.get("provider") \
            or not selection.get("runtimeArtifact"):
        errors.append("toolchain identity is not an exact ready hosted native selection")

    inputs = manifest.get("inputs", {})
    input_cases = inputs.get("cases") if isinstance(inputs, dict) else None
    results = manifest.get("results")
    if not isinstance(input_cases, list) or not input_cases:
        errors.append("inputs must contain at least one governed case")
        return errors
    if inputs.get("case_count") != len(input_cases):
        errors.append("input case count mismatch")
    paths = [row.get("path") for row in input_cases if isinstance(row, dict)]
    if len(set(paths)) != len(input_cases):
        errors.append("input case paths must be unique")
    if not isinstance(results, list) or len(results) != len(input_cases):
        errors.append("results must cover every input exactly once")
        return errors
    result_paths = [row.get("case") for row in results if isinstance(row, dict)]
    if result_paths != paths:
        errors.append("results are not in exact governed input order")

    aggregates: dict[tuple[str, str, str], list[str]] = collections.defaultdict(list)
    counts: collections.Counter[str] = collections.Counter()
    result_keys = {"case", "outcome", "first_refusal", "refusals", "diagnostic", "build_log"}
    input_by_path = {row["path"]: row for row in input_cases}
    for index, row in enumerate(results):
        where = f"results[{index}]"
        if not exact_keys(row, result_keys, where, errors):
            continue
        outcome = row.get("outcome")
        if outcome not in {"pass", "expected-rejection", "refused", "skip"}:
            errors.append(f"{where} has a non-qualifying outcome")
            continue
        counts[outcome] += 1
        rejection_oracle = input_by_path[row["case"]].get("oracle", {}).get("kind") \
            == "vm-plus-native-rejection"
        if (outcome == "expected-rejection") != rejection_oracle:
            errors.append(f"{where} expected-rejection does not match its governed oracle")
        if outcome != "refused":
            if row.get("first_refusal") is not None or row.get("refusals") != [] \
                    or row.get("build_log") is not None:
                errors.append(f"{where} non-refusal carries refusal evidence")
            continue
        log_row = row.get("build_log")
        if not isinstance(log_row, dict) or set(log_row) != {"path", "sha256", "size_bytes"}:
            errors.append(f"{where} refusal has no exact log identity")
            continue
        log_path = (manifest_path.parent / str(log_row.get("path", ""))).resolve()
        try:
            log_path.relative_to(manifest_path.parent.resolve())
        except ValueError:
            errors.append(f"{where} log escapes manifest directory")
            continue
        if not log_path.is_file():
            errors.append(f"{where} log is missing")
            continue
        data = log_path.read_bytes()
        if digest_bytes(data) != log_row.get("sha256") or len(data) != log_row.get("size_bytes"):
            errors.append(f"{where} log identity mismatch")
            continue
        reconstructed = refusal_rows(data)
        if not reconstructed or reconstructed != row.get("refusals"):
            errors.append(f"{where} refusal rows do not match raw log")
            continue
        if row.get("first_refusal") != reconstructed[0]:
            errors.append(f"{where} first refusal is not the first source-emitted row")
        for refusal in reconstructed:
            if refusal["owner"] not in OWNERS:
                errors.append(f"{where} has an unknown refusal owner")
            key = (refusal["owner"], refusal["family"], refusal["blocking_fact"])
            aggregates[key].append(row["case"])

    refused_names = {row["case"] for row in results if row.get("outcome") == "refused"}
    skipped_names = {row["case"] for row in results if row.get("outcome") == "skip"}
    listed_names = {
        row["path"] for row in input_cases if row.get("listed_refusal") is True
    }
    expected_coverage = {
        "listed_refusal_count": len(listed_names),
        "observed_refusal_count": len(refused_names),
        "new_refusals": sorted(refused_names - listed_names),
        "resolved_refusals": sorted(listed_names - refused_names - skipped_names),
    }
    if manifest.get("coverage") != expected_coverage:
        errors.append("coverage drift is not independently reproducible")
    if expected_coverage["new_refusals"] or expected_coverage["resolved_refusals"]:
        errors.append("manifest violates the refusal coverage ratchet")

    expected_roots = [
        {
            "owner": owner,
            "family": family,
            "blocking_fact": fact,
            "case_count": len(cases),
            "cases": sorted(cases),
        }
        for (owner, family, fact), cases in sorted(aggregates.items())
    ]
    if manifest.get("root_causes") != expected_roots:
        errors.append("root cause aggregation is not independently reproducible")
    expected_summary = {
        "case_count": len(results),
        "comparable_count": counts["pass"],
        "expected_rejection_count": counts["expected-rejection"],
        "refused_count": counts["refused"],
        "skipped_count": counts["skip"],
        "failed_count": 0,
        "root_cause_count": len(expected_roots),
    }
    if manifest.get("summary") != expected_summary:
        errors.append("summary does not match independently counted results")
    return errors


def validate_current(root: Path, build: Path, manifest_path: Path, manifest: dict[str, Any]) -> list[str]:
    errors = validate_core(manifest, manifest_path)
    diff = load_module("xray_live_refusal_diff_owner", root / "tests/diff/run_backend_diff.py")
    try:
        current_inputs = expected_inputs(root, diff)
    except (OSError, RuntimeError, ValueError) as error:
        errors.append(f"cannot reconstruct governed inputs: {error}")
    else:
        if manifest.get("inputs") != current_inputs:
            errors.append("manifest inputs do not match current governed discovery/oracles/baselines")

    identity_owner = load_module(
        "xray_live_refusal_identity_owner", root / "tests/target-machine/phase0/run_baseline.py"
    )
    try:
        identity = identity_owner.compiler_identity(
            root, build, {"residue": {"source_root_globs": []}}
        )
        values = identity_owner.cmake_identity_values(build)
        source = Path(values.get("CMAKE_HOME_DIRECTORY", "")).resolve()
        if values.get("CMAKE_GENERATOR") != "Ninja" \
                or values.get("CMAKE_BUILD_TYPE") != "Release" \
                or values.get("XRAY_STDLIB_VM_FASTPATHS") != "OFF" \
                or source != root.resolve() \
                or identity.get("version", {}).get("buildProfile") != "Release":
            raise RuntimeError("compiler is not the exact source-root Ninja Release fastpaths-off build")
        identity["build"] = {
            "generator": "Ninja", "build_type": "Release",
            "stdlib_vm_fastpaths": "OFF", "source_root": "${SOURCE_ROOT}",
        }
    except (OSError, RuntimeError, ValueError) as error:
        errors.append(f"compiler identity verification failed: {error}")
        return errors
    if manifest.get("identity") != identity:
        errors.append("manifest source/binary identity does not match current compiler")

    survey = load_module("xray_live_refusal_generator", root / GENERATOR)
    try:
        binary = identity_owner.compiler_binary_path(build)
        toolchain = survey.toolchain_identity(binary, 180)
    except (OSError, RuntimeError, ValueError) as error:
        errors.append(f"toolchain identity verification failed: {error}")
    else:
        if stable_toolchain(manifest.get("measurement", {}).get("toolchain", {})) != toolchain:
            errors.append("manifest toolchain identity does not match current provider")
    return errors


def self_test() -> int:
    decision_log = (
        b"[refusal-survey] owner=target-plan-builder family=calls "
        b"XR_TARGET_1003: direct-local argument contract needs unsupported storage or ownership "
        b"opcode=17 parameter-ordinal=2 storage-mask=4 operand-mode=1 parameter-mode=0 "
        b"operand-transfer=2 parameter-transfer=1 operand-ownership=3 parameter-ownership=2 "
        b"operand-access=1 operand-role=4 expected-role=4 type-match=1 ordinal-match=1 "
        b"contract-flag=1 addressable=0\n"
    )
    expected_decision = [{
        "owner": "target-plan-builder",
        "family": "calls",
        "blocking_fact": (
            "calls|XR_TARGET_1003: direct-local argument contract needs unsupported storage or "
            "ownership|opcode=17|parameter-ordinal=2|storage-mask=4|operand-mode=1|"
            "parameter-mode=0|operand-transfer=2|parameter-transfer=1|operand-ownership=3|"
            "parameter-ownership=2|operand-access=1|operand-role=4|expected-role=4|type-match=1|"
            "ordinal-match=1|contract-flag=1|addressable=0"
        ),
    }]
    if refusal_rows(decision_log) != expected_decision:
        print("self-test rejected stable decision facts", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="xray-live-refusal-check-") as temp:
        base = Path(temp)
        log_dir = base / "evidence.json.logs"
        log_dir.mkdir()
        log = (
            b"[refusal-survey] owner=target-plan-builder family=calls "
            b"XR_TARGET_1001: unsupported argument operation=4 opcode=17\n"
        )
        log_path = log_dir / "0000.log"
        log_path.write_bytes(log)
        refusal = {
            "owner": "target-plan-builder",
            "family": "calls",
            "blocking_fact": "calls|XR_TARGET_1001: unsupported argument|opcode=17",
        }
        case = {
            "path": "tests/diff/cases/a.xr", "source": {}, "args": None, "stdin": None,
            "project": None, "oracle": {}, "diff_backends": [],
            "listed_refusal": True, "listed_divergence": False,
        }
        manifest = {
            "schema": 1, "kind": KIND, "generator": GENERATOR, "status": "passed",
            "identity": {},
            "coverage": {
                "listed_refusal_count": 1, "observed_refusal_count": 1,
                "new_refusals": [], "resolved_refusals": [],
            },
            "measurement": {
                "profile": "hosted", "artifact": "native-executable",
                "host_c_optimization": "0", "xi_optimization": "pipeline-default",
                "backends": ["vm", "aot"], "collect_all_refusals": True,
                "toolchain": {
                    "request": {"target": "native", "profile": "hosted", "normalizedTarget": "x"},
                    "selection": {"ready": True, "fallbackUsed": False, "provider": "p", "runtimeArtifact": "r"},
                },
            },
            "inputs": {"case_roots": ["tests/diff/cases"], "case_manifests": [], "case_count": 1, "cases": [case]},
            "results": [{
                "case": case["path"], "outcome": "refused", "first_refusal": refusal,
                "refusals": [refusal], "diagnostic": None,
                "build_log": {"path": "evidence.json.logs/0000.log", "sha256": digest_bytes(log), "size_bytes": len(log)},
            }],
            "root_causes": [{**refusal, "case_count": 1, "cases": [case["path"]]}],
            "summary": {"case_count": 1, "comparable_count": 0, "expected_rejection_count": 0, "refused_count": 1, "skipped_count": 0, "failed_count": 0, "root_cause_count": 1},
        }
        path = base / "evidence.json"
        if validate_core(manifest, path):
            print("self-test rejected valid fixture", file=sys.stderr)
            return 1
        mutations = []
        bad = copy.deepcopy(manifest); bad["results"][0]["first_refusal"]["owner"] = "legacy"; mutations.append(bad)
        bad = copy.deepcopy(manifest); bad["summary"]["refused_count"] = 0; mutations.append(bad)
        bad = copy.deepcopy(manifest); bad["root_causes"] = []; mutations.append(bad)
        bad = copy.deepcopy(manifest); bad["coverage"]["observed_refusal_count"] = 0; mutations.append(bad)
        for index, mutation in enumerate(mutations):
            if not validate_core(mutation, path):
                print(f"self-test mutation {index} was accepted", file=sys.stderr)
                return 1
        log_path.write_bytes(log + b"mutation\n")
        if not validate_core(manifest, path):
            print("self-test log mutation was accepted", file=sys.stderr)
            return 1
    print("live refusal manifest injection self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--build", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--self-test", action="store_true")
    options = parser.parse_args()
    if options.self_test:
        return self_test()
    if options.build is None or options.manifest is None:
        parser.error("--build and --manifest are required unless --self-test is used")
    root = options.root.resolve()
    manifest_path = options.manifest.resolve()
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8", errors="strict"))
        errors = validate_current(root, options.build.resolve(), manifest_path, manifest)
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        print(f"live refusal manifest verification failed: {error}", file=sys.stderr)
        return 1
    if errors:
        for error in errors:
            print(f"live refusal manifest verification failed: {error}", file=sys.stderr)
        return 1
    print("live refusal manifest verification: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
