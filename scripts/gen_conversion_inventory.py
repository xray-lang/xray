#!/usr/bin/env python3
"""Generate a compiler-classified numeric conversion inventory.

Every included Xray source is analyzed by ``xray language conversions``.  The
generator never guesses conversion kinds from source spelling: regex is used
only to avoid launching the compiler for files that contain no numeric syntax.
Expected-negative compile fixtures remain visible as governed exclusions.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from functools import lru_cache
from pathlib import Path
from typing import Any


NUMERIC_SOURCE_RE = re.compile(
    r"\b(?:byte|u8|u16|u32|u64|usize|i8|i16|i32|i64|isize|int|f32|f64|float)\b"
    r"|\b(?:0[xob][0-9A-Fa-f_]+|[0-9][0-9_]*)\b"
)


# These are not ad-hoc compiler failures.  Each rule names a source family whose
# owning test contract deliberately requires a different entry mode, or whose
# README freezes it as historical input.  Keep the rules narrow and emit every
# match in the checked-in inventory so exclusions remain reviewable.
GOVERNED_EXCLUSION_PREFIXES: tuple[tuple[str, str], ...] = (
    ("tests/benchmarks/coro/", "historical_coroutine_benchmark_pending_runner_rewrite"),
    ("tests/benchmarks/game/", "legacy_benchmark_runner_input"),
    ("tests/benchmarks/ws/", "manual_external_websocket_benchmark"),
    ("tests/coroutine_safety/", "historical_manual_coroutine_suite"),
    ("tests/network/ws_tests/", "manual_external_websocket_suite"),
    ("tests/work_stealing/", "historical_manual_scheduler_suite"),
    ("tests/fixtures/manifest_export/", "requires_package_manifest_fixture"),
    ("tests/fixtures/native_output/", "requires_native_output_contract_fixture"),
)

GOVERNED_EXCLUSION_FILES: dict[str, str] = {
    "tests/network/ws_concurrent_test.xr": "manual_external_websocket_suite",
    "tests/network/ws_echo_server.xr": "manual_external_websocket_suite",
    "tests/network/ws_functional_test.xr": "manual_external_websocket_suite",
    "tests/network/ws_server_client_test.xr": "manual_external_websocket_suite",
    "tests/network/ws_simple_test.xr": "manual_external_websocket_suite",
    "tests/regression/13_types/1454_read_param_return_alias.xr": "expected_ownership_failure",
    "tests/test_harness/no_tests.xr": "expected_test_harness_failure",
}


def governed_exclusion_reason(rel: str) -> str | None:
    exact = GOVERNED_EXCLUSION_FILES.get(rel)
    if exact is not None:
        return exact
    for prefix, reason in GOVERNED_EXCLUSION_PREFIXES:
        if rel.startswith(prefix):
            return reason
    if rel.startswith("tests/stdlib/contracts/") and rel.endswith("/probes/legacy.xr"):
        return "cross_version_legacy_contract_probe"
    name = Path(rel).name
    if rel.startswith("tests/vm/") and (
        name.startswith("sys_process_") or name.startswith("sys_pipe_")
    ):
        return "requires_sys_test_profile_and_lifecycle_expectations"
    return None


@lru_cache(maxsize=None)
def canonical(path: Path) -> str:
    return os.path.normcase(str(path.resolve()))


@lru_cache(maxsize=None)
def display_path(path: Path, root: Path, ports_root: Path | None) -> str:
    resolved = path.resolve()
    for prefix, base in (("xray", root), ("xray-ports", ports_root)):
        if base is None:
            continue
        try:
            return f"{prefix}/{resolved.relative_to(base.resolve()).as_posix()}"
        except ValueError:
            pass
    return resolved.as_posix()


def source_candidates(root: Path) -> tuple[list[Path], list[dict[str, str]]]:
    included: list[Path] = []
    excluded: list[dict[str, str]] = []
    for base in (root / "bench", root / "demos", root / "stdlib", root / "tests"):
        for path in sorted(base.rglob("*.xr")):
            rel = path.relative_to(root).as_posix()
            governed_reason = governed_exclusion_reason(rel)
            if governed_reason is not None:
                excluded.append({"file": f"xray/{rel}", "reason": governed_reason})
                continue
            if rel.startswith("stdlib/types/"):
                excluded.append({"file": f"xray/{rel}", "reason": "native_type_declaration_stub"})
                continue
            if rel.startswith("tests/compile_errors/"):
                excluded.append({"file": f"xray/{rel}", "reason": "expected_compile_failure"})
                continue
            if rel.startswith("tests/aot/negative/"):
                excluded.append({"file": f"xray/{rel}", "reason": "expected_aot_negative"})
                continue
            if rel.startswith("tests/fuzz/corpus/"):
                excluded.append({"file": f"xray/{rel}", "reason": "parser_fuzz_corpus"})
                continue
            if rel.startswith("tests/aot/filetests/"):
                expectation = path.with_suffix(".expect")
                expectation_lines = (
                    expectation.read_text(encoding="utf-8").splitlines()
                    if expectation.is_file()
                    else []
                )
                if any(line.strip() == "status=fail" for line in expectation_lines):
                    excluded.append(
                        {"file": f"xray/{rel}", "reason": "expected_aot_compile_failure"}
                    )
                    continue
                if any(
                    line.startswith("args=") and "--profile freestanding" in line
                    for line in expectation_lines
                ):
                    excluded.append(
                        {"file": f"xray/{rel}", "reason": "requires_freestanding_profile"}
                    )
                    continue
            text = path.read_text(encoding="utf-8")
            if NUMERIC_SOURCE_RE.search(text):
                included.append(path)
            else:
                excluded.append({"file": f"xray/{rel}", "reason": "no_numeric_syntax"})
    return included, excluded


def port_entries(ports_root: Path) -> list[Path]:
    names = ("lmdb", "wasm3", "xv6-riscv", "xxhash", "yyjson")
    entries = [ports_root / "ports" / name / "src" / "main.xr" for name in names]
    missing = [path for path in entries if not path.is_file()]
    if missing:
        raise ValueError("missing governed port entries: " + ", ".join(map(str, missing)))
    return entries


def run_entry(xray: Path, entry: Path) -> tuple[Path, dict[str, Any] | None, str]:
    proc = subprocess.run(
        [str(xray), "language", "conversions", "--json", str(entry)],
        cwd=entry.parent,
        text=True,
        encoding="utf-8",
        errors="strict",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if proc.returncode != 0:
        return entry, None, proc.stderr.strip() or f"exit {proc.returncode}"
    try:
        payload = json.loads(proc.stdout)
    except json.JSONDecodeError as error:
        return entry, None, f"invalid JSON: {error}"
    if payload.get("schema_version") != 1 or payload.get("unresolved") != 0:
        return entry, None, "inventory schema mismatch or unresolved conversions"
    return entry, payload, ""


def record_key(record: dict[str, Any]) -> tuple[Any, ...]:
    return (
        record["file"],
        record["line"],
        record["column"],
        record["syntax"],
        record["source"],
        record["target"],
        record["kind"],
        record["implicit"],
        record["compile_time"],
    )


def generate(
    root: Path,
    ports_root: Path | None,
    xray: Path,
    jobs: int,
    failure_report: Path | None = None,
) -> dict[str, Any]:
    entries, excluded = source_candidates(root)
    if ports_root is not None:
        entries.extend(port_entries(ports_root))

    results: list[tuple[Path, dict[str, Any]]] = []
    failures: list[tuple[Path, str]] = []
    with ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        futures = {pool.submit(run_entry, xray, entry): entry for entry in entries}
        for future in as_completed(futures):
            entry, payload, error = future.result()
            if payload is None:
                failures.append((entry, error))
            else:
                results.append((entry, payload))
    if failures:
        ordered = sorted(failures)
        if failure_report is not None:
            failure_report.parent.mkdir(parents=True, exist_ok=True)
            failure_report.write_text(
                json.dumps(
                    [
                        {
                            "entry": display_path(entry, root, ports_root),
                            "error": error,
                        }
                        for entry, error in ordered
                    ],
                    indent=2,
                    ensure_ascii=False,
                )
                + "\n",
                encoding="utf-8",
            )
        shown = ordered[:40]
        details = "\n".join(f"  {entry}: {error}" for entry, error in shown)
        omitted = len(ordered) - len(shown)
        suffix = f"\n  ... {omitted} additional failures omitted" if omitted else ""
        raise RuntimeError(
            f"{len(failures)} conversion inventory entries failed:\n{details}{suffix}"
        )

    analyzed_files: dict[str, str] = {}
    records: dict[tuple[Any, ...], dict[str, Any]] = {}
    entry_rows: list[dict[str, Any]] = []
    for entry, payload in sorted(results, key=lambda item: canonical(item[0])):
        entry_rows.append({
            "entry": display_path(entry, root, ports_root),
            "file_count": payload["file_count"],
            "conversion_count": payload["count"],
        })
        for raw_file in payload["files"]:
            source = Path(raw_file)
            analyzed_files[canonical(source)] = display_path(source, root, ports_root)
        for raw in payload["conversions"]:
            record = dict(raw)
            source = Path(record["file"])
            record["file"] = display_path(source, root, ports_root)
            record.pop("node_id", None)
            records[record_key(record)] = record

    uncovered = [
        display_path(entry, root, ports_root)
        for entry in entries
        if canonical(entry) not in analyzed_files
    ]
    if uncovered:
        raise RuntimeError("entries missing from compiler-reported file coverage: " + ", ".join(uncovered))

    ordered_records = sorted(records.values(), key=record_key)
    kind_counts: dict[str, int] = {}
    for record in ordered_records:
        kind_counts[record["kind"]] = kind_counts.get(record["kind"], 0) + 1
    return {
        "schema_version": 1,
        "generator": "scripts/gen_conversion_inventory.py",
        "scope": "xray+five-ports" if ports_root is not None else "xray",
        "counts": {
            "entries": len(entry_rows),
            "analyzed_files": len(analyzed_files),
            "excluded_files": len(excluded),
            "conversions": len(ordered_records),
            "unresolved": 0,
        },
        "kinds": dict(sorted(kind_counts.items())),
        "entries": entry_rows,
        "analyzed_files": sorted(analyzed_files.values()),
        "excluded": sorted(excluded, key=lambda row: row["file"]),
        "conversions": ordered_records,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--ports-root", type=Path)
    parser.add_argument("--xray", type=Path, required=True)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--check", type=Path, help="compare generated inventory with this file")
    parser.add_argument(
        "--failure-report",
        type=Path,
        help="write all failed entry diagnostics as JSON before failing",
    )
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    args = parser.parse_args()
    root = args.root.resolve()
    xray = args.xray.resolve()
    ports_root = args.ports_root.resolve() if args.ports_root else None
    if not xray.is_file():
        parser.error(f"xray executable not found: {xray}")

    try:
        inventory = generate(root, ports_root, xray, args.jobs, args.failure_report)
    except (OSError, ValueError, RuntimeError) as error:
        print(f"conversion inventory: {error}", file=sys.stderr)
        return 1
    rendered = json.dumps(inventory, indent=2, ensure_ascii=False) + "\n"
    if args.check:
        if not args.check.is_file() or args.check.read_text(encoding="utf-8") != rendered:
            print(f"conversion inventory is stale: {args.check}", file=sys.stderr)
            return 1
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(rendered, encoding="utf-8")
    elif not args.check:
        sys.stdout.write(rendered)
    counts = inventory["counts"]
    print(
        "conversion inventory: PASS "
        f"({counts['entries']} entries, {counts['analyzed_files']} files, "
        f"{counts['conversions']} conversions, unresolved=0)",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
