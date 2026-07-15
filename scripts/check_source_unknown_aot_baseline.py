#!/usr/bin/env python3
"""Check task-202 AOT baseline fixtures for typed-erasure convergence.

This is a P0 coverage gate. It does not claim the four surfaces are fixed; it
only makes sure their current AOT shape/gap evidence stays executable and
reviewable while later 202 phases replace erased boundaries with final typed
contracts.
"""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import asdict, dataclass
from pathlib import Path


@dataclass(frozen=True)
class Baseline:
    category: str
    xr_path: str
    expect_path: str
    source_contains: tuple[str, ...]
    expect_contains: tuple[str, ...]
    expect_regex: tuple[str, ...] = ()


@dataclass(frozen=True)
class CheckResult:
    category: str
    path: str
    ok: bool
    detail: str


BASELINES = (
    Baseline(
        category="TASK_AOT_BASELINE",
        xr_path="tests/aot/filetests/cgen/array_task_direct.xr",
        expect_path="tests/aot/filetests/cgen/array_task_direct.expect",
        source_contains=("Array<Task<int>>", "await tasks[0]"),
        expect_contains=("xr_aot_await_task", "xr_slot_aot_frame_offset"),
    ),
    Baseline(
        category="THREADLOCAL_AOT_BASELINE",
        xr_path="tests/aot/filetests/cgen/sys_threadlocal_typed_baseline.xr",
        expect_path="tests/aot/filetests/cgen/sys_threadlocal_typed_baseline.expect",
        source_contains=("sys.ThreadLocal<int>", "local.set(20)", "local.get()"),
        expect_contains=(
            "xrt_sys_thread_local_id()",
            "xrt_map_new_typed(0, XR_ELEM_I64, XR_ELEM_I64)",
            "xrt_map_new_typed(0, XR_ELEM_I64, XR_ELEM_BOOL)",
            'xrt_map_set_class_name(_inst, "ThreadLocal$i64")',
            r"c_regex=int64_t sys_[0-9a-f]+_ThreadLocal_i64_get_m",
            r"c_regex=void sys_[0-9a-f]+_ThreadLocal_i64_set_m",
            "xrt_map_has_i64_typed",
            "xrt_map_set_i64_i64_typed",
        ),
    ),
    Baseline(
        category="JSON_ENCODE_AOT_BASELINE",
        xr_path="tests/aot/filetests/cgen/json_decode_record_plan_consumption.xr",
        expect_path="tests/aot/filetests/cgen/json_decode_record_plan_consumption.expect",
        source_contains=("Json.decode<User>", "Json.encode(user!)"),
        expect_contains=(
            "kind=encode action=encode_field_table",
            "xrt_json_decode_record",
            "xrt_json_encode",
        ),
    ),
    Baseline(
        category="HTTP_HANDLER_AOT_BASELINE",
        xr_path="tests/aot/filetests/cgen/http_route_handler_boundary.xr",
        expect_path="tests/aot/filetests/cgen/http_route_handler_boundary.expect",
        source_contains=("http.routeHandler", "handle(req: Json) -> Json"),
        expect_contains=(
            "name=http.routeHandler",
            "c_not_contains=xrt_typename(",
            'c_not_contains=xrt_map_set_class_name(_inst, "_RouteHandler")',
            "xrt_json_get_name_owned",
            r"c_regex=void http_[0-9a-f]+_routeHandler_exp",
        ),
    ),
)


def check_contains(text: str, needles: tuple[str, ...]) -> list[str]:
    return [needle for needle in needles if needle not in text]


def check_regex(text: str, patterns: tuple[str, ...]) -> list[str]:
    missing: list[str] = []
    for pattern in patterns:
        if not re.search(pattern, text, re.MULTILINE):
            missing.append(pattern)
    return missing


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def check_baseline(root: Path, baseline: Baseline) -> list[CheckResult]:
    results: list[CheckResult] = []
    xr_file = root / baseline.xr_path
    expect_file = root / baseline.expect_path

    if not xr_file.is_file():
        return [
            CheckResult(baseline.category, baseline.xr_path, False, "missing source fixture")
        ]
    if not expect_file.is_file():
        return [
            CheckResult(baseline.category, baseline.expect_path, False, "missing expect fixture")
        ]

    source = read_text(xr_file)
    expect = read_text(expect_file)
    missing_source = check_contains(source, baseline.source_contains)
    if missing_source:
        results.append(
            CheckResult(
                baseline.category,
                baseline.xr_path,
                False,
                "missing source anchors: " + ", ".join(missing_source),
            )
        )
    else:
        results.append(CheckResult(baseline.category, baseline.xr_path, True, "source anchors ok"))

    missing_expect = check_contains(expect, baseline.expect_contains)
    missing_regex = check_regex(expect, baseline.expect_regex)
    if missing_expect or missing_regex:
        detail_parts = []
        if missing_expect:
            detail_parts.append("missing expect anchors: " + ", ".join(missing_expect))
        if missing_regex:
            detail_parts.append("missing expect regex: " + ", ".join(missing_regex))
        results.append(
            CheckResult(baseline.category, baseline.expect_path, False, "; ".join(detail_parts))
        )
    else:
        results.append(
            CheckResult(baseline.category, baseline.expect_path, True, "expect anchors ok")
        )
    return results


def build_results(root: Path) -> list[CheckResult]:
    results: list[CheckResult] = []
    for baseline in BASELINES:
        results.extend(check_baseline(root, baseline))
    return results


def print_text(results: list[CheckResult]) -> None:
    print("Task 202 typed AOT baseline coverage")
    for result in results:
        status = "ok" if result.ok else "missing"
        print(f"{result.category}: {status}: {result.path}: {result.detail}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--json", action="store_true", help="print machine-readable JSON")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    results = build_results(root)

    if args.json:
        print(json.dumps([asdict(result) for result in results], indent=2, sort_keys=True))
    else:
        print_text(results)
    return 0 if all(result.ok for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
