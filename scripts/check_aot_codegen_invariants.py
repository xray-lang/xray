#!/usr/bin/env python3
"""AOT generated-C invariant scanner.

Reports the code-shape metrics that matter for the C90 AOT plan: XrValue
leakage, boxing/unboxing, tagged arithmetic helpers, typed-array runtime
switches, and VM/JIT header dependencies. With --expect it also compares them
against an expectation file; with --strict, an expectation failure exits
non-zero.

Metrics are counted in three scopes from one pattern table:

    (whole file)   what the translation unit as a whole looks like
    hot_*          only the functions matching the manifest's hot_function
    hot_region_*   only the marked region inside those functions

The narrower scopes are what the plan is actually about. A benchmark can carry
XrValue in its setup and still have a scalar hot loop, so a whole-file count
alone would either forbid the setup or excuse the loop.

Expectation file format:
  # comments are ignored
  hot_function=^module_run_[0-9]+$
  hot_region=typed_array_raw_access
  hot_region_start=^\\s*int64_t v[0-9]+ = \\(int64_t\\)_ad[0-9]+\\[phi[0-9]+\\];
  hot_region_end=^\\s*goto L[0-9]+;
  <metric>=<max>

Each expectation means metric <= value. Unknown keys are ignored so the same
manifest can also carry benchmark fields such as min_ratio.

Usage: check_aot_codegen_invariants.py [--expect FILE] [--strict] <generated.c>...
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

# grep's [[:space:]] is line-wise, so \s is the exact Python equivalent here.
SP = r"\s"

# name -> pattern, shared by every scope that has the metric. One table rather
# than three: the shell repeated all of these once per scope, which is how a
# pattern fix could land in one scope and not the others.
SHARED_PATTERNS: Dict[str, str] = {
    "xrvalue_count": r"\bXrValue\b",
    "xrvalue_local_count": r"(^|[({;=,\s])XrValue\s+[*]?[A-Za-z_][A-Za-z0-9_]*",
    "box_count": r"XR_FROM_[A-Z0-9_]*\s*\(|xr_box_[A-Za-z0-9_]*\s*\(|XI_BOX",
    "unbox_count": r"XR_TO_[A-Z0-9_]*\s*\(|xr_unbox_[A-Za-z0-9_]*\s*\(|XI_UNBOX",
    "runtime_arith_calls": r"xrt_(add|sub|mul|div|mod)\s*\(",
    "runtime_int_checked_arith_calls": r"xrt_int_(div|mod)\s*\(",
    "typed_array_runtime_calls": r"xr_typed_(get|set)\s*\(",
    "typed_array_bounds_check_count":
        r"if\s*\(_idx\s*<\s*0\)|_idx\s*>=\s*0\s*&&\s*_idx\s*<\s*_a->len",
    "typed_array_capacity_check_count":
        r"_a->len\s*>=\s*_a->cap|xrt_array_data_grow\s*\(_a",
    "typed_array_data_field_load_count": r"->data",
    "typed_array_direct_data_index_count": r"->data\)\[",
    "typed_array_per_iter_len_store_count": r"_a->len\s*=",
    "runtime_array_calls": r"xrt_array_[A-Za-z0-9_]*\s*\(",
    "runtime_map_calls": r"xrt_map_[A-Za-z0-9_]*\s*\(",
    "runtime_set_calls": r"xrt_set_[A-Za-z0-9_]*\s*\(",
    "runtime_property_calls": r"xrt_(getprop|setprop)\s*\(",
    "dynamic_dispatch_calls":
        r"xrt_(call_method|method|vcall|invoke|dispatch)[A-Za-z0-9_]*\s*\(",
    "pending_error_check_count": r"xrt_has_pending_error\s*\(",
    "restrict_local_count": r"\*\s*XRT_RESTRICT\s+_ad",
}

# Only meaningful for the whole translation unit.
BASE_ONLY_PATTERNS: Dict[str, str] = {
    "dtor_emitted": r"^static\s+void\s+[A-Za-z0-9_]+_dtor\s*\(void\s+\*obj\)",
    "release_runs_dtor": r"xrt_type_register\s*\([^;]*_dtor",
    "vm_jit_include_count": r'^#include\s+["<].*(src/)?(vm|jit|xvm|xm_)',
}

# Loop shape, only asked of the hot functions.
HOT_ONLY_PATTERNS: Dict[str, str] = {
    "int64_phi_count": r"^\s*int64_t\s+phi[0-9]+\s*=",
    "if_count": r"^\s*if\s*\(",
    "while_count": r"^\s*while\s*\(",
}

FUNCTION_OPEN = re.compile(r"^static\s.*\)\s*\{")

# Printed in exactly this order. restrict_local_count and its hot twin are
# computed and checkable but deliberately absent: they describe an
# implementation detail of the array fast path, not a plan metric.
OUTPUT_ORDER: Tuple[str, ...] = (
    "xrvalue_count", "xrvalue_local_count", "box_count", "unbox_count",
    "runtime_arith_calls", "runtime_int_checked_arith_calls",
    "typed_array_runtime_calls", "typed_array_bounds_check_count",
    "typed_array_capacity_check_count", "typed_array_data_field_load_count",
    "typed_array_direct_data_index_count",
    "typed_array_per_iter_len_store_count", "runtime_array_calls",
    "runtime_map_calls", "runtime_set_calls", "runtime_property_calls",
    "dynamic_dispatch_calls", "pending_error_check_count", "dtor_emitted",
    "release_runs_dtor", "vm_jit_include_count",
    "hot_function_count", "hot_xrvalue_count", "hot_xrvalue_local_count",
    "hot_box_count", "hot_unbox_count", "hot_runtime_arith_calls",
    "hot_runtime_int_checked_arith_calls", "hot_typed_array_runtime_calls",
    "hot_typed_array_bounds_check_count", "hot_typed_array_capacity_check_count",
    "hot_typed_array_data_field_load_count",
    "hot_typed_array_direct_data_index_count",
    "hot_typed_array_per_iter_len_store_count", "hot_int64_phi_count",
    "hot_if_count", "hot_while_count", "hot_runtime_array_calls",
    "hot_runtime_map_calls", "hot_runtime_set_calls",
    "hot_runtime_property_calls", "hot_dynamic_dispatch_calls",
    "hot_pending_error_check_count",
    "hot_region_count", "hot_region_xrvalue_count",
    "hot_region_xrvalue_local_count", "hot_region_box_count",
    "hot_region_unbox_count", "hot_region_runtime_arith_calls",
    "hot_region_runtime_int_checked_arith_calls",
    "hot_region_typed_array_runtime_calls",
    "hot_region_typed_array_bounds_check_count",
    "hot_region_typed_array_capacity_check_count",
    "hot_region_typed_array_data_field_load_count",
    "hot_region_typed_array_direct_data_index_count",
    "hot_region_typed_array_per_iter_len_store_count",
    "hot_region_runtime_array_calls", "hot_region_runtime_map_calls",
    "hot_region_runtime_set_calls", "hot_region_runtime_property_calls",
    "hot_region_dynamic_dispatch_calls",
    "hot_region_pending_error_check_count",
)

# The one named region shape the scanner knows how to bracket.
NAMED_REGIONS: Dict[str, Tuple[str, str]] = {
    "typed_array_raw_access": (r"XR_AOT_HOT_REGION_BEGIN\s+typed_array_raw_access",
                               r"XR_AOT_HOT_REGION_END\s+typed_array_raw_access"),
}


def manifest_value(expect_file: Optional[Path], key: str) -> str:
    """`key = value` from the expectation file, comments stripped."""
    if expect_file is None or not expect_file.is_file():
        return ""
    for line in expect_file.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith("#") or "=" not in line:
            continue
        name, _, value = line.partition("=")
        if name.strip() != key:
            continue
        return value.split("#")[0].strip()
    return ""


def count_lines(pattern: str, blocks: Sequence[Sequence[str]]) -> int:
    """Lines matching the pattern, as `grep -E ... | wc -l` counted them."""
    if not blocks:
        return 0
    compiled = re.compile(pattern)
    return sum(1 for block in blocks for line in block if compiled.search(line))


def function_name(line: str) -> str:
    """The identifier in a `static <type> name(...)` definition line."""
    name = re.sub(r"^static\s+", "", line)
    name = re.sub(r"\(.*", "", name)
    return re.sub(r".*[\s*]", "", name)


def extract_hot_functions(pattern: str, lines: Sequence[str]) -> Optional[List[str]]:
    """Whole bodies of the functions whose name matches, or None if none did.

    Brace depth rather than a closing-brace regex: a nested block or a struct
    literal in the body would end the capture early otherwise.
    """
    compiled = re.compile(pattern)
    captured: List[str] = []
    matched = 0
    depth = 0
    inside = False
    for line in lines:
        if not inside:
            if FUNCTION_OPEN.search(line) and compiled.search(function_name(line)):
                inside = True
                matched += 1
                depth = line.count("{") - line.count("}")
                captured.append(line)
                if depth <= 0:
                    inside = False
        else:
            captured.append(line)
            depth += line.count("{") - line.count("}")
            if depth <= 0:
                inside = False
    return captured if matched else None


def extract_hot_regions(start: str, end: str,
                        lines: Sequence[str]) -> Tuple[Optional[List[str]], bool]:
    """(captured lines or None, unterminated) for the marked region."""
    start_re, end_re = re.compile(start), re.compile(end)
    captured: List[str] = []
    matched = 0
    inside = False
    for line in lines:
        if not inside:
            if start_re.search(line):
                inside = True
                matched += 1
                captured.append(line)
                if end_re.search(line):
                    inside = False
        else:
            captured.append(line)
            if end_re.search(line):
                inside = False
    if inside:
        return captured, True
    return (captured if matched else None), False


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Scan generated C for AOT code-shape invariants.",
        add_help=True)
    parser.add_argument("--expect", type=Path, default=None,
                        help="expectation file")
    parser.add_argument("--strict", action="store_true",
                        help="exit non-zero when an expectation fails")
    parser.add_argument("files", nargs="+", type=Path, metavar="generated.c")
    args = parser.parse_args(argv[1:])

    for path in args.files:
        if not path.is_file():
            sys.stderr.write(f"Missing generated C file: {path}\n")
            return 2
    if args.expect is not None and not args.expect.is_file():
        sys.stderr.write(f"Missing expectation file: {args.expect}\n")
        return 2

    sources = [path.read_text(encoding="utf-8").splitlines()
               for path in args.files]

    hot_function = manifest_value(args.expect, "hot_function")
    region_name = manifest_value(args.expect, "hot_region")
    region_start = manifest_value(args.expect, "hot_region_start")
    region_end = manifest_value(args.expect, "hot_region_end")

    region_incomplete = False
    if region_name:
        known = NAMED_REGIONS.get(region_name)
        if known is None:
            region_incomplete = True
        else:
            region_start = region_start or known[0]
            region_end = region_end or known[1]

    hot_blocks: List[List[str]] = []
    if hot_function:
        for block in sources:
            found = extract_hot_functions(hot_function, block)
            if found is not None:
                hot_blocks.append(found)
    hot_function_missing = bool(hot_function) and not hot_blocks

    region_blocks: List[List[str]] = []
    region_missing = False
    if region_start or region_end:
        if not (region_start and region_end):
            region_incomplete = True
        else:
            # Regions are looked for inside the hot functions when there are
            # any, so a marker outside them cannot inflate the region metrics.
            inputs = hot_blocks if hot_blocks else sources
            for block in inputs:
                found, unterminated = extract_hot_regions(region_start, region_end,
                                                          block)
                if found is not None:
                    region_blocks.append(found)
                    if unterminated:
                        region_incomplete = True
            region_missing = not region_blocks

    metrics: Dict[str, int] = {}
    for name, pattern in SHARED_PATTERNS.items():
        metrics[name] = count_lines(pattern, sources)
        metrics[f"hot_{name}"] = count_lines(pattern, hot_blocks)
        if name != "restrict_local_count":
            metrics[f"hot_region_{name}"] = count_lines(pattern, region_blocks)
    for name, pattern in BASE_ONLY_PATTERNS.items():
        metrics[name] = count_lines(pattern, sources)
    for name, pattern in HOT_ONLY_PATTERNS.items():
        metrics[f"hot_{name}"] = count_lines(pattern, hot_blocks)

    metrics["hot_function_count"] = (
        count_lines(r"^static\s.*\)\s*\{", hot_blocks) if hot_blocks else 0)
    metrics["hot_region_count"] = (
        count_lines(region_start, region_blocks) if region_blocks else 0)

    audit_pass = 1
    expect_checked = 0

    if hot_function_missing:
        audit_pass = 0
        print(f"expectation_failure=hot_function actual=0 "
              f"expected_match={hot_function}")
    if region_incomplete:
        audit_pass = 0
        print("expectation_failure=hot_region actual=incomplete "
              "expected=start_and_end_match")
    if region_missing:
        audit_pass = 0
        print(f"expectation_failure=hot_region actual=0 "
              f"expected_match={region_start}")

    if args.expect is not None:
        for raw in args.expect.read_text(encoding="utf-8").splitlines():
            line = raw.split("#")[0].replace("\r", "").strip()
            if not line or "=" not in line:
                continue
            key, _, expected = line.partition("=")
            key = re.sub(r"\s", "", key)
            expected = re.sub(r"\s", "", expected)
            if key not in metrics:
                # Unknown keys are ignored so one manifest can also carry
                # benchmark fields such as min_ratio.
                continue
            expect_checked += 1
            try:
                limit = float(expected)
            except ValueError:
                continue
            if metrics[key] > limit:
                audit_pass = 0
                print(f"expectation_failure={key} actual={metrics[key]} "
                      f"expected_max={expected}")

    for name in OUTPUT_ORDER:
        print(f"{name}={metrics.get(name, 0)}")
    print(f"expect_checked={expect_checked}")
    print(f"audit_pass={audit_pass}")

    return 1 if (args.strict and audit_pass != 1) else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
