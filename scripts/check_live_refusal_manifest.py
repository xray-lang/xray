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
import tomllib
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
SCHEMA = 3
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
DIAGNOSTIC_ID = re.compile(r"XR_[A-Z0-9_]+")
DIAGNOSTIC_TOKEN = re.compile(r"\bXR_[A-Z0-9_]+\b")
# This checker replays every build log independently of the generator, so the
# two must not share code here. What they do share is the governed registry:
# code identity is decided there, once, and both sides look it up rather than
# each carrying its own idea of what an XR_-shaped token means.


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
        # The registry decides which refusals count as carrying a diagnostic, so a
        # manifest is only replayable against the registry revision that produced it.
        "diagnostic_registry": file_identity(root, root / DIAGNOSTIC_REGISTRY),
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
        ("definition-rep", r"\bdefinition-rep=(\d+)"),
        ("use-rep", r"\buse-rep=(\d+)"),
        ("machine-rep", r"\bmachine-rep=(\d+)"),
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


def diagnostic_codes_in_text(text: str, codes: frozenset[str]) -> list[str]:
    """Return registered diagnostic occurrences in source order."""
    return [
        match.group(0)
        for match in DIAGNOSTIC_TOKEN.finditer(text)
        if match.group(0) in codes
    ]


def refusal_rows(log: bytes, codes: frozenset[str]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for line_number, raw in enumerate(
        log.decode("utf-8", errors="backslashreplace").splitlines(), start=1
    ):
        source_text = raw.strip()
        match = SURVEY_LINE.fullmatch(source_text)
        if not match:
            continue
        owner, family, detail = match.group(1), match.group(2), match.group(3) or ""
        row_codes = diagnostic_codes_in_text(source_text, codes)
        rows.append({
            "sequence": len(rows),
            "line_number": line_number,
            "source_text": source_text,
            "owner": owner,
            "family": family,
            "diagnostic_code": row_codes[0] if len(row_codes) == 1 else None,
            "detail": detail,
            "blocking_fact": fact_from_detail(family, detail),
        })
    return rows


def registered_diagnostic_codes(root: Path) -> frozenset[str]:
    """Load the governed diagnostic registry, the sole authority over code identity."""
    path = root / DIAGNOSTIC_REGISTRY
    if not path.is_file():
        raise RuntimeError(f"diagnostic registry missing: {DIAGNOSTIC_REGISTRY}")
    try:
        registry = tomllib.loads(path.read_text(encoding="utf-8", errors="strict"))
    except tomllib.TOMLDecodeError as error:
        raise RuntimeError(f"diagnostic registry is not valid TOML: {error}") from error
    rows = registry.get("code")
    if not isinstance(rows, list) or not rows:
        raise RuntimeError(f"diagnostic registry declares no codes: {DIAGNOSTIC_REGISTRY}")
    codes: set[str] = set()
    for index, row in enumerate(rows):
        code = row.get("id") if isinstance(row, dict) else None
        if not isinstance(code, str) or DIAGNOSTIC_ID.fullmatch(code) is None:
            raise RuntimeError(f"diagnostic registry code[{index}] has a malformed id")
        if code in codes:
            raise RuntimeError(f"diagnostic registry contains duplicate id: {code}")
        codes.add(code)
    if not codes:
        raise RuntimeError(f"diagnostic registry declares no codes: {DIAGNOSTIC_REGISTRY}")
    return frozenset(codes)


def first_registered_log_diagnostic(log: bytes, codes: frozenset[str]) -> str | None:
    """Return the first registered log code for zero-row debt triage only."""
    found = diagnostic_codes_in_text(
        log.decode("utf-8", errors="backslashreplace"), codes
    )
    return found[0] if found else None


def missing_source_gap(log: bytes, codes: frozenset[str]) -> dict[str, Any]:
    diagnostic = first_registered_log_diagnostic(log, codes)
    if diagnostic is not None:
        return {
            "classification": "diagnostic-without-source-refusal",
            "owner": None,
            "family": None,
            "observed_diagnostic_codes": [diagnostic],
            "required_action": "emit-source-refusal-with-one-registered-diagnostic",
        }
    return {
        "classification": "opaque-refusal-without-source-diagnostic",
        "owner": None,
        "family": None,
        "observed_diagnostic_codes": [],
        "required_action": "emit-stable-diagnostic-and-source-refusal",
    }


def row_evidence_gap(row: dict[str, Any], codes: frozenset[str]) -> dict[str, Any] | None:
    observed = diagnostic_codes_in_text(str(row["source_text"]), codes)
    if len(observed) == 1:
        return None
    return {
        "classification": (
            "source-refusal-without-diagnostic"
            if not observed
            else "source-refusal-with-ambiguous-diagnostic"
        ),
        "owner": row["owner"],
        "family": row["family"],
        "observed_diagnostic_codes": observed,
        "required_action": "bind-exactly-one-registered-diagnostic-on-source-row",
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


def validate_core(
    manifest: dict[str, Any], manifest_path: Path, codes: frozenset[str]
) -> list[str]:
    errors: list[str] = []
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

    aggregate_events: collections.Counter[
        tuple[str, str, str | None, str]
    ] = collections.Counter()
    aggregate_cases: dict[
        tuple[str, str, str | None, str], set[str]
    ] = collections.defaultdict(set)
    gap_events: collections.Counter[
        tuple[str, str | None, str | None, tuple[str, ...], str]
    ] = collections.Counter()
    gap_cases: dict[
        tuple[str, str | None, str | None, tuple[str, ...], str], set[str]
    ] = collections.defaultdict(set)
    incomplete_cases: set[str] = set()
    missing_source_cases: set[str] = set()
    bound_event_count = 0
    unbound_event_count = 0
    counts: collections.Counter[str] = collections.Counter()
    result_keys = {
        "case", "outcome", "first_refusal", "refusals", "build_log", "failure",
    }
    input_by_path = {row["path"]: row for row in input_cases}
    for index, row in enumerate(results):
        where = f"results[{index}]"
        if not exact_keys(row, result_keys, where, errors):
            continue
        outcome = row.get("outcome")
        if outcome not in {"pass", "expected-rejection", "refused", "skip", "fail"}:
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
                    or row.get("build_log") is not None:
                errors.append(f"{where} non-refusal carries refusal evidence")
            failure = row.get("failure")
            if outcome == "fail":
                if not isinstance(failure, str) or not failure:
                    errors.append(f"{where} differential failure has no failure text")
            elif failure is not None:
                errors.append(f"{where} qualifying non-refusal carries failure text")
            continue
        if row.get("failure") is not None:
            errors.append(f"{where} refusal carries non-refusal failure text")
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
        reconstructed = refusal_rows(data, codes)
        if reconstructed != row.get("refusals"):
            errors.append(f"{where} refusal rows do not match raw log")
            continue
        if not reconstructed:
            if row.get("first_refusal") is not None:
                errors.append(f"{where} has a first refusal absent from the raw log")
            incomplete_cases.add(row["case"])
            missing_source_cases.add(row["case"])
            gap = missing_source_gap(data, codes)
            key = (
                str(gap["classification"]), gap["owner"], gap["family"],
                tuple(gap["observed_diagnostic_codes"]),
                str(gap["required_action"]),
            )
            gap_events[key] += 1
            gap_cases[key].add(row["case"])
            continue
        if row.get("first_refusal") != reconstructed[0]:
            errors.append(f"{where} first refusal is not the first source-emitted row")
        for refusal in reconstructed:
            if refusal["owner"] not in OWNERS:
                errors.append(f"{where} has an unknown refusal owner")
            gap = row_evidence_gap(refusal, codes)
            if gap is None:
                bound_event_count += 1
            else:
                unbound_event_count += 1
                incomplete_cases.add(row["case"])
                gap_key = (
                    str(gap["classification"]), gap["owner"], gap["family"],
                    tuple(gap["observed_diagnostic_codes"]),
                    str(gap["required_action"]),
                )
                gap_events[gap_key] += 1
                gap_cases[gap_key].add(row["case"])
            key = (
                refusal["owner"], refusal["family"],
                refusal["diagnostic_code"], refusal["blocking_fact"],
            )
            aggregate_events[key] += 1
            aggregate_cases[key].add(row["case"])

    if counts["fail"]:
        errors.append("manifest contains non-refusal differential failures")

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
            "diagnostic_code": diagnostic_code,
            "owner": owner,
            "family": family,
            "blocking_fact": fact,
            "event_count": aggregate_events[(owner, family, diagnostic_code, fact)],
            "case_count": len(cases),
            "cases": sorted(cases),
        }
        for (owner, family, diagnostic_code, fact), cases in sorted(
            aggregate_cases.items(),
            key=lambda item: (
                item[0][0], item[0][1], item[0][2] or "", item[0][3]
            ),
        )
    ]
    if manifest.get("root_causes") != expected_roots:
        errors.append("root cause aggregation is not independently reproducible")
    expected_gaps = [
        {
            "classification": classification,
            "owner": owner,
            "family": family,
            "observed_diagnostic_codes": list(observed_codes),
            "required_action": required_action,
            "event_count": gap_events[
                (classification, owner, family, observed_codes, required_action)
            ],
            "case_count": len(cases),
            "cases": sorted(cases),
        }
        for (
            classification, owner, family, observed_codes, required_action
        ), cases in sorted(
            gap_cases.items(),
            key=lambda item: (
                item[0][0], item[0][1] or "", item[0][2] or "",
                item[0][3], item[0][4],
            ),
        )
    ]
    if manifest.get("evidence_gaps") != expected_gaps:
        errors.append("evidence-gap aggregation is not independently reproducible")
    for gap in expected_gaps:
        errors.append(
            "missing source-emitted refusal evidence: "
            f"classification={gap['classification']} "
            f"owner={gap['owner'] or '<none>'} family={gap['family'] or '<none>'} "
            f"diagnostics={','.join(gap['observed_diagnostic_codes']) or '<none>'} "
            f"events={gap['event_count']} cases={gap['case_count']} "
            f"required-action={gap['required_action']}"
        )
    expected_summary = {
        "case_count": len(results),
        "comparable_count": counts["pass"],
        "expected_rejection_count": counts["expected-rejection"],
        "refused_count": counts["refused"],
        "skipped_count": counts["skip"],
        "failed_count": counts["fail"],
        "complete_refusal_count": counts["refused"] - len(incomplete_cases),
        "incomplete_refusal_count": len(incomplete_cases),
        "missing_source_refusal_case_count": len(missing_source_cases),
        "refusal_event_count": sum(aggregate_events.values()),
        "diagnostic_bound_refusal_event_count": bound_event_count,
        "unbound_refusal_event_count": unbound_event_count,
        "root_cause_count": len(expected_roots),
    }
    if manifest.get("summary") != expected_summary:
        errors.append("summary does not match independently counted results")
    return errors


def validate_current(root: Path, build: Path, manifest_path: Path, manifest: dict[str, Any]) -> list[str]:
    codes = registered_diagnostic_codes(root)
    errors = validate_core(manifest, manifest_path, codes)
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
    for required in (
        "XR_TARGET_1000",
        "XR_TARGET_1001",
        "XR_TARGET_1003",
        "XR_CORO_4003",
        "XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE",
        "XR_AOT_TAIL_CALL_CONFORMANCE_LIVE_OPCODE",
    ):
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
        "diagnostic_code": "XR_TARGET_1003",
        "detail": decision_source.split(" family=calls ", 1)[1],
        "blocking_fact": expected_fact,
    } for sequence in range(2)]
    if refusal_rows(decision_log, codes) != expected_decision:
        print("self-test lost, deduplicated or reordered stable decision facts", file=sys.stderr)
        return 1
    product_source = (
        "[refusal-survey] owner=target-plan-builder family=program_build "
        "XR_TARGET_1003: call result or argument partition cannot bind canonical storage"
    )
    product_rows = refusal_rows((product_source + "\n").encode(), codes)
    if product_rows != [{
        "sequence": 0,
        "line_number": 1,
        "source_text": product_source,
        "owner": "target-plan-builder",
        "family": "program_build",
        "diagnostic_code": "XR_TARGET_1003",
        "detail": product_source.split(" family=program_build ", 1)[1],
        "blocking_fact": (
            "program_build|XR_TARGET_1003: call result or argument partition cannot bind "
            "canonical storage"
        ),
    }]:
        print("self-test lost product TargetPlan refusal evidence", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="xray-refusal-registry-check-") as temp:
        registry_root = Path(temp)
        registry_path = registry_root / DIAGNOSTIC_REGISTRY
        registry_path.parent.mkdir(parents=True)
        registry_path.write_text(
            'stray = "XR_NOT_A_CODE_ENTRY"\n'
            '[[code]]\nid = "XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE"\n',
            encoding="utf-8",
        )
        if registered_diagnostic_codes(registry_root) != frozenset({
            "XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE"
        }):
            print("self-test did not load the exact [[code]].id registry", file=sys.stderr)
            return 1
        invalid_registries = (
            '[[code]]\nid = "XR_DUPLICATE"\n[[code]]\nid = "XR_DUPLICATE"\n',
            '[[code]]\nid = "not-a-diagnostic"\n',
            '[[code]]\nid = 7\n',
            '[[code]]\nname = "missing-id"\n',
            '[[code]\nid = "XR_BROKEN"\n',
        )
        for index, text in enumerate(invalid_registries):
            registry_path.write_text(text, encoding="utf-8")
            try:
                registered_diagnostic_codes(registry_root)
            except RuntimeError:
                pass
            else:
                print(f"self-test accepted invalid registry {index}", file=sys.stderr)
                return 1

    with tempfile.TemporaryDirectory(prefix="xray-live-refusal-check-") as temp:
        base = Path(temp)
        log_dir = base / "evidence.json.logs"
        log_dir.mkdir()
        case = {
            "path": "tests/diff/cases/a.xr", "source": {}, "args": None, "stdin": None,
            "project": None, "oracle": {}, "diff_backends": [],
            "listed_refusal": True, "listed_divergence": False,
        }
        path = base / "evidence.json"

        def fixture_manifest(raw_log: bytes, log_name: str) -> dict[str, Any]:
            log_path = log_dir / log_name
            log_path.write_bytes(raw_log)
            rows = refusal_rows(raw_log, codes)
            root_events: collections.Counter[
                tuple[str, str, str | None, str]
            ] = collections.Counter()
            gap_events: collections.Counter[
                tuple[str, str | None, str | None, tuple[str, ...], str]
            ] = collections.Counter()
            for refusal in rows:
                root_key = (
                    refusal["owner"], refusal["family"],
                    refusal["diagnostic_code"], refusal["blocking_fact"],
                )
                root_events[root_key] += 1
                gap = row_evidence_gap(refusal, codes)
                if gap is not None:
                    gap_key = (
                        str(gap["classification"]), gap["owner"], gap["family"],
                        tuple(gap["observed_diagnostic_codes"]),
                        str(gap["required_action"]),
                    )
                    gap_events[gap_key] += 1
            if not rows:
                gap = missing_source_gap(raw_log, codes)
                gap_key = (
                    str(gap["classification"]), gap["owner"], gap["family"],
                    tuple(gap["observed_diagnostic_codes"]),
                    str(gap["required_action"]),
                )
                gap_events[gap_key] += 1
            roots = [
                {
                    "diagnostic_code": diagnostic_code,
                    "owner": owner,
                    "family": family,
                    "blocking_fact": fact,
                    "event_count": count,
                    "case_count": 1,
                    "cases": [case["path"]],
                }
                for (owner, family, diagnostic_code, fact), count in sorted(
                    root_events.items(),
                    key=lambda item: (
                        item[0][0], item[0][1], item[0][2] or "", item[0][3]
                    ),
                )
            ]
            gaps = [
                {
                    "classification": classification,
                    "owner": owner,
                    "family": family,
                    "observed_diagnostic_codes": list(observed_codes),
                    "required_action": required_action,
                    "event_count": count,
                    "case_count": 1,
                    "cases": [case["path"]],
                }
                for (
                    classification, owner, family, observed_codes, required_action
                ), count in sorted(
                    gap_events.items(),
                    key=lambda item: (
                        item[0][0], item[0][1] or "", item[0][2] or "",
                        item[0][3], item[0][4],
                    ),
                )
            ]
            incomplete = bool(gaps)
            bound_count = sum(
                refusal["diagnostic_code"] is not None for refusal in rows
            )
            return {
                "schema": SCHEMA,
                "kind": KIND,
                "generator": GENERATOR,
                "status": "failed" if incomplete else "passed",
                "identity": {},
                "coverage": {
                    "listed_refusal_count": 1,
                    "observed_refusal_count": 1,
                    "new_refusals": [],
                    "resolved_refusals": [],
                },
                "measurement": {
                    "profile": "hosted",
                    "artifact": "native-executable",
                    "host_c_optimization": "0",
                    "xi_optimization": "pipeline-default",
                    "backends": ["vm", "aot"],
                    "collect_all_refusals": True,
                    "toolchain": {
                        "request": {
                            "target": "native", "profile": "hosted",
                            "normalizedTarget": "x",
                        },
                        "selection": {
                            "ready": True, "fallbackUsed": False,
                            "provider": "p", "runtimeArtifact": "r",
                        },
                    },
                },
                "inputs": {
                    "case_roots": ["tests/diff/cases"],
                    "case_manifests": [],
                    "case_count": 1,
                    "cases": [case],
                },
                "results": [{
                    "case": case["path"],
                    "outcome": "refused",
                    "first_refusal": rows[0] if rows else None,
                    "refusals": rows,
                    "build_log": {
                        "path": f"evidence.json.logs/{log_name}",
                        "sha256": digest_bytes(raw_log),
                        "size_bytes": len(raw_log),
                    },
                    "failure": None,
                }],
                "root_causes": roots,
                "evidence_gaps": gaps,
                "summary": {
                    "case_count": 1,
                    "comparable_count": 0,
                    "expected_rejection_count": 0,
                    "refused_count": 1,
                    "skipped_count": 0,
                    "failed_count": 0,
                    "complete_refusal_count": 0 if incomplete else 1,
                    "incomplete_refusal_count": 1 if incomplete else 0,
                    "missing_source_refusal_case_count": 1 if not rows else 0,
                    "refusal_event_count": len(rows),
                    "diagnostic_bound_refusal_event_count": bound_count,
                    "unbound_refusal_event_count": len(rows) - bound_count,
                    "root_cause_count": len(roots),
                },
            }

        first_source = (
            "[refusal-survey] owner=target-plan-builder family=calls "
            "XR_TARGET_1001: unsupported argument operation=4 opcode=17"
        )
        named_source = (
            "[refusal-survey] owner=aot-representation-refinement "
            "family=refinement_definition_oracle "
            "XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE record=0 value=5 operation=6"
        )
        valid_log = (first_source + "\n" + named_source + "\n").encode()
        manifest = fixture_manifest(valid_log, "valid.log")
        if validate_core(manifest, path, codes):
            print("self-test rejected valid fixture", file=sys.stderr)
            return 1
        alternate_root = base / "alternate-root"
        alternate_registry = alternate_root / DIAGNOSTIC_REGISTRY
        alternate_registry.parent.mkdir(parents=True)
        alternate_registry.write_text(
            '[[code]]\nid = "XR_TARGET_1001"\n',
            encoding="utf-8",
        )
        alternate_codes = registered_diagnostic_codes(alternate_root)
        if not validate_core(manifest, path, alternate_codes):
            print("self-test ignored the selected root diagnostic registry", file=sys.stderr)
            return 1
        mutations: list[tuple[str, dict[str, Any]]] = []

        def add_mutation(name: str, mutation: dict[str, Any]) -> None:
            mutations.append((name, mutation))

        bad = copy.deepcopy(manifest); bad["schema"] = 2; add_mutation("schema-2", bad)
        bad = copy.deepcopy(manifest); bad["legacy"] = None; add_mutation("top-add", bad)
        bad = copy.deepcopy(manifest); del bad["summary"]; add_mutation("top-delete", bad)
        bad = copy.deepcopy(manifest); bad["results"][0]["diagnostic"] = {}; add_mutation("result-add", bad)
        bad = copy.deepcopy(manifest); del bad["results"][0]["failure"]; add_mutation("result-delete", bad)
        bad = copy.deepcopy(manifest); bad["results"][0]["first_refusal"] = copy.deepcopy(bad["results"][0]["refusals"][1]); add_mutation("first-row", bad)
        bad = copy.deepcopy(manifest); bad["results"][0]["first_refusal"]["owner"] = "legacy"; add_mutation("first-owner", bad)
        bad = copy.deepcopy(manifest); bad["results"][0]["refusals"][0]["diagnostic_code"] = "XR_TARGET_1003"; add_mutation("code-substitute", bad)
        bad = copy.deepcopy(manifest); del bad["results"][0]["refusals"][0]["diagnostic_code"]; add_mutation("code-delete", bad)
        bad = copy.deepcopy(manifest); bad["results"][0]["refusals"][0]["extra_code"] = "XR_TARGET_1001"; add_mutation("code-add", bad)
        bad = copy.deepcopy(manifest); bad["summary"]["refused_count"] = 0; add_mutation("summary-refused", bad)
        bad = copy.deepcopy(manifest); bad["summary"]["diagnostic_bound_refusal_event_count"] = 1; add_mutation("summary-bound", bad)
        bad = copy.deepcopy(manifest); bad["summary"]["unbound_refusal_event_count"] = 1; add_mutation("summary-unbound", bad)
        bad = copy.deepcopy(manifest); bad["summary"]["root_cause_count"] = 0; add_mutation("summary-root", bad)
        bad = copy.deepcopy(manifest); bad["root_causes"][0]["diagnostic_code"] = "XR_TARGET_1003"; add_mutation("root-code", bad)
        bad = copy.deepcopy(manifest); bad["root_causes"][0]["event_count"] += 1; add_mutation("root-count", bad)
        bad = copy.deepcopy(manifest); bad["root_causes"].pop(); add_mutation("root-delete", bad)
        bad = copy.deepcopy(manifest); bad["evidence_gaps"] = [{"legacy": True}]; add_mutation("gap-add", bad)
        bad = copy.deepcopy(manifest); bad["coverage"]["observed_refusal_count"] = 0; add_mutation("coverage", bad)
        bad = copy.deepcopy(manifest); bad["results"][0]["refusals"][0]["sequence"] = 1; add_mutation("sequence", bad)
        bad = copy.deepcopy(manifest); del bad["results"][0]["refusals"][0]["line_number"]; add_mutation("line-delete", bad)
        bad = copy.deepcopy(manifest); bad["results"][0]["refusals"][0]["source_text"] += " "; add_mutation("source", bad)
        bad = copy.deepcopy(manifest); bad["results"][0]["refusals"].reverse(); add_mutation("order", bad)
        bad = copy.deepcopy(manifest); bad["results"][0]["refusals"].append(copy.deepcopy(bad["results"][0]["refusals"][0])); add_mutation("row-add", bad)
        bad = copy.deepcopy(manifest); bad["results"][0]["refusals"].pop(); add_mutation("row-delete", bad)
        bad = copy.deepcopy(manifest); bad["results"][0]["build_log"]["sha256"] = "0" * 64; add_mutation("hash", bad)
        for name, mutation in mutations:
            if not validate_core(mutation, path, codes):
                print(f"self-test mutation {name} was accepted", file=sys.stderr)
                return 1

        valid_log_path = log_dir / "valid.log"
        valid_log_path.write_bytes(valid_log + b"mutation\n")
        if not validate_core(manifest, path, codes):
            print("self-test log mutation was accepted", file=sys.stderr)
            return 1
        missing_log = b"Error: XR_TARGET_1000: authority was not produced\n"
        missing = fixture_manifest(missing_log, "missing.log")
        missing_errors = validate_core(missing, path, codes)
        if not any("missing source-emitted refusal evidence" in error for error in missing_errors) \
                or any("refusal rows do not match raw log" in error for error in missing_errors):
            print("self-test did not classify missing source evidence precisely", file=sys.stderr)
            return 1

        uncoded_source = (
            "[refusal-survey] owner=target-plan-builder family=calls "
            "XR_INTERNAL_ONLY unsupported argument"
        )
        uncoded_log = (
            uncoded_source + "\nError: XR_TARGET_1000: later terminal diagnostic\n"
        ).encode()
        uncoded = fixture_manifest(uncoded_log, "uncoded.log")
        uncoded_errors = validate_core(uncoded, path, codes)
        if uncoded["results"][0]["refusals"][0]["diagnostic_code"] is not None \
                or uncoded["evidence_gaps"][0]["classification"] \
                != "source-refusal-without-diagnostic" \
                or any("refusal rows do not match raw log" in error for error in uncoded_errors) \
                or not any("missing source-emitted refusal evidence" in error for error in uncoded_errors):
            print("self-test let a terminal diagnostic satisfy source-row debt", file=sys.stderr)
            return 1

        ambiguous_source = (
            "[refusal-survey] owner=target-plan-builder family=calls "
            "XR_TARGET_1000 XR_TARGET_1003 conflicting diagnostics"
        )
        ambiguous = fixture_manifest((ambiguous_source + "\n").encode(), "ambiguous.log")
        ambiguous_errors = validate_core(ambiguous, path, codes)
        if ambiguous["results"][0]["refusals"][0]["diagnostic_code"] is not None \
                or ambiguous["evidence_gaps"][0]["classification"] \
                != "source-refusal-with-ambiguous-diagnostic" \
                or any("refusal rows do not match raw log" in error for error in ambiguous_errors) \
                or not any("missing source-emitted refusal evidence" in error for error in ambiguous_errors):
            print("self-test did not preserve ambiguous source-row debt", file=sys.stderr)
            return 1

        failure = copy.deepcopy(manifest)
        failure["status"] = "failed"
        failure["inputs"]["cases"][0]["listed_refusal"] = False
        failure["coverage"] = {
            "listed_refusal_count": 0,
            "observed_refusal_count": 0,
            "new_refusals": [],
            "resolved_refusals": [],
        }
        failure["results"][0] = {
            "case": case["path"],
            "outcome": "fail",
            "first_refusal": None,
            "refusals": [],
            "build_log": None,
            "failure": "differential runner failed",
        }
        failure["root_causes"] = []
        failure["evidence_gaps"] = []
        failure["summary"] = {
            "case_count": 1,
            "comparable_count": 0,
            "expected_rejection_count": 0,
            "refused_count": 0,
            "skipped_count": 0,
            "failed_count": 1,
            "complete_refusal_count": 0,
            "incomplete_refusal_count": 0,
            "missing_source_refusal_case_count": 0,
            "refusal_event_count": 0,
            "diagnostic_bound_refusal_event_count": 0,
            "unbound_refusal_event_count": 0,
            "root_cause_count": 0,
        }
        failure_errors = validate_core(failure, path, codes)
        if not any("non-refusal differential failures" in error for error in failure_errors) \
                or any("refusal evidence" in error for error in failure_errors):
            print("self-test mixed non-refusal failure with refusal evidence", file=sys.stderr)
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
