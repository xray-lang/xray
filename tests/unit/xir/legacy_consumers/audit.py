#!/usr/bin/env python3
"""Inspect legacy consumer obligations without running or qualifying Xray."""
from __future__ import annotations

import argparse
import collections
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import subprocess

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


def load_module(name: str, relative: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def relative(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def file_record(path: Path) -> dict:
    if not path.is_file():
        return {"path": relative(path), "state": "MISSING"}
    raw = path.read_bytes()
    return {"path": relative(path), "state": "PRESENT", "bytes": len(raw),
            "sha256": sha(raw), "lf_sha256": sha(raw.replace(b"\r\n", b"\n"))}


def read_json(path: Path) -> dict:
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key: {key}")
            result[key] = value
        return result
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique)


def validate_responsibilities(data: dict) -> None:
    expected = {"capabilities": 27, "task308_obligations": 8,
                "task318_families": 14, "task322_requirements": 18}
    for key, count in expected.items():
        rows = data[key]
        ids = [r["id"] for r in rows]
        if len(rows) != count or len(set(ids)) != count:
            raise ValueError(f"missing or duplicate responsibility in {key}")
        for row in rows:
            if row["status"] != "OPEN" or not row["packages"]:
                raise ValueError(f"preparation cannot qualify {row['id']}")
    if {r["id"] for r in data["task322_requirements"]} != {f"R{i:02}" for i in range(1, 19)}:
        raise ValueError("root-effect requirement set changed")


def validate_routes(data: dict, legacy: dict, responsibilities: dict) -> dict:
    """Preserve the full original responsibility set while assigning producers."""
    families = {row["id"] for row in responsibilities["capabilities"]}
    packages = {"P02"} | {f"P{i:02}" for i in range(6, 19)}
    if data["status"] != "ROUTED_NOT_QUALIFIED":
        raise ValueError("routing cannot publish product qualification")
    originals = {row["name"]: row for row in legacy["source"]}
    rows = data["source_cases"]
    identities = [row["case_id"] for row in rows]
    if len(set(identities)) != len(rows) or set(identities) != set(originals):
        raise ValueError("source routing changed the original responsibility set")
    for row in rows:
        original = originals[row["case_id"]]
        if row["primary_family"] not in families or not row["producer_packages"] or not set(row["producer_packages"]) <= packages:
            raise ValueError("source routing has no known family or producer")
        if row["body_lf_sha256"] != original["body_lf_sha256"] or row["legacy_path"] != original["legacy_path"]:
            raise ValueError("source routing detached its original assertion body")
        if row["original_native_harness_expectation"] != original["original_fixture"]:
            raise ValueError("native harness expectation changed during routing")
        if row["existing_projection"] != original.get("initial_projection"):
            raise ValueError("source projection changed during routing")
        if row["replacement_qualification"] != "OPEN" or row["new_entry_status"] != "PENDING_FIXED_PRODUCT_DELIVERY":
            raise ValueError("routing cannot grant replacement qualification")
    paths = [row["path"] for row in data["other_files"]]
    originals_by_path = legacy["other_consumer_files"]
    if len(set(paths)) != len(paths) or set(paths) != set(originals_by_path):
        raise ValueError("other-file routing omitted a function or helper owner")
    for row in data["other_files"]:
        original = originals_by_path[row["path"]]
        functions = [{"name": f["name"], "body_lf_sha256": f["body_lf_sha256"]} for f in original["functions"]]
        if row["lf_sha256"] != original["lf_sha256"] or row["functions"] != functions:
            raise ValueError("other-file routing changed original function obligations")
        if (not row["producer_packages"] or not set(row["producer_packages"]) <= packages
                or row["replacement_qualification"] != "OPEN" or row["assertion_mapping"] != "OPEN"):
            raise ValueError("other-file routing is unowned or incorrectly qualified")
    return {"source_routes": len(rows), "other_file_routes": len(paths),
            "other_function_routes": sum(len(r["functions"]) for r in data["other_files"])}


def compare_required(expected: dict, observed: dict) -> None:
    """Compare one observation to its independent oracle, never to another backend."""
    for key, value in expected.items():
        if key not in observed or type(observed[key]) is not type(value) or observed[key] != value:
            raise ValueError(f"independent observation mismatch: {key}")


def validate_oracles(data: dict) -> dict:
    ids = [row["id"] for row in data["cases"]]
    if len(ids) != len(set(ids)) or not ids:
        raise ValueError("empty or duplicate oracle cases")
    legacy = load_module("legacy_consumer_literals", "scripts/source_product_consumer_inventory.py")
    source = legacy.SOURCE.read_text(encoding="utf-8")
    for row in data["cases"]:
        if row["status"] != "PREPARED_NOT_EXECUTED" or not row["rationale"] or not row["expected"]:
            raise ValueError("an oracle needs independent reasoning and must remain unqualified")
        for item in row["inputs"]:
            path = ROOT / item["path"]
            if not path.is_file() or file_record(path)["lf_sha256"] != item["lf_sha256"]:
                raise ValueError(f"oracle input changed: {item['path']}")
            if "original_literal" in item:
                start = source.index("TEST(" + row["legacy_case"] + ")")
                end = legacy.function_end(source, source.index("{", start))
                original = legacy.static_strings(source[start:end])[item["original_literal"]]
                if path.read_bytes().replace(b"\r\n", b"\n") != original:
                    raise ValueError(f"original positive program changed: {row['id']}")
        for backend in ("vm", "native"):
            if row["execution"][backend] != "NOT_RUN":
                raise ValueError("fixture preparation is not execution evidence")
        if not row["remaining_qualification"]:
            raise ValueError("preparation must retain the full qualification gap")
    return {"oracle_cases": len(ids), "product_execution": "NOT_RUN"}


def snapshot() -> dict:
    responsibilities = read_json(HERE / "responsibilities.json")
    validate_responsibilities(responsibilities)
    reg = load_module("legacy_regression_discovery", "scripts/run_regression_tests.py")
    diff = load_module("legacy_diff_discovery", "tests/diff/run_backend_diff.py")
    errors = load_module("legacy_error_discovery", "tests/compile_errors/run_compile_error_tests.py")
    ratchet = load_module("current_corpus_discovery", "scripts/check_regression_corpus.py")
    inventory = load_module("legacy_source_discovery", "scripts/source_product_consumer_inventory.py")
    legacy, generated = inventory.inventory()
    recorded = read_json(ROOT / "tests/unit/xir/product_consumers/legacy_responsibilities.json")
    if recorded != legacy:
        raise ValueError("legacy responsibility ledger drift; review before accepting a new denominator")
    for path, expected in generated.items():
        if path.read_bytes().replace(b"\r\n", b"\n") != expected:
            raise ValueError(f"legacy projection drift: {relative(path)}")
    anchors = collections.defaultdict(list)
    for family in responsibilities["capabilities"]:
        for path in family["original_anchors"]:
            anchors[path].append(family["id"])

    def case_base(path: Path, entry: str) -> dict:
        key = relative(path)
        families = anchors[key]
        return {"case_id": key, "entry": entry, "input": file_record(path),
                "primary_family": families[0] if len(families) == 1 else "UNMAPPED",
                "reviewed_anchor_families": families, "execution": "NOT_RUN"}

    regression = []
    for path in reg.collect_cases():
        row = case_base(path, "test" if reg.is_test_module(path) else "run")
        row.update(expected_exit=0, stdout_sidecar=file_record(Path(str(path) + ".expected")),
                   stdout_enforced_by_original_runner=False,
                   independent_oracle="SOURCE_ASSERTIONS_REQUIRE_SEMANTIC_REVIEW")
        regression.append(row)
    selected_reg = {r["case_id"] for r in regression}
    auxiliary = [file_record(p) for p in sorted(reg.TEST_DIR.rglob("*.xr")) if relative(p) not in selected_reg]
    differential = []
    for path in diff.collect_cases("", str(diff.SCRIPT_DIR / "coro_regression_cases.txt")):
        row = case_base(path, "run")
        expected = diff.read_expected_stdout(path)
        row.update(stdout_hex=None if expected is None else expected.hex(),
                   independent_stdout_oracle=expected is not None,
                   arguments=diff.read_args(path), stdin_hex=diff.read_stdin(path).hex(),
                   original_regression_entry=next((r["entry"] for r in regression if r["case_id"] == relative(path)), None))
        differential.append(row)
    negative = []
    for path in errors.collect_cases():
        row = case_base(path, "implicit_compile")
        expected_path, kind = errors.expected_path_for(path)
        row.update(expectation=file_record(expected_path), expectation_kind=kind)
        try:
            assertions = errors.parse_expected(expected_path)
            row.update(assertion_count=len(assertions),
                       diagnostic_position_gap=any(a.misplaced or a.where.degenerate for a in assertions),
                       expectation_parse="PARSED")
        except (errors.ExpectedFormatError, OSError) as exc:
            row.update(expectation_parse="FAILED", expectation_error=str(exc).replace(str(ROOT), "<repository>"))
        negative.append(row)
    routing = read_json(HERE / "consumer_routes.json")
    routing_counts = validate_routes(routing, legacy, responsibilities)
    source_routes = {row["case_id"]: row for row in routing["source_cases"]}
    sources = []
    for row in legacy["source"]:
        sources.append({"case_id": row["name"], "legacy_path": row["legacy_path"],
                        "body_lf_sha256": row["body_lf_sha256"], "original_fixture": row["original_fixture"],
                        "projection": row.get("initial_projection"),
                        "candidate_gates": row.get("candidate_replacement_gates", []),
                        "responsibility_detail": "tests/unit/xir/product_consumers/legacy_responsibilities.json#" + row["name"],
                        "execution": "NOT_RUN", "status": "OPEN",
                        "primary_family": source_routes[row["name"]]["primary_family"],
                        "producer_packages": source_routes[row["name"]]["producer_packages"],
                        "tasks": source_routes[row["name"]]["tasks"],
                        "new_entry_status": source_routes[row["name"]]["new_entry_status"]})
    extra = []
    for path in sorted((ROOT / "tests/unit/execution").glob("*.c")):
        extra.append({"input": file_record(path), "tasks": ["315", "316", "318", "322"],
                      "packages": ["P07", "P08", "P09"], "assertion_mapping": "OPEN",
                      "qualification_owner": "P20/P21; read-only for this preparation"})
    baseline = ratchet.read_baseline(ratchet.BASELINE)
    counts = {"registered_corpus_passing_test_baseline": len(baseline), "regression_entries": len(regression), "regression_test": sum(r["entry"] == "test" for r in regression),
              "regression_run": sum(r["entry"] == "run" for r in regression), "regression_auxiliary": len(auxiliary),
              "regression_stdout_sidecars": sum(r["stdout_sidecar"]["state"] == "PRESENT" for r in regression),
              "diff_entries": len(differential), "diff_independent_stdout": sum(r["independent_stdout_oracle"] for r in differential),
              "diff_regression_entry_mismatches": sum(r["original_regression_entry"] == "test" for r in differential),
              "compile_errors": len(negative), "expectation_parse_failures": sum(r["expectation_parse"] == "FAILED" for r in negative),
              "diagnostic_position_gaps": sum(bool(r.get("diagnostic_position_gap")) for r in negative),
              "source_responsibilities": len(sources), "source_fixtures": legacy["source_native_fixtures"],
              "source_projections": sum(r["projection"] is not None for r in sources),
              "other_consumer_files": len(legacy["other_consumer_files"]),
              "other_consumer_functions": sum(len(r["functions"]) for r in legacy["other_consumer_files"].values()),
              "execution_files": len(extra), "allocation_scenarios": legacy["source_allocation_scenarios"]["count"],
              "runner_rows_without_reviewed_primary_family": sum(r["primary_family"] == "UNMAPPED" for r in regression + differential + negative)}
    counts.update(routing_counts)
    return {"scope": "PREPARATION_ONLY_NO_PRODUCT_QUALIFICATION", "summary": counts,
            "source_identity": {"seed": "e16797c62636f1d138583ead18d234c86a3cde6e",
                                "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                                "working_tree": subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=all"], cwd=ROOT, text=True).splitlines(),
                                "p00_received": False},
            "regression": regression, "regression_auxiliary": auxiliary,
            "differential": differential, "compile_errors": negative,
            "source_responsibilities": sources, "other_consumers": legacy["other_consumer_files"],
            "source_allocation_scenarios": legacy["source_allocation_scenarios"],
            "execution_responsibilities": extra, "other_consumer_routes": routing["other_files"],
            "registered_corpus": {"gate": "regression_corpus", "runner": "scripts/check_regression_corpus.py",
                                  "entry": "xray test tests/regression --jobs <registered_jobs> --quiet --report <report>",
                                  "runner_default_timeout_seconds": 900, "ctest_timeout_expression": "600 * XR_TEST_TIMEOUT_SCALE",
                                  "registered_jobs": "min(4, XRAY_HOST_PROCESSOR_COUNT)", "runtime_discovery": "NOT_RUN",
                                  "passing_test_baseline": baseline, "status": "NOT_RUN"},
            "original_baselines": [file_record(ROOT / p) for p in ["tests/regression/baseline_failures.txt", "tests/regression/nondeterministic_cases.txt", "tests/regression/xir_passing.txt"]],
            "discovery_inputs": [file_record(ROOT / p) for p in ["CMakeLists.txt", "scripts/check_regression_corpus.py", "scripts/run_regression_tests.py", "tests/diff/run_backend_diff.py", "tests/diff/coro_regression_cases.txt", "tests/compile_errors/run_compile_error_tests.py", "tests/unit/xir/product_consumers/legacy_responsibilities.json"]],
            "original_anchor_presence": [file_record(ROOT / p) for p in sorted(anchors)],
            "preparation_inputs": [file_record(p) for p in sorted(HERE.rglob("*")) if p.is_file() and "__pycache__" not in p.parts],
            "note": "Runner entries overlap; counts are not a language completion percentage. Unmapped rows and unchanged old failures remain OPEN."}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("snapshot", "check"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    validate_responsibilities(read_json(HERE / "responsibilities.json"))
    result = validate_oracles(read_json(HERE / "oracles.json"))
    result.update(validate_routes(read_json(HERE / "consumer_routes.json"),
                                  read_json(ROOT / "tests/unit/xir/product_consumers/legacy_responsibilities.json"),
                                  read_json(HERE / "responsibilities.json")))
    if args.action == "snapshot":
        result = snapshot()
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(json.dumps(result["summary"], sort_keys=True))
    else:
        print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
