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


ROOT = Path(__file__).resolve().parents[1]
SCHEMA = 2
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
DIAGNOSTIC_REGISTRY = "contracts/target-machine/diagnostic-codes.toml"
REGISTRY_CODE = re.compile(r'^id = "(XR_[A-Z]+_[0-9]{4})"$', re.M)
# This checker replays every build log independently of the generator, so the
# two must not share code here. What they do share is the governed registry:
# code identity is decided there, once, and both sides look it up rather than
# each carrying its own idea of what an XR_-shaped token means.
DIAGNOSTIC_SHAPE = re.compile(r"\b(XR_[A-Z0-9_]+)(?::[ \t]*|[ \t]+)([^\r\n]+)")


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


def refusal_rows(log: bytes) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for line_number, raw in enumerate(
        log.decode("utf-8", errors="backslashreplace").splitlines(), start=1
    ):
        source_text = raw.strip()
        match = SURVEY_LINE.fullmatch(source_text)
        if not match:
            continue
        owner, family, detail = match.group(1), match.group(2), match.group(3) or ""
        rows.append({
            "sequence": len(rows),
            "line_number": line_number,
            "source_text": source_text,
            "owner": owner,
            "family": family,
            "detail": detail,
            "blocking_fact": fact_from_detail(family, detail),
        })
    return rows


def registered_diagnostic_codes(root: Path) -> frozenset[str]:
    """Load the governed diagnostic registry, the sole authority over code identity."""
    path = root / DIAGNOSTIC_REGISTRY
    if not path.is_file():
        raise RuntimeError(f"diagnostic registry missing: {DIAGNOSTIC_REGISTRY}")
    codes = frozenset(REGISTRY_CODE.findall(path.read_text(encoding="utf-8", errors="strict")))
    if not codes:
        raise RuntimeError(f"diagnostic registry declares no codes: {DIAGNOSTIC_REGISTRY}")
    return codes


def diagnostic_row(log: bytes, codes: frozenset[str]) -> dict[str, str] | None:
    """Report the first registered code in the log, with the message it introduces."""
    text = log.decode("utf-8", errors="backslashreplace")
    best: tuple[int, str, str] | None = None
    for match in DIAGNOSTIC_SHAPE.finditer(text):
        if match.group(1) not in codes:
            continue
        if best is None or match.start() < best[0]:
            best = (match.start(), match.group(1), match.group(2).strip())
    return None if best is None else {"code": best[1], "message": best[2]}


def missing_evidence_gap(diagnostic: dict[str, str] | None) -> dict[str, str | None]:
    if diagnostic is not None:
        return {
            "classification": "diagnostic-without-structured-refusal",
            "diagnostic_code": diagnostic["code"],
            "required_action": "emit-source-owned-structured-refusal",
        }
    return {
        "classification": "opaque-refusal-without-structured-diagnostic",
        "diagnostic_code": None,
        "required_action": "emit-stable-diagnostic-and-source-owned-structured-refusal",
    }


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
    codes = registered_diagnostic_codes(ROOT)
    top = {
        "schema", "kind", "generator", "status", "identity", "coverage", "measurement",
        "inputs", "results", "root_causes", "evidence_gaps", "summary",
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

    aggregate_events: collections.Counter[tuple[str, str, str]] = collections.Counter()
    aggregate_cases: dict[tuple[str, str, str], set[str]] = collections.defaultdict(set)
    gap_cases: dict[tuple[str, str | None, str], list[str]] = collections.defaultdict(list)
    counts: collections.Counter[str] = collections.Counter()
    result_keys = {
        "case", "outcome", "first_refusal", "refusals", "diagnostic", "build_log",
        "evidence_gap",
    }
    input_by_path = {row["path"]: row for row in input_cases}
    for index, row in enumerate(results):
        where = f"results[{index}]"
        if not exact_keys(row, result_keys, where, errors):
            continue
        outcome = row.get("outcome")
        if outcome not in {"pass", "expected-rejection", "refused", "skip"}:
            errors.append(f"{where} has a non-qualifying outcome")
            if isinstance(outcome, str):
                counts[outcome] += 1
            continue
        counts[outcome] += 1
        rejection_oracle = input_by_path[row["case"]].get("oracle", {}).get("kind") \
            == "vm-plus-native-rejection"
        if (outcome == "expected-rejection") != rejection_oracle:
            errors.append(f"{where} expected-rejection does not match its governed oracle")
        if outcome != "refused":
            if row.get("first_refusal") is not None or row.get("refusals") != [] \
                    or row.get("build_log") is not None or row.get("evidence_gap") is not None:
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
        reconstructed_diagnostic = diagnostic_row(data, codes)
        if reconstructed_diagnostic != row.get("diagnostic"):
            errors.append(f"{where} diagnostic does not match raw log")
        if reconstructed != row.get("refusals"):
            errors.append(f"{where} refusal rows do not match raw log")
            continue
        if not reconstructed:
            if row.get("first_refusal") is not None:
                errors.append(f"{where} has a first refusal absent from the raw log")
            gap = missing_evidence_gap(reconstructed_diagnostic)
            if row.get("evidence_gap") != gap:
                errors.append(f"{where} evidence-gap classification does not match raw log")
                continue
            key = (
                str(gap["classification"]), gap["diagnostic_code"],
                str(gap["required_action"]),
            )
            gap_cases[key].append(row["case"])
            continue
        if row.get("evidence_gap") is not None:
            errors.append(f"{where} structured refusal carries a false evidence gap")
        if row.get("first_refusal") != reconstructed[0]:
            errors.append(f"{where} first refusal is not the first source-emitted row")
        for refusal in reconstructed:
            if refusal["owner"] not in OWNERS:
                errors.append(f"{where} has an unknown refusal owner")
            key = (refusal["owner"], refusal["family"], refusal["blocking_fact"])
            aggregate_events[key] += 1
            aggregate_cases[key].add(row["case"])

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
            "event_count": aggregate_events[(owner, family, fact)],
            "case_count": len(cases),
            "cases": sorted(cases),
        }
        for (owner, family, fact), cases in sorted(aggregate_cases.items())
    ]
    if manifest.get("root_causes") != expected_roots:
        errors.append("root cause aggregation is not independently reproducible")
    expected_gaps = [
        {
            "classification": classification,
            "diagnostic_code": diagnostic_code,
            "required_action": required_action,
            "case_count": len(cases),
            "cases": sorted(cases),
        }
        for (classification, diagnostic_code, required_action), cases in sorted(
            gap_cases.items(), key=lambda item: (item[0][0], item[0][1] or "", item[0][2])
        )
    ]
    if manifest.get("evidence_gaps") != expected_gaps:
        errors.append("evidence-gap aggregation is not independently reproducible")
    for gap in expected_gaps:
        errors.append(
            "missing source-emitted refusal evidence: "
            f"classification={gap['classification']} "
            f"diagnostic={gap['diagnostic_code'] or '<none>'} "
            f"cases={gap['case_count']} required-action={gap['required_action']}"
        )
    expected_summary = {
        "case_count": len(results),
        "comparable_count": counts["pass"],
        "expected_rejection_count": counts["expected-rejection"],
        "refused_count": counts["refused"],
        "skipped_count": counts["skip"],
        "failed_count": counts["fail"],
        "structured_refusal_count": counts["refused"] - sum(
            len(cases) for cases in gap_cases.values()
        ),
        "missing_refusal_evidence_count": sum(len(cases) for cases in gap_cases.values()),
        "refusal_event_count": sum(aggregate_events.values()),
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
    codes = registered_diagnostic_codes(ROOT)
    for required in ("XR_TARGET_1000", "XR_TARGET_1001", "XR_CORO_4003"):
        if required not in codes:
            print(f"diagnostic registry does not carry {required}", file=sys.stderr)
            return 1
    decision_source = (
        "[refusal-survey] owner=target-plan-builder family=calls "
        "XR_TARGET_1003: direct-local argument contract needs unsupported storage or ownership "
        "opcode=17 parameter-ordinal=2 storage-mask=4 operand-mode=1 parameter-mode=0 "
        "operand-transfer=2 parameter-transfer=1 operand-ownership=3 parameter-ownership=2 "
        "operand-access=1 operand-role=4 expected-role=4 type-match=1 ordinal-match=1 "
        "contract-flag=1 addressable=0"
    )
    decision_log = ("noise\n" + decision_source + "\n" + decision_source + "\n").encode()
    expected_fact = (
        "calls|XR_TARGET_1003: direct-local argument contract needs unsupported storage or "
        "ownership|opcode=17|parameter-ordinal=2|storage-mask=4|operand-mode=1|"
        "parameter-mode=0|operand-transfer=2|parameter-transfer=1|operand-ownership=3|"
        "parameter-ownership=2|operand-access=1|operand-role=4|expected-role=4|type-match=1|"
        "ordinal-match=1|contract-flag=1|addressable=0"
    )
    expected_decision = [{
        "sequence": sequence,
        "line_number": sequence + 2,
        "source_text": decision_source,
        "owner": "target-plan-builder",
        "family": "calls",
        "detail": decision_source.split(" family=calls ", 1)[1],
        "blocking_fact": expected_fact,
    } for sequence in range(2)]
    if refusal_rows(decision_log) != expected_decision:
        print("self-test lost, deduplicated or reordered stable decision facts", file=sys.stderr)
        return 1
    product_source = (
        "[refusal-survey] owner=target-plan-builder family=program_build "
        "XR_TARGET_1003: call result or argument partition cannot bind canonical storage"
    )
    product_rows = refusal_rows((product_source + "\n").encode())
    if product_rows != [{
        "sequence": 0,
        "line_number": 1,
        "source_text": product_source,
        "owner": "target-plan-builder",
        "family": "program_build",
        "detail": product_source.split(" family=program_build ", 1)[1],
        "blocking_fact": (
            "program_build|XR_TARGET_1003: call result or argument partition cannot bind "
            "canonical storage"
        ),
    }]:
        print("self-test lost product TargetPlan refusal evidence", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="xray-live-refusal-check-") as temp:
        base = Path(temp)
        log_dir = base / "evidence.json.logs"
        log_dir.mkdir()
        log = (
            b"[refusal-survey] owner=target-plan-builder family=calls "
            b"XR_TARGET_1001: unsupported argument operation=4 opcode=17\n"
            b"[refusal-survey] owner=target-plan-builder family=calls "
            b"XR_TARGET_1001: unsupported argument operation=4 opcode=17\n"
        )
        log_path = log_dir / "0000.log"
        log_path.write_bytes(log)
        refusals = refusal_rows(log)
        refusal = refusals[0]
        case = {
            "path": "tests/diff/cases/a.xr", "source": {}, "args": None, "stdin": None,
            "project": None, "oracle": {}, "diff_backends": [],
            "listed_refusal": True, "listed_divergence": False,
        }
        manifest = {
            "schema": SCHEMA, "kind": KIND, "generator": GENERATOR, "status": "passed",
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
                "refusals": refusals,
                "diagnostic": {
                    "code": "XR_TARGET_1001",
                    "message": "unsupported argument operation=4 opcode=17",
                },
                "build_log": {"path": "evidence.json.logs/0000.log", "sha256": digest_bytes(log), "size_bytes": len(log)},
                "evidence_gap": None,
            }],
            "root_causes": [{
                "owner": refusal["owner"], "family": refusal["family"],
                "blocking_fact": refusal["blocking_fact"], "event_count": 2,
                "case_count": 1, "cases": [case["path"]],
            }],
            "evidence_gaps": [],
            "summary": {
                "case_count": 1, "comparable_count": 0, "expected_rejection_count": 0,
                "refused_count": 1, "skipped_count": 0, "failed_count": 0,
                "structured_refusal_count": 1, "missing_refusal_evidence_count": 0,
                "refusal_event_count": 2, "root_cause_count": 1,
            },
        }
        path = base / "evidence.json"
        if validate_core(manifest, path):
            print("self-test rejected valid fixture", file=sys.stderr)
            return 1
        mutations = []
        bad = copy.deepcopy(manifest); bad["schema"] = 1; mutations.append(bad)
        bad = copy.deepcopy(manifest); bad["results"][0]["first_refusal"]["owner"] = "legacy"; mutations.append(bad)
        bad = copy.deepcopy(manifest); bad["summary"]["refused_count"] = 0; mutations.append(bad)
        bad = copy.deepcopy(manifest); bad["root_causes"] = []; mutations.append(bad)
        bad = copy.deepcopy(manifest); bad["coverage"]["observed_refusal_count"] = 0; mutations.append(bad)
        bad = copy.deepcopy(manifest); bad["results"][0]["refusals"][0]["sequence"] = 1; mutations.append(bad)
        bad = copy.deepcopy(manifest); del bad["results"][0]["refusals"][0]["line_number"]; mutations.append(bad)
        bad = copy.deepcopy(manifest); bad["results"][0]["refusals"][0]["source_text"] += " "; mutations.append(bad)
        bad = copy.deepcopy(manifest); bad["results"][0]["evidence_gap"] = missing_evidence_gap(None); mutations.append(bad)
        bad = copy.deepcopy(manifest); bad["results"][0]["refusals"].reverse(); mutations.append(bad)
        bad = copy.deepcopy(manifest); bad["results"][0]["refusals"].append(copy.deepcopy(refusal)); mutations.append(bad)
        bad = copy.deepcopy(manifest); bad["results"][0]["refusals"].pop(); mutations.append(bad)
        bad = copy.deepcopy(manifest); bad["results"][0].update({
            "first_refusal": None, "refusals": [],
            "evidence_gap": missing_evidence_gap(bad["results"][0]["diagnostic"]),
        }); mutations.append(bad)
        for index, mutation in enumerate(mutations):
            if not validate_core(mutation, path):
                print(f"self-test mutation {index} was accepted", file=sys.stderr)
                return 1
        log_path.write_bytes(log + b"mutation\n")
        if not validate_core(manifest, path):
            print("self-test log mutation was accepted", file=sys.stderr)
            return 1
        missing_log = b"Error: XR_TARGET_1000: authority was not produced\n"
        log_path.write_bytes(missing_log)
        gap = missing_evidence_gap(diagnostic_row(missing_log, codes))
        missing = copy.deepcopy(manifest)
        missing["status"] = "failed"
        missing["results"][0].update({
            "first_refusal": None,
            "refusals": [],
            "diagnostic": diagnostic_row(missing_log, codes),
            "build_log": {
                "path": "evidence.json.logs/0000.log",
                "sha256": digest_bytes(missing_log),
                "size_bytes": len(missing_log),
            },
            "evidence_gap": gap,
        })
        missing["root_causes"] = []
        missing["evidence_gaps"] = [{
            **gap, "case_count": 1, "cases": [case["path"]],
        }]
        missing["summary"].update({
            "structured_refusal_count": 0,
            "missing_refusal_evidence_count": 1,
            "refusal_event_count": 0,
            "root_cause_count": 0,
        })
        missing_errors = validate_core(missing, path)
        if not any("missing source-emitted refusal evidence" in error for error in missing_errors) \
                or any("refusal rows do not match raw log" in error for error in missing_errors):
            print("self-test did not classify missing source evidence precisely", file=sys.stderr)
            return 1
    colon_free_log = (
        b"Error: Xi pipeline failed at ownership: XR_CORO_4003 func '<main>': "
        b"state 1 error continuation is not derivable\n"
    )
    if diagnostic_row(colon_free_log, codes) != {
        "code": "XR_CORO_4003",
        "message": "func '<main>': state 1 error continuation is not derivable",
    }:
        print("self-test lost a space-separated diagnostic code", file=sys.stderr)
        return 1
    # An internal enumerator name is XR_-shaped but carries no registered
    # identity, so a refusal that names one still owes a stable diagnostic.
    unregistered_log = (
        b"Error: module representation materialization failed for 'm': "
        b"XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE record=0 value=5 operation=6\n"
    )
    if "XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE" in codes:
        print("registry unexpectedly carries an enumerator name", file=sys.stderr)
        return 1
    if diagnostic_row(unregistered_log, codes) is not None:
        print("self-test read an unregistered name as a code", file=sys.stderr)
        return 1
    # Reach the uncoded classification through the log reader, not by passing
    # None: that shortcut leaves the lookup untested on this path.
    uncoded_log = b"Error: Xi pipeline failed at ownership: coroutine lowering failed closed\n"
    if diagnostic_row(uncoded_log, codes) is not None \
            or missing_evidence_gap(diagnostic_row(uncoded_log, codes)) != {
        "classification": "opaque-refusal-without-structured-diagnostic",
        "diagnostic_code": None,
        "required_action": "emit-stable-diagnostic-and-source-owned-structured-refusal",
    }:
        print("self-test misread an uncoded refusal", file=sys.stderr)
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
