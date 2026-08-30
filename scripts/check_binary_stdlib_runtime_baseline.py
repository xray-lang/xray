#!/usr/bin/env python3
"""Check binary stdlib runtime, performance and size coverage."""

from __future__ import annotations

import argparse
import json
import re
import sys
import tomllib
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class CheckResult:
    category: str
    path: str
    ok: bool
    detail: str


REQUIRED_BENCHMARKS: dict[str, dict[str, str]] = {
    "base64.contract": {
        "module": "base64",
        "suite": "stdlib/base64",
        "source": "tests/diff/cases/semantics/stdlib/base64_module.xr",
    },
    "encoding.contract": {
        "module": "encoding",
        "suite": "stdlib/encoding",
        "source": "tests/diff/cases/semantics/stdlib/encoding_module.xr",
    },
    "compress.contract": {
        "module": "compress",
        "suite": "stdlib/compress",
        "source": "tests/diff/cases/semantics/stdlib/compress_roundtrip_direct.xr",
    },
    "crypto.contract": {
        "module": "crypto",
        "suite": "stdlib/crypto",
        "source": "tests/diff/cases/semantics/stdlib/crypto_timing_safe_equal_direct.xr",
    },
}

RUNNER_INVARIANTS: tuple[tuple[str, tuple[str, ...]], ...] = (
    (
        "validated_resolved_backends",
        (
            r'if compare not in \(\["vm"\], \["vm", "aot"\]\):',
            r'elif compare != contract_backends\.get\(module\):',
            r'backends = list\(entry\["compare"\]\)',
        ),
    ),
    (
        "aot_build_requires_selected_aot_backend",
        (r'if "aot" in backends:\n\s+build = subprocess\.run\(',),
    ),
    (
        "observable_equality_only_for_multiple_outputs",
        (r'if len\(outputs\) > 1 and len\(set\(outputs\.values\(\)\)\) != 1:',),
    ),
    (
        "selected_vm_output_hash",
        (r'"output_sha256": hashlib\.sha256\(outputs\["vm"\]\)\.hexdigest\(\)',),
    ),
    (
        "vm_only_byte_exact_output_oracle",
        (
            r'if compare == \["vm"\]:[\s\S]+?VM-only benchmark requires output_oracle',
            r'expected_oracle = f"\{source_value\}\.expected"',
            r'elif output_oracle != expected_oracle:',
            r'if backends == \["vm"\]:[\s\S]+?expected_output = oracle_path\.read_bytes\(\)',
            r'if outputs\["vm"\] != expected_output:',
            r'if backends == \["vm"\]:[\s\S]+?outputs\["vm"\] != expected_output:'
            r'[\s\S]+?"output_sha256": hashlib\.sha256\(outputs\["vm"\]\)\.hexdigest\(\)',
            r'if output_oracle is not None:\n\s+result\["output_oracle"\] = output_oracle',
        ),
    ),
    (
        "aot_size_requires_selected_aot_backend",
        (
            r'if "aot" in backends:\n\s+result\["aot_binary_size_bytes"\] = '
            r'binary\.stat\(\)\.st_size',
        ),
    ),
    (
        "vm_aot_ratio_requires_dual_mode",
        (
            r'if backends == \["vm", "aot"\]:\n\s+ratio = '
            r'medians\["vm"\] / max\(medians\["aot"\], 1\)',
            r'if backends == \["vm", "aot"\]:[\s\S]+?result\["vm_aot_ratio"\] = ratio',
        ),
    ),
)

BACKEND_DIFF_ANCHORS = (
    "Observable contract = stdout + exit code",
    "XRAY_DIFF_CASES_FILE",
    "XRAY_DIFF_EXTRA_CASES_FILE",
    "XRAY_DIFF_SINGLE_CASE",
)

AOT_FILETEST_ANCHORS = (
    'ALL_MODES = ["rep", "layout", "abi", "boundary", "container", "link", "cgen"]',
)

# The .expect directive language lives in its own module so it can be unit
# tested; the generated-C assertion keys are the surface this baseline pins.
AOT_FILETEST_DSL_ANCHORS = (
    '"c_contains"',
    '"c_not_contains"',
)

LINK_EXPECTS: dict[str, tuple[str, ...]] = {
    "tests/aot/filetests/link/core_base64.expect": (
        '"stdlib_symbols": ["base64.Base64Alphabet"',
        'not_contains="runtime_objects": ["xray_core"]',
        "c_not_contains=xrt_base64_",
    ),
    "tests/aot/filetests/link/core_encoding.expect": (
        '"stdlib_symbols": ["encoding.HexError"',
        'not_contains="runtime_objects": ["xray_core"]',
        "c_not_contains=xrt_encoding_",
    ),
    "tests/aot/filetests/link/core_compress.expect": (
        '"stdlib_symbols": ["compress.crc32"',
        'not_contains="runtime_objects": ["xray_core"]',
        "c_not_contains=xrt_compress_",
        "c_not_contains=xrt_method_",
    ),
    "tests/aot/filetests/link/core_crypto.expect": (
        '"stdlib_symbols": ["crypto.timingSafeEqual"',
        'not_contains="runtime_objects": ["xray_core"]',
        "c_contains=xrt_crypto_sha512(",
        "c_not_contains=xrt_method_",
    ),
    "tests/aot/filetests/link/system_crypto_random.expect": (
        '"stdlib_symbols": ["crypto.randomBytes", "crypto.uuid"]',
        'not_contains="runtime_objects": ["xray_core"]',
        "c_contains=xrt_crypto_random_bytes(",
        "c_not_contains=xrt_method_",
    ),
}


def load_toml(root: Path, path: Path) -> dict[str, Any]:
    with path.open("rb") as handle:
        return tomllib.load(handle)


def check_text_file(root: Path, category: str, path_text: str, anchors: tuple[str, ...]) -> CheckResult:
    path = root / path_text
    if not path.is_file():
        return CheckResult(category, path_text, False, "missing file")
    text = path.read_text(encoding="utf-8")
    missing = [anchor for anchor in anchors if anchor not in text]
    if missing:
        return CheckResult(category, path_text, False, "missing anchors: " + ", ".join(missing))
    return CheckResult(category, path_text, True, "anchors ok")


def check_runner_source(text: str) -> list[str]:
    """Return missing source-current execution invariants for the benchmark runner."""
    return [
        name
        for name, patterns in RUNNER_INVARIANTS
        if any(re.search(pattern, text) is None for pattern in patterns)
    ]


def check_runner(root: Path) -> CheckResult:
    path_text = "tests/benchmarks/stdlib/run.py"
    path = root / path_text
    if not path.is_file():
        return CheckResult("STDLIB_BENCH_OUTPUT_SIZE", path_text, False, "missing file")
    missing = check_runner_source(path.read_text(encoding="utf-8"))
    if missing:
        return CheckResult(
            "STDLIB_BENCH_OUTPUT_SIZE",
            path_text,
            False,
            "missing invariants: " + ", ".join(missing),
        )
    return CheckResult(
        "STDLIB_BENCH_OUTPUT_SIZE",
        path_text,
        True,
        "VM-only and dual-backend execution invariants are source-current",
    )


def check_benchmarks(root: Path) -> list[CheckResult]:
    manifest_path = root / "tests/benchmarks/stdlib/manifest.toml"
    if not manifest_path.is_file():
        return [CheckResult("STDLIB_BINARY_BENCHMARK", str(manifest_path), False, "missing manifest")]

    data = load_toml(root, manifest_path)
    by_id = {str(entry.get("id", "")): entry for entry in data.get("benchmark", ())}
    results: list[CheckResult] = []

    for bench_id, expected in REQUIRED_BENCHMARKS.items():
        entry = by_id.get(bench_id)
        if entry is None:
            results.append(
                CheckResult("STDLIB_BINARY_BENCHMARK", str(manifest_path), False, f"missing {bench_id}")
            )
            continue

        failures: list[str] = []
        for key, value in expected.items():
            if entry.get(key) != value:
                failures.append(f"{key}={entry.get(key)!r}, expected {value!r}")
        if entry.get("compare") != ["vm", "aot"]:
            failures.append("compare must be ['vm', 'aot']")
        if entry.get("metrics") != ["wall_ns"]:
            failures.append("metrics must be ['wall_ns']")
        for numeric_key in ("warmup", "iterations", "quick_iterations"):
            if not isinstance(entry.get(numeric_key), int) or int(entry[numeric_key]) < 1:
                failures.append(f"{numeric_key} must be a positive int")
        if not isinstance(entry.get("vm_budget_ratio"), (int, float)) or float(entry["vm_budget_ratio"]) <= 0:
            failures.append("vm_budget_ratio must be positive")
        source = root / expected["source"]
        if not source.is_file():
            failures.append(f"source missing: {expected['source']}")

        detail = "; ".join(failures) if failures else f"{bench_id} covers VM/AOT wall_ns"
        results.append(CheckResult("STDLIB_BINARY_BENCHMARK", str(manifest_path), not failures, detail))

    return results


def build_results(root: Path) -> list[CheckResult]:
    results: list[CheckResult] = []
    results.extend(check_benchmarks(root))
    results.append(check_runner(root))
    results.append(check_text_file(root, "VM_AOT_OUTPUT_DIFF", "tests/diff/run_backend_diff.py", BACKEND_DIFF_ANCHORS))
    results.append(check_text_file(root, "AOT_LINK_SIZE_BASELINE", "tests/aot/run_aot_filetests.py", AOT_FILETEST_ANCHORS))
    results.append(check_text_file(root, "AOT_LINK_SIZE_BASELINE", "tests/aot/filetest_expect.py", AOT_FILETEST_DSL_ANCHORS))
    for path, anchors in LINK_EXPECTS.items():
        results.append(check_text_file(root, "AOT_LINK_SIZE_BASELINE", path, anchors))
    return results


def print_text(results: list[CheckResult]) -> None:
    print("Task 200 binary stdlib runtime/perf/size baseline coverage")
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
