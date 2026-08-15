#!/usr/bin/env python3
"""Compare one scalar corpus across three executors and an independent oracle."""

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


def validate_observation(label: str, observation: Any) -> dict[str, Any]:
    if not isinstance(observation, dict) or set(observation) != OBSERVATION_KEYS:
        raise ComparisonFailure(f"{label}: observation fields are incomplete")
    if observation["termination"] not in ("returned", "error"):
        raise ComparisonFailure(f"{label}: unknown termination")
    if observation["destruction"] != DESTRUCTION_ZERO:
        raise ComparisonFailure(f"{label}: scalar destruction must be canonical zero")
    if observation["lifecycle"] != [] or observation["trace"] != []:
        raise ComparisonFailure(
            f"{label}: scalar lifecycle and trace must be canonical empty"
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
    lane: str, raw: RawLaneResult, oracle: dict[str, Any]
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
        return canonical_observation(value=int(text.strip()))

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
    return canonical_observation(error=oracle["error"])


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
    return validate_observation(LANE_TYPED, parsed)


def canonical_observation(
    *, value: int | None = None, error: str | None = None
) -> dict[str, Any]:
    return {
        "value": value,
        "error": error,
        "termination": "error" if error else "returned",
        "destruction": dict(DESTRUCTION_ZERO),
        "lifecycle": [],
        "trace": [],
    }


def expect_failure(label: str, action: Any) -> None:
    try:
        action()
    except ComparisonFailure:
        return
    raise ComparisonFailure(f"runner self-test did not reject {label}")


def self_test_transport() -> None:
    oracle = canonical_observation(value=1)
    empty = run_raw([sys.executable, "-c", "pass"], 2)
    expect_failure(
        "empty output", lambda: language_observation("self-empty", empty, oracle)
    )
    crashed = run_raw(
        [sys.executable, "-c", "import sys; print('crash'); sys.exit(7)"], 2
    )
    expect_failure(
        "abnormal exit", lambda: language_observation("self-crash", crashed, oracle)
    )
    hung = run_raw([sys.executable, "-c", "import time; time.sleep(2)"], 0.02)
    expect_failure("timeout", lambda: language_observation("self-hang", hung, oracle))


def read_manifest(path: Path) -> dict[str, Any]:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ComparisonFailure(f"cannot read manifest: {exc}") from exc
    expected_lanes = [LANE_LEGACY, LANE_TYPED, LANE_AOT]
    if manifest.get("schema") != "xray-target-machine-comparison/1":
        raise ComparisonFailure("comparison manifest schema mismatch")
    if manifest.get("executorLanes") != expected_lanes:
        raise ComparisonFailure(
            "comparison executor lanes are not the exact private set"
        )
    cases = manifest.get("cases")
    unsupported = manifest.get("unsupportedPositiveCases")
    unavailable = manifest.get("unavailableExecutorLanes")
    timeouts = manifest.get("laneTimeoutSeconds")
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
        if case_id not in BINARY_OPERATORS and case_id not in RELATION_OPERATORS:
            raise ComparisonFailure(f"{case_id}: comparison operation is not mapped")
        validate_observation(f"oracle:{case_id}", case.get("oracle"))
        ids.add(case_id)
    expected_cases = set(BINARY_OPERATORS) | set(RELATION_OPERATORS)
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
                else language_observation(lane, raw, oracle)
            )
            observations[lane] = observation
            if observation != oracle:
                failures.append(f"{lane}: observation differs from independent oracle")
        except (ComparisonFailure, KeyError, TypeError, ValueError) as exc:
            observations[lane] = {"transportFailure": str(exc)}
            failures.append(str(exc))
    try:
        native_binary = build_aot(
            xray, source, work_dir / f"{case_id}-native", aot_build_timeout
        )
        raw = run_raw([native_binary], float(timeouts[LANE_AOT]))
        observation = language_observation(LANE_AOT, raw, oracle)
        observations[LANE_AOT] = observation
        if observation != oracle:
            failures.append(f"{LANE_AOT}: observation differs from independent oracle")
    except (ComparisonFailure, KeyError, TypeError, ValueError) as exc:
        observations[LANE_AOT] = {"transportFailure": str(exc)}
        failures.append(str(exc))
    return {
        "id": case_id,
        "status": "failed" if failures else "passed",
        "oracle": oracle,
        "lanes": observations,
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
        "schema": "xray-target-machine-comparison-report/1",
        "harnessStatus": "failed",
        "cases": [],
    }
    try:
        self_test_transport()
        manifest = read_manifest(args.manifest)
        if not args.xray.is_file() or not args.typed_fixture.is_file():
            raise ComparisonFailure("comparison executable is missing")
        report["toolIdentity"] = xray_identity(args.xray)
        report["executorLanes"] = manifest["executorLanes"]
        report["unavailableExecutorLanes"] = manifest["unavailableExecutorLanes"]
        report["unsupportedPositiveCases"] = manifest["unsupportedPositiveCases"]
        report["taskCompletionStatus"] = "blocked-by-unsupported-positive-cases"
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
                    )
                )
        failed = [case["id"] for case in report["cases"] if case["status"] != "passed"]
        report["summary"] = {
            "passedCases": len(report["cases"]) - len(failed),
            "failedCases": failed,
            "unsupportedPositiveCount": len(manifest["unsupportedPositiveCases"]),
        }
        report["harnessStatus"] = "passed" if not failed else "failed"
    except (ComparisonFailure, KeyError, TypeError, ValueError) as exc:
        report["infrastructureFailure"] = str(exc)

    write_report(args.report, report)
    if report["harnessStatus"] != "passed":
        print(json.dumps(report, indent=2, sort_keys=True), file=sys.stderr)
        return 1
    print(
        "target-machine scalar comparison: "
        f"{report['summary']['passedCases']} cases passed; "
        f"{report['summary']['unsupportedPositiveCount']} positive families remain unavailable"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
