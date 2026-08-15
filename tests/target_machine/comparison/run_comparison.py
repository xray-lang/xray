#!/usr/bin/env python3
"""Compare test-only target executors and report exact first divergence."""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any


def _bootstrap() -> None:
    tests_lib = Path(__file__).resolve().parents[2] / "lib"
    if str(tests_lib) not in sys.path:
        sys.path.insert(0, str(tests_lib))


_bootstrap()
from xraytest import proc  # noqa: E402


LANE_LEGACY = "legacy-vm-frozen"
LANE_TYPED = "typed-target-plan-vm"
LANE_AOT = "aot-native-current"
OBSERVATION_KEYS = {
    "value",
    "error",
    "termination",
    "destruction",
    "lifecycle",
    "trace",
}
COMMON_OBSERVATION_FIELDS = (
    "value",
    "error",
    "termination",
    "destruction",
    "lifecycle",
)
DESTRUCTION_FIELDS = ("allocations", "releases", "drops")
TRACE_SCHEMA = "xray-canonical-logical-safepoint-trace/1"
TRACE_EVENT_FIELDS = (
    "ordinal",
    "kind",
    "function",
    "relatedFunction",
    "instruction",
    "block",
    "call",
    "frame",
    "parentFrame",
    "relatedFrame",
    "frameDepth",
    "status",
    "opcode",
    "semanticOperationIdentity",
    "sourceSpanIdentity",
    "ownerIdentity",
    "layoutFingerprint",
    "coroutineStateIdentity",
)
TRACE_EVENT_KINDS = {
    "frame-enter",
    "block-enter",
    "instruction",
    "call-enter",
    "call-return",
    "error",
    "frame-exit",
}
DESTRUCTION_ZERO = {"allocations": 0, "releases": 0, "drops": 0}
BINARY_OPERATORS = {
    "add": "+",
    "sub": "-",
    "mul": "*",
    "bit-and": "&",
    "bit-or": "|",
    "bit-xor": "^",
    "shift-left": "<<",
    "shift-right": ">>",
    "divide": "/",
    "divide-by-zero": "/",
    "modulo": "%",
    "modulo-by-zero": "%",
}
RELATION_OPERATORS = {
    "cfg-eq": "==",
    "cfg-ne": "!=",
    "cfg-lt": "<",
    "cfg-le": "<=",
    "cfg-gt": ">",
    "cfg-ge": ">=",
}
DIRECT_CALL_OPERATORS = {
    "direct-function-call": "+",
    "direct-function-call-error": "/",
}


class ComparisonFailure(RuntimeError):
    pass


@dataclass(frozen=True)
class RawLaneResult:
    returncode: int
    stdout: bytes
    stderr: bytes
    timed_out: bool


def run_raw(
    command: list[Path | str], timeout: float, env: dict[str, str] | None = None
) -> RawLaneResult:
    result = proc.run(command, timeout=timeout, env=env)
    return RawLaneResult(
        returncode=result.returncode,
        stdout=result.stdout,
        stderr=result.stderr,
        timed_out=result.timed_out,
    )


def output_text(raw: RawLaneResult) -> str:
    return (raw.stdout + raw.stderr).decode("utf-8", "replace")


def require_transport(lane: str, raw: RawLaneResult) -> None:
    if raw.timed_out:
        raise ComparisonFailure(f"{lane}: timed out")
    if not raw.stdout and not raw.stderr:
        raise ComparisonFailure(f"{lane}: produced empty output")


def unavailable_trace(reason: str) -> dict[str, Any]:
    return {"availability": "unavailable", "reason": reason}


def validate_optional_identity(label: str, value: Any, digits: int) -> None:
    if value is not None and (
        not isinstance(value, str)
        or re.fullmatch(rf"[0-9a-f]{{{digits}}}", value) is None
    ):
        raise ComparisonFailure(f"{label}: identity is not canonical lowercase hex")


def validate_trace(label: str, trace: Any) -> dict[str, Any]:
    if not isinstance(trace, dict) or "availability" not in trace:
        raise ComparisonFailure(f"{label}: trace availability is missing")
    availability = trace["availability"]
    if availability == "unavailable":
        if set(trace) != {"availability", "reason"} or not isinstance(
            trace["reason"], str
        ) or not trace["reason"].strip():
            raise ComparisonFailure(
                f"{label}: unavailable trace requires one explicit reason"
            )
        return trace
    if availability != "available" or set(trace) != {
        "availability",
        "schema",
        "targetPlanFingerprint",
        "safepoints",
    }:
        raise ComparisonFailure(f"{label}: trace schema fields are not exact")
    if trace["schema"] != TRACE_SCHEMA:
        raise ComparisonFailure(f"{label}: canonical trace schema mismatch")
    if (
        not isinstance(trace["targetPlanFingerprint"], str)
        or re.fullmatch(r"[0-9a-f]{64}", trace["targetPlanFingerprint"]) is None
    ):
        raise ComparisonFailure(f"{label}: target plan fingerprint is invalid")
    safepoints = trace["safepoints"]
    if not isinstance(safepoints, list) or not safepoints:
        raise ComparisonFailure(f"{label}: available trace has no safepoints")
    for index, event in enumerate(safepoints):
        if not isinstance(event, dict) or set(event) != set(TRACE_EVENT_FIELDS):
            raise ComparisonFailure(f"{label}: safepoint {index} fields are not exact")
        if event["ordinal"] != index or event["kind"] not in TRACE_EVENT_KINDS:
            raise ComparisonFailure(f"{label}: safepoint {index} order is not canonical")
        for field in (
            "ordinal",
            "function",
            "frame",
            "frameDepth",
            "status",
            "opcode",
        ):
            if type(event[field]) is not int or event[field] < 0:
                raise ComparisonFailure(
                    f"{label}: safepoint {index} {field} is not unsigned"
                )
        for field in (
            "relatedFunction",
            "instruction",
            "block",
            "call",
            "parentFrame",
            "relatedFrame",
        ):
            if event[field] is not None and (
                type(event[field]) is not int or event[field] < 0
            ):
                raise ComparisonFailure(
                    f"{label}: safepoint {index} {field} is invalid"
                )
        for field in (
            "semanticOperationIdentity",
            "sourceSpanIdentity",
            "ownerIdentity",
            "coroutineStateIdentity",
        ):
            validate_optional_identity(
                f"{label}: safepoint {index} {field}", event[field], 32
            )
        validate_optional_identity(
            f"{label}: safepoint {index} layoutFingerprint",
            event["layoutFingerprint"],
            64,
        )
    if safepoints[0]["kind"] != "frame-enter" or safepoints[-1]["kind"] != "frame-exit":
        raise ComparisonFailure(f"{label}: trace lacks exact frame lifetime boundaries")
    return trace


def validate_observation(
    label: str, observation: Any, expected_trace_availability: str | None = None
) -> dict[str, Any]:
    if not isinstance(observation, dict) or set(observation) != OBSERVATION_KEYS:
        raise ComparisonFailure(f"{label}: observation fields are incomplete")
    if observation["termination"] not in ("returned", "error"):
        raise ComparisonFailure(f"{label}: unknown termination")
    if observation["destruction"] != DESTRUCTION_ZERO:
        raise ComparisonFailure(f"{label}: scalar destruction must be canonical zero")
    if observation["lifecycle"] != []:
        raise ComparisonFailure(f"{label}: scalar lifecycle must be canonical empty")
    trace = validate_trace(f"{label}:trace", observation["trace"])
    if (
        expected_trace_availability is not None
        and trace["availability"] != expected_trace_availability
    ):
        raise ComparisonFailure(
            f"{label}: trace availability differs from the frozen lane capability"
        )
    if observation["termination"] == "returned":
        if (
            not isinstance(observation["value"], int)
            or observation["error"] is not None
        ):
            raise ComparisonFailure(f"{label}: returned observation is inconsistent")
    elif observation["value"] is not None or not isinstance(observation["error"], str):
        raise ComparisonFailure(f"{label}: error observation is inconsistent")
    return observation


def language_observation(
    lane: str,
    raw: RawLaneResult,
    oracle: dict[str, Any],
    trace_unavailable_reason: str,
) -> dict[str, Any]:
    require_transport(lane, raw)
    if oracle["termination"] == "returned":
        if raw.returncode != 0:
            raise ComparisonFailure(
                f"{lane}: expected return, rc={raw.returncode}: {output_text(raw).strip()}"
            )
        text = raw.stdout.decode("utf-8", "replace")
        if re.fullmatch(r"-?[0-9]+\r?\n?", text) is None:
            raise ComparisonFailure(
                f"{lane}: value output is not one canonical integer: {text!r}"
            )
        return canonical_observation(
            value=int(text.strip()), trace=unavailable_trace(trace_unavailable_reason)
        )

    if raw.returncode != 1:
        raise ComparisonFailure(
            f"{lane}: expected language error rc=1, got {raw.returncode}: "
            f"{output_text(raw).strip()}"
        )
    diagnostic = output_text(raw).lower()
    expected_phrase = oracle["error"].replace("-", " ")
    if expected_phrase not in diagnostic:
        raise ComparisonFailure(
            f"{lane}: missing {expected_phrase!r} diagnostic: {diagnostic.strip()}"
        )
    return canonical_observation(
        error=oracle["error"], trace=unavailable_trace(trace_unavailable_reason)
    )


def typed_observation(raw: RawLaneResult) -> dict[str, Any]:
    require_transport(LANE_TYPED, raw)
    if raw.returncode != 0:
        raise ComparisonFailure(
            f"{LANE_TYPED}: fixture failed rc={raw.returncode}: {output_text(raw).strip()}"
        )
    try:
        parsed = json.loads(raw.stdout)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ComparisonFailure(
            f"{LANE_TYPED}: malformed observation JSON: {exc}"
        ) from exc
    return validate_observation(LANE_TYPED, parsed, "available")


def canonical_observation(
    *,
    value: int | None = None,
    error: str | None = None,
    trace: dict[str, Any] | None = None,
) -> dict[str, Any]:
    return {
        "value": value,
        "error": error,
        "termination": "error" if error else "returned",
        "destruction": dict(DESTRUCTION_ZERO),
        "lifecycle": [],
        "trace": trace
        if trace is not None
        else unavailable_trace(
            "The independent scalar oracle does not execute a target machine."
        ),
    }


def first_sequence_divergence(
    field: str, expected: list[Any], actual: list[Any]
) -> dict[str, Any] | None:
    for index in range(max(len(expected), len(actual))):
        if index >= len(expected):
            return {
                "field": field,
                "eventIndex": index,
                "reason": "unexpected-event",
                "expected": None,
                "actual": actual[index],
            }
        if index >= len(actual):
            return {
                "field": field,
                "eventIndex": index,
                "reason": "missing-event",
                "expected": expected[index],
                "actual": None,
            }
        if expected[index] != actual[index]:
            divergence: dict[str, Any] = {
                "field": field,
                "eventIndex": index,
                "reason": "event-mismatch",
                "expected": expected[index],
                "actual": actual[index],
            }
            if field == "trace":
                for event_field in TRACE_EVENT_FIELDS:
                    if expected[index].get(event_field) != actual[index].get(
                        event_field
                    ):
                        divergence["eventField"] = event_field
                        divergence["expected"] = expected[index].get(event_field)
                        divergence["actual"] = actual[index].get(event_field)
                        break
            return divergence
    return None


def first_common_divergence(
    expected: dict[str, Any], actual: dict[str, Any]
) -> dict[str, Any] | None:
    for field in COMMON_OBSERVATION_FIELDS:
        if expected[field] == actual[field]:
            continue
        if field == "lifecycle":
            return first_sequence_divergence(field, expected[field], actual[field])
        if field == "destruction":
            for counter in DESTRUCTION_FIELDS:
                if expected[field].get(counter) != actual[field].get(counter):
                    return {
                        "field": field,
                        "counter": counter,
                        "expected": expected[field].get(counter),
                        "actual": actual[field].get(counter),
                    }
        return {
            "field": field,
            "expected": expected[field],
            "actual": actual[field],
        }
    return None


def first_trace_divergence(
    expected: dict[str, Any], actual: dict[str, Any]
) -> dict[str, Any] | None:
    if expected["schema"] != actual["schema"]:
        return {
            "field": "trace",
            "traceField": "schema",
            "expected": expected["schema"],
            "actual": actual["schema"],
        }
    if expected["targetPlanFingerprint"] != actual["targetPlanFingerprint"]:
        return {
            "field": "trace",
            "traceField": "targetPlanFingerprint",
            "expected": expected["targetPlanFingerprint"],
            "actual": actual["targetPlanFingerprint"],
        }
    return first_sequence_divergence(
        "trace", expected["safepoints"], actual["safepoints"]
    )


def align_common_observables(
    oracle: dict[str, Any], observations: dict[str, dict[str, Any]]
) -> dict[str, Any]:
    for lane in (LANE_LEGACY, LANE_TYPED, LANE_AOT):
        divergence = first_common_divergence(oracle, observations[lane])
        if divergence is not None:
            return {
                "status": "diverged",
                "lane": lane,
                "firstDivergence": divergence,
            }
    return {"status": "matched", "firstDivergence": None}


def compare_canonical_traces(
    observations: dict[str, dict[str, Any]], capabilities: dict[str, dict[str, Any]]
) -> dict[str, Any]:
    unavailable_lanes: list[dict[str, str]] = []
    for lane in (LANE_LEGACY, LANE_TYPED, LANE_AOT):
        trace = observations[lane]["trace"]
        capability = capabilities[lane]
        if trace["availability"] != capability["availability"]:
            raise ComparisonFailure(
                f"{lane}: observed trace availability violates the manifest"
            )
        if trace["availability"] == "unavailable":
            unavailable_lanes.append(
                {"lane": lane, "reason": capability["reason"]}
            )
    if unavailable_lanes:
        return {
            "status": "unavailable",
            "blocksW7FirstDivergence": True,
            "unavailableLanes": unavailable_lanes,
            "firstDivergence": None,
        }

    baseline_lane = LANE_LEGACY
    baseline = observations[baseline_lane]["trace"]
    for lane in (LANE_TYPED, LANE_AOT):
        divergence = first_trace_divergence(baseline, observations[lane]["trace"])
        if divergence is not None:
            return {
                "status": "diverged",
                "blocksW7FirstDivergence": True,
                "baselineLane": baseline_lane,
                "lane": lane,
                "firstDivergence": divergence,
            }
    return {
        "status": "matched",
        "blocksW7FirstDivergence": False,
        "firstDivergence": None,
    }


def expect_failure(label: str, action: Any) -> None:
    try:
        action()
    except ComparisonFailure:
        return
    raise ComparisonFailure(f"runner self-test did not reject {label}")


def self_test_transport() -> None:
    oracle = canonical_observation(value=1)
    reason = "self-test lane has no canonical trace"
    empty = run_raw([sys.executable, "-c", "pass"], 2)
    expect_failure(
        "empty output",
        lambda: language_observation("self-empty", empty, oracle, reason),
    )
    crashed = run_raw(
        [sys.executable, "-c", "import sys; print('crash'); sys.exit(7)"], 2
    )
    expect_failure(
        "abnormal exit",
        lambda: language_observation("self-crash", crashed, oracle, reason),
    )
    hung = run_raw([sys.executable, "-c", "import time; time.sleep(2)"], 0.02)
    expect_failure(
        "timeout", lambda: language_observation("self-hang", hung, oracle, reason)
    )
    expect_failure("typed empty output", lambda: typed_observation(empty))
    expect_failure("typed abnormal exit", lambda: typed_observation(crashed))
    expect_failure("typed timeout", lambda: typed_observation(hung))


def self_test_first_divergence() -> None:
    expected = canonical_observation(value=7)
    mutations = {
        "value": {**expected, "value": 8},
        "error": {**expected, "error": "unexpected"},
        "termination": {**expected, "termination": "error"},
        "destruction": {
            **expected,
            "destruction": {**DESTRUCTION_ZERO, "drops": 1},
        },
        "lifecycle": {**expected, "lifecycle": [{"kind": "drop"}]},
    }
    for field, actual in mutations.items():
        divergence = first_common_divergence(expected, actual)
        if divergence is None or divergence["field"] != field:
            raise ComparisonFailure(
                f"runner self-test missed exact {field} first divergence"
            )

    event = {field: None for field in TRACE_EVENT_FIELDS}
    event.update(
        {
            "ordinal": 0,
            "kind": "frame-enter",
            "function": 0,
            "frame": 0,
            "frameDepth": 0,
            "status": 0,
            "opcode": 0,
        }
    )
    second = dict(event)
    second.update({"ordinal": 1, "kind": "frame-exit"})
    trace = {
        "availability": "available",
        "schema": TRACE_SCHEMA,
        "targetPlanFingerprint": "01" * 32,
        "safepoints": [event, second],
    }
    mutated_second = dict(second)
    mutated_second["status"] = 9
    mutated = {**trace, "safepoints": [event, mutated_second]}
    divergence = first_trace_divergence(trace, mutated)
    if divergence != {
        "field": "trace",
        "eventIndex": 1,
        "reason": "event-mismatch",
        "eventField": "status",
        "expected": 0,
        "actual": 9,
    }:
        raise ComparisonFailure(
            "runner self-test did not identify the exact first divergent safepoint"
        )
    missing = {**trace, "safepoints": [event]}
    divergence = first_trace_divergence(trace, missing)
    if divergence is None or divergence.get("eventIndex") != 1 or divergence.get(
        "reason"
    ) != "missing-event":
        raise ComparisonFailure("runner self-test missed a truncated trace")
    unavailable = unavailable_trace("canonical events are not emitted")
    expect_failure(
        "unavailable trace without reason",
        lambda: validate_trace("self-unavailable", {"availability": "unavailable"}),
    )
    validate_trace("self-trace", trace)
    validate_trace("self-unavailable", unavailable)


def read_trace_capabilities(manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    expected_lanes = (LANE_LEGACY, LANE_TYPED, LANE_AOT)
    contract = manifest.get("canonicalLogicalSafepointTrace")
    if not isinstance(contract, dict) or set(contract) != {
        "schema",
        "requiredForW7FirstDivergence",
        "lanes",
    }:
        raise ComparisonFailure("canonical trace contract fields are incomplete")
    if (
        contract["schema"] != TRACE_SCHEMA
        or contract["requiredForW7FirstDivergence"] is not True
        or not isinstance(contract["lanes"], list)
        or len(contract["lanes"]) != len(expected_lanes)
    ):
        raise ComparisonFailure("canonical trace contract is not fail closed")
    capabilities: dict[str, dict[str, Any]] = {}
    for expected_lane, capability in zip(
        expected_lanes, contract["lanes"], strict=False
    ):
        if not isinstance(capability, dict) or capability.get("lane") != expected_lane:
            raise ComparisonFailure("canonical trace lane order is not exact")
        availability = capability.get("availability")
        expected_fields = (
            {"lane", "availability"}
            if availability == "available"
            else {"lane", "availability", "reason"}
        )
        if set(capability) != expected_fields or availability not in {
            "available",
            "unavailable",
        }:
            raise ComparisonFailure(f"{expected_lane}: trace capability is invalid")
        if availability == "unavailable" and (
            not isinstance(capability["reason"], str)
            or not capability["reason"].strip()
        ):
            raise ComparisonFailure(
                f"{expected_lane}: unavailable trace capability lacks a reason"
            )
        capabilities[expected_lane] = capability
    if set(capabilities) != set(expected_lanes):
        raise ComparisonFailure("canonical trace lane set is incomplete")
    if capabilities[LANE_TYPED]["availability"] != "available":
        raise ComparisonFailure("typed lane must expose its real canonical trace")
    if capabilities[LANE_AOT]["availability"] != "unavailable":
        raise ComparisonFailure("current AOT trace absence must remain explicit")
    return capabilities


def read_manifest(path: Path) -> dict[str, Any]:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ComparisonFailure(f"cannot read manifest: {exc}") from exc
    expected_lanes = [LANE_LEGACY, LANE_TYPED, LANE_AOT]
    if set(manifest) != {
        "schema",
        "laneTimeoutSeconds",
        "aotBuildTimeoutSeconds",
        "executorLanes",
        "unavailableExecutorLanes",
        "canonicalLogicalSafepointTrace",
        "cases",
        "unsupportedPositiveCases",
    }:
        raise ComparisonFailure("comparison manifest fields are not exact")
    if manifest.get("schema") != "xray-target-machine-comparison/2":
        raise ComparisonFailure("comparison manifest schema mismatch")
    if manifest.get("executorLanes") != expected_lanes:
        raise ComparisonFailure(
            "comparison executor lanes are not the exact private set"
        )
    cases = manifest.get("cases")
    unsupported = manifest.get("unsupportedPositiveCases")
    unavailable = manifest.get("unavailableExecutorLanes")
    timeouts = manifest.get("laneTimeoutSeconds")
    read_trace_capabilities(manifest)
    if not isinstance(cases, list) or not cases:
        raise ComparisonFailure("comparison manifest has no executable cases")
    if not isinstance(unsupported, list) or not unsupported:
        raise ComparisonFailure(
            "comparison manifest lost its unsupported positive inventory"
        )
    if (
        not isinstance(unavailable, list)
        or len(unavailable) != 1
        or unavailable[0].get("id") != "aot-baseline-vs-refined-separate"
        or unavailable[0].get("status") != "unavailable"
        or not unavailable[0].get("reason")
    ):
        raise ComparisonFailure("comparison manifest lost the refined AOT boundary")
    if not isinstance(timeouts, dict) or set(timeouts) != set(expected_lanes):
        raise ComparisonFailure("comparison manifest lane timeout set is incomplete")
    if any(
        type(value) not in (int, float) or value <= 0 for value in timeouts.values()
    ):
        raise ComparisonFailure("comparison lane timeouts must be positive numbers")
    build_timeout = manifest.get("aotBuildTimeoutSeconds")
    if type(build_timeout) not in (int, float) or build_timeout <= 0:
        raise ComparisonFailure("comparison AOT build timeout must be positive")
    ids: set[str] = set()
    for case in cases:
        case_id = case.get("id") if isinstance(case, dict) else None
        arguments = case.get("arguments") if isinstance(case, dict) else None
        if not isinstance(case_id, str) or case_id in ids:
            raise ComparisonFailure("comparison case IDs must be unique strings")
        if (
            not isinstance(arguments, list)
            or len(arguments) != 2
            or not all(type(value) is int for value in arguments)
        ):
            raise ComparisonFailure(
                f"{case_id}: expected exactly two integer arguments"
            )
        if (
            case_id not in BINARY_OPERATORS
            and case_id not in RELATION_OPERATORS
            and case_id not in DIRECT_CALL_OPERATORS
        ):
            raise ComparisonFailure(f"{case_id}: comparison operation is not mapped")
        validate_observation(f"oracle:{case_id}", case.get("oracle"), "unavailable")
        ids.add(case_id)
    expected_cases = (
        set(BINARY_OPERATORS) | set(RELATION_OPERATORS) | set(DIRECT_CALL_OPERATORS)
    )
    if ids != expected_cases:
        raise ComparisonFailure(
            "comparison manifest does not cover the exact scalar corpus"
        )
    gap_ids: set[str] = set()
    for gap in unsupported:
        if (
            not isinstance(gap, dict)
            or gap.get("typedStatus") != "unavailable"
            or gap.get("blocksCompletion") is not True
            or not gap.get("id")
            or not gap.get("family")
            or not gap.get("reason")
        ):
            raise ComparisonFailure(
                "unsupported positive inventory entry is incomplete"
            )
        if gap["id"] in gap_ids:
            raise ComparisonFailure("unsupported positive inventory IDs must be unique")
        gap_ids.add(gap["id"])
    return manifest


def resolve_native_binary(requested: Path) -> Path:
    if requested.is_file():
        return requested
    windows_binary = requested.with_suffix(".exe")
    if windows_binary.is_file():
        return windows_binary
    raise ComparisonFailure(
        "AOT build succeeded without producing its requested binary"
    )


def build_aot(xray: Path, source: Path, output: Path, timeout: float) -> Path:
    env = os.environ.copy()
    env.setdefault("XRAY_AOT_FAST_TEST_BUILD", "1")
    env.setdefault("XRAY_TOOLCHAIN_PROBE_SCALE", "4")
    raw = run_raw(
        [xray, "build", "--native", "-O", "0", source, "-o", output],
        timeout,
        env,
    )
    if raw.timed_out:
        raise ComparisonFailure("AOT native build timed out")
    if raw.returncode != 0:
        raise ComparisonFailure(
            f"AOT native build failed rc={raw.returncode}: {output_text(raw).strip()}"
        )
    return resolve_native_binary(output)


def xray_identity(xray: Path) -> dict[str, Any]:
    raw = run_raw([xray, "--version", "--json"], 10)
    require_transport("xray-identity", raw)
    if raw.returncode != 0:
        raise ComparisonFailure("xray identity command failed")
    try:
        parsed = json.loads(raw.stdout)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ComparisonFailure(f"xray identity is not JSON: {exc}") from exc
    return parsed


def render_source(case: dict[str, Any]) -> str:
    case_id = case["id"]
    left, right = case["arguments"]
    if case_id in DIRECT_CALL_OPERATORS:
        operator = DIRECT_CALL_OPERATORS[case_id]
        return (
            "fn callee(left: int, right: int) -> int {\n"
            f"    return left {operator} right\n"
            "}\n\n"
            f"print(callee({left}, {right}))\n"
        )
    if case_id in BINARY_OPERATORS:
        body = f"    return left {BINARY_OPERATORS[case_id]} right\n"
    else:
        relation = RELATION_OPERATORS[case_id]
        body = (
            f"    if (left {relation} right) {{ return left + right }}\n"
            "    return left - right\n"
        )
    return (
        "fn evaluate(left: int, right: int) -> int {\n"
        f"{body}"
        "}\n\n"
        f"print(evaluate({left}, {right}))\n"
    )


def run_case(
    case: dict[str, Any],
    xray: Path,
    typed_fixture: Path,
    work_dir: Path,
    timeouts: dict[str, Any],
    aot_build_timeout: float,
    trace_capabilities: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    case_id = case["id"]
    value_args = [str(value) for value in case["arguments"]]
    oracle = case["oracle"]
    source = work_dir / f"{case_id}.xr"
    source.write_text(render_source(case), encoding="utf-8")
    lane_commands = {
        LANE_LEGACY: [xray, "run", source],
        LANE_TYPED: [typed_fixture, case_id, *value_args],
    }
    observations: dict[str, Any] = {}
    failures: list[str] = []
    for lane, command in lane_commands.items():
        try:
            timeout = float(timeouts[lane])
            raw = run_raw(command, timeout)
            observation = (
                typed_observation(raw)
                if lane == LANE_TYPED
                else language_observation(
                    lane, raw, oracle, trace_capabilities[lane]["reason"]
                )
            )
            observations[lane] = observation
            divergence = first_common_divergence(oracle, observation)
            if divergence is not None:
                failures.append(
                    f"{lane}: first common-observable divergence: "
                    f"{json.dumps(divergence, sort_keys=True)}"
                )
        except (ComparisonFailure, KeyError, TypeError, ValueError) as exc:
            observations[lane] = {"transportFailure": str(exc)}
            failures.append(str(exc))
    try:
        native_binary = build_aot(
            xray, source, work_dir / f"{case_id}-native", aot_build_timeout
        )
        raw = run_raw([native_binary], float(timeouts[LANE_AOT]))
        observation = language_observation(
            LANE_AOT, raw, oracle, trace_capabilities[LANE_AOT]["reason"]
        )
        observations[LANE_AOT] = observation
        divergence = first_common_divergence(oracle, observation)
        if divergence is not None:
            failures.append(
                f"{LANE_AOT}: first common-observable divergence: "
                f"{json.dumps(divergence, sort_keys=True)}"
            )
    except (ComparisonFailure, KeyError, TypeError, ValueError) as exc:
        observations[LANE_AOT] = {"transportFailure": str(exc)}
        failures.append(str(exc))
    alignment: dict[str, Any] = {
        "status": "transport-failure",
        "firstDivergence": None,
    }
    trace_comparison: dict[str, Any] = {
        "status": "transport-failure",
        "blocksW7FirstDivergence": True,
        "firstDivergence": None,
    }
    if set(observations) == {LANE_LEGACY, LANE_TYPED, LANE_AOT} and all(
        "transportFailure" not in observation for observation in observations.values()
    ):
        alignment = align_common_observables(oracle, observations)
        trace_comparison = compare_canonical_traces(
            observations, trace_capabilities
        )
    return {
        "id": case_id,
        "status": "failed" if failures else "passed",
        "oracle": oracle,
        "lanes": observations,
        "commonObservableAlignment": alignment,
        "canonicalLogicalSafepointTraceComparison": trace_comparison,
        "failures": failures,
    }


def write_report(path: Path, report: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", type=Path, required=True)
    parser.add_argument("--typed-fixture", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args(argv[1:])

    report: dict[str, Any] = {
        "schema": "xray-target-machine-comparison-report/2",
        "harnessStatus": "failed",
        "cases": [],
    }
    try:
        self_test_transport()
        self_test_first_divergence()
        manifest = read_manifest(args.manifest)
        trace_capabilities = read_trace_capabilities(manifest)
        if not args.xray.is_file() or not args.typed_fixture.is_file():
            raise ComparisonFailure("comparison executable is missing")
        report["toolIdentity"] = xray_identity(args.xray)
        report["executorLanes"] = manifest["executorLanes"]
        report["unavailableExecutorLanes"] = manifest["unavailableExecutorLanes"]
        report["unsupportedPositiveCases"] = manifest["unsupportedPositiveCases"]
        report["canonicalLogicalSafepointTrace"] = manifest[
            "canonicalLogicalSafepointTrace"
        ]
        report["verifiedCommonObservableFields"] = list(COMMON_OBSERVATION_FIELDS)
        report["w7FirstDivergence"] = {
            "status": "unavailable",
            "blocksCompletion": True,
            "reason": (
                "Canonical logical safepoint traces are not emitted by every "
                "required executor lane."
            ),
            "unavailableLanes": [
                {
                    "lane": lane,
                    "reason": capability["reason"],
                }
                for lane, capability in trace_capabilities.items()
                if capability["availability"] == "unavailable"
            ],
        }
        report["taskCompletionStatus"] = (
            "blocked-by-canonical-trace-and-unsupported-positive-cases"
        )
        with tempfile.TemporaryDirectory(prefix="xray-target-comparison-") as temp:
            for case in manifest["cases"]:
                report["cases"].append(
                    run_case(
                        case,
                        args.xray,
                        args.typed_fixture,
                        Path(temp),
                        manifest["laneTimeoutSeconds"],
                        float(manifest["aotBuildTimeoutSeconds"]),
                        trace_capabilities,
                    )
                )
        failed = [case["id"] for case in report["cases"] if case["status"] != "passed"]
        report["summary"] = {
            "passedCases": len(report["cases"]) - len(failed),
            "failedCases": failed,
            "unsupportedPositiveCount": len(manifest["unsupportedPositiveCases"]),
            "canonicalTraceStatus": "unavailable",
        }
        report["harnessStatus"] = "passed" if not failed else "failed"
    except (ComparisonFailure, KeyError, TypeError, ValueError) as exc:
        report["infrastructureFailure"] = str(exc)

    write_report(args.report, report)
    if report["harnessStatus"] != "passed":
        print(json.dumps(report, indent=2, sort_keys=True), file=sys.stderr)
        return 1
    print(
        "target-machine comparison: "
        f"{report['summary']['passedCases']} cases passed; "
        f"{report['summary']['unsupportedPositiveCount']} positive families and "
        "cross-lane canonical safepoint trace remain unavailable"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
