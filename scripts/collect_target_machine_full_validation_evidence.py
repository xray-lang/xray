#!/usr/bin/env python3
"""Collect identity-bound raw evidence from the governed full validation run."""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import platform as host_platform
import shutil
import signal
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "scripts") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts"))
import assemble_target_machine_completion_evidence as assembler  # noqa: E402
import check_target_machine_completion as completion  # noqa: E402


PRODUCER = "target-machine-full-validation-evidence/3"
KIND = "full-validation"
BASELINE_RUNNER = "tests/target-machine/phase0/run_baseline.py"
BASELINE_POLICY = "contracts/target-machine/baseline-manifest.json"
LANE_REGEX = {
    "unit": r"^(test_semantic_plan|test_target_plan|test_xtp_format|test_runtime_generation)$",
    "regression": r"backend_diff|regression",
    "compile-error": r"compile_error|compile-error",
    "aot": r"aot_(filetests|standalone_suite|manifest_sweep)|test_xaot",
    "generated-c": r"test_xi_cgen|generated_c|generated-c",
    "vm": r"vm_regression|test_xvm|backend_diff",
    "stdlib": r"stdlib",
    "ffi": r"ffi",
    # The coroutine suite lives in tests/unit/coro/ and no registered test
    # carries "coro" in its name, so a name substring selects nothing and the
    # lane can never be discovered.  Anchor the exact registered names.
    "coroutine": (r"coro|coroutine|^test_(async_pool|channel_close|mpsc_queue|"
                  r"native_backend|result_group|scheduler_runq|scope_wait|"
                  r"steal_queue|timer_wheel|work_queue_wait)$"),
    "concurrency": r"concurrency|thread|atomic",
    "contract": r"^(contract_freeze|meta_ownership_inventory|semantic_owner_inventory)$",
    "asan-ubsan": r"asan_focused|ubsan",
    "tsan": r"tsan_focused|tsan",
    "fuzz": r"fuzz",
    "portability": r"freestanding|c90|portability",
    "performance": r"performance|benchmark",
    "runtime-embedding": r"runtime_(target_plan_load|generation|typed_frame).*archive",
    "installer-package": r"install_public_artifact_smoke|installed_runtime_symbol_inventory|installer",
}
BASELINE_ONLY_LANE = "model-stress"


class CollectionError(ValueError):
    pass


def repository_identity(root: Path) -> dict[str, str]:
    identity, findings = completion.repository_identity(root)
    if findings:
        raise CollectionError("source identity is not collectable: " + "; ".join(
            row.message for row in findings
        ))
    return identity


def platform_identity(build: Path) -> dict[str, str]:
    cache = build / "CMakeCache.txt"
    compiler = "unknown-compiler"
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_C_COMPILER:") and "=" in line:
                compiler = line.split("=", 1)[1]
                break
    return {
        "os": host_platform.system() or "unknown-os",
        "arch": host_platform.machine() or "unknown-arch",
        "toolchain": f"target-machine-baseline/3; compiler={compiler}",
    }


def configured_build_identity(root: Path, build: Path) -> dict[str, str]:
    cache = build / "CMakeCache.txt"
    if not cache.is_file() or not (build / "build.ninja").is_file():
        raise CollectionError("full-validation evidence requires a configured Ninja build")
    required = {
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
        "CMAKE_GENERATOR": "Ninja",
        "XRAY_STDLIB_VM_FASTPATHS": "OFF",
    }
    values: dict[str, str] = {}
    for line in cache.read_text(encoding="utf-8", errors="strict").splitlines():
        if not line or line.startswith(("#", "//")) or ":" not in line or "=" not in line:
            continue
        name, rest = line.split(":", 1)
        if name not in {*required, "CMAKE_HOME_DIRECTORY"}:
            continue
        if name in values:
            raise CollectionError(f"configured build identity duplicates {name}")
        values[name] = rest.split("=", 1)[1].strip()
    home = values.get("CMAKE_HOME_DIRECTORY")
    if not home or Path(home).resolve() != root.resolve():
        raise CollectionError("configured build does not belong to the exact source root")
    for name, expected in required.items():
        if values.get(name) != expected:
            raise CollectionError(
                f"configured build {name} is {values.get(name)!r}, expected {expected!r}"
            )
    identity = {
        "build_root": "${BUILD_ROOT}",
        "source_root": "${SOURCE_ROOT}",
        "generator": values["CMAKE_GENERATOR"],
        "build_type": values["CMAKE_BUILD_TYPE"],
        "export_compile_commands": values["CMAKE_EXPORT_COMPILE_COMMANDS"],
        "stdlib_vm_fastpaths": values["XRAY_STDLIB_VM_FASTPATHS"],
    }
    identity["sha256"] = hashlib.sha256(json.dumps(
        identity, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")).hexdigest()
    return identity


def canonical_command(root: Path, build: Path, output: Path,
                      owner: str, lane_timeout: int) -> list[str]:
    return [
        sys.executable,
        str((root / "scripts/collect_target_machine_full_validation_evidence.py").resolve()),
        "--root", str(root), "--build", str(build),
        "--output-dir", str(output), "--owner", owner,
        "--lane-timeout", str(lane_timeout),
    ]


def run(command: list[str], root: Path, timeout: int) -> tuple[int, str]:
    process: subprocess.Popen[str] | None = None
    try:
        creationflags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        process = subprocess.Popen(
            command, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace",
            creationflags=creationflags, start_new_session=os.name != "nt",
        )
        stdout, stderr = process.communicate(timeout=timeout)
        return process.returncode, stdout + stderr
    except subprocess.TimeoutExpired:
        if process is not None:
            if os.name == "nt":
                subprocess.run(
                    ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                    check=False, capture_output=True,
                )
            else:
                os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate()
        else:
            stdout, stderr = "", ""
        return 124, stdout + stderr + "\nfull validation timed out\n"
    except OSError as error:
        return 127, f"full validation could not start: {error}\n"


def parse_ctest_discovery(output: str) -> tuple[str, ...]:
    try:
        document = json.loads(output)
    except json.JSONDecodeError as error:
        raise CollectionError(f"CTest discovery is not JSON: {error}") from error
    tests = document.get("tests") if isinstance(document, dict) else None
    if not isinstance(tests, list):
        raise CollectionError("CTest discovery has no tests array")
    names: list[str] = []
    for index, row in enumerate(tests):
        name = row.get("name") if isinstance(row, dict) else None
        if not isinstance(name, str) or not name:
            raise CollectionError(f"CTest discovery test {index} has no exact name")
        names.append(name)
    if len(names) != len(set(names)):
        raise CollectionError("CTest discovery contains duplicate test names")
    return tuple(sorted(names))


def selection_sha256(names: tuple[str, ...]) -> str:
    payload = json.dumps(list(names), ensure_ascii=False, separators=(",", ":"))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def discover_lanes(root: Path, build: Path,
                   required: list[str]) -> tuple[dict[str, tuple[str, ...]], str, bool]:
    selections: dict[str, tuple[str, ...]] = {}
    logs: list[str] = []
    all_ok = True
    for name in required:
        if name == BASELINE_ONLY_LANE:
            continue
        regex = LANE_REGEX[name]
        command = [
            "ctest", "--test-dir", str(build), "-C", "Release", "-R", regex,
            "-N", "--show-only=json-v1",
        ]
        code, output = run(command, root, 120)
        logs.append(
            f"lane={name}\nargv={json.dumps(command, ensure_ascii=False)}\n"
            f"exit_code={code}\n{output}\n"
        )
        if code != 0:
            all_ok = False
            selections[name] = ()
            continue
        try:
            names = parse_ctest_discovery(output)
        except CollectionError as error:
            logs.append(f"discovery_error={error}\n")
            all_ok = False
            names = ()
        if not names:
            all_ok = False
        selections[name] = names
    return selections, "\n".join(logs), all_ok


def load_baseline(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8", errors="strict"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise CollectionError(f"cannot read baseline evidence: {error}") from error
    if not isinstance(document, dict):
        raise CollectionError("baseline evidence is not an object")
    return document


def baseline_lane_statuses(document: dict[str, Any]) -> dict[str, bool]:
    correctness = document.get("correctness_lanes")
    performance = document.get("performance_lanes")
    if not isinstance(correctness, list) or not isinstance(performance, list):
        raise CollectionError("baseline evidence lacks lane collections")
    correctness_by_name = {
        row.get("name"): row for row in correctness
        if isinstance(row, dict) and isinstance(row.get("name"), str)
    }
    performance_rows = [row for row in performance if isinstance(row, dict)]
    repeated = [
        row for row in correctness_by_name.values()
        if row.get("repeat_policy") == "three-or-more"
    ]
    performance_ok = bool(performance_rows) and all(
        row.get("status") == "passed"
        and isinstance(row.get("variance"), dict)
        and row["variance"].get("status") == "passed"
        for row in performance_rows
    )
    suite_ok = correctness_by_name.get("non-sanitizer-suite", {}).get("status") == "passed"
    statuses = {name: suite_ok for name in LANE_REGEX}
    statuses["asan-ubsan"] = correctness_by_name.get("asan-ubsan", {}).get("status") == "passed"
    statuses["tsan"] = correctness_by_name.get("tsan", {}).get("status") == "passed"
    statuses["performance"] = performance_ok
    statuses[BASELINE_ONLY_LANE] = (
        document.get("result") == "passed"
        and bool(repeated)
        and all(row.get("status") == "passed" for row in repeated)
        and performance_ok
    )
    return statuses


def baseline_selection(document: dict[str, Any]) -> tuple[str, ...]:
    correctness = document.get("correctness_lanes", [])
    performance = document.get("performance_lanes", [])
    names = [
        f"correctness:{row['name']}"
        for row in correctness if isinstance(row, dict) and isinstance(row.get("name"), str)
    ] + [
        f"performance:{row['name']}"
        for row in performance if isinstance(row, dict) and isinstance(row.get("name"), str)
    ]
    if len(names) != len(set(names)) or not names:
        raise CollectionError("baseline evidence lane names are missing or duplicate")
    return tuple(sorted(names))


def collect(root: Path, build: Path, output: Path, owner: str,
            lane_timeout: int) -> int:
    governance = assembler.read_object(root / completion.DEFAULT_MANIFEST)
    if completion.validate_manifest(governance):
        raise CollectionError("completion governance manifest is invalid")
    required = governance["validation"]["required_lanes"]
    expected = set(LANE_REGEX) | {BASELINE_ONLY_LANE}
    if set(required) != expected or len(required) != len(expected):
        raise CollectionError("full-validation lane authority is not exact")
    if not (build / "CTestTestfile.cmake").is_file():
        raise CollectionError("full-validation evidence requires a configured CTest build")
    build_identity = configured_build_identity(root, build)
    if output.exists() or output.is_symlink():
        raise CollectionError("raw evidence package already exists; collection never overwrites")
    identity = repository_identity(root)
    governance_hash = completion.governance_input_sha256(root, governance)
    output.parent.mkdir(parents=True, exist_ok=True)
    lock = output.with_name(f".{output.name}.collect-lock")
    try:
        lock.mkdir()
    except FileExistsError as error:
        raise CollectionError("another full-validation collection owns the output") from error
    staging: Path | None = None
    try:
        staging = Path(tempfile.mkdtemp(
            prefix=f".{output.name}-staging-", dir=output.parent
        ))
        logs = staging / "logs"
        logs.mkdir()
        selections, discovery_output, discovery_ok = discover_lanes(root, build, required)
        discovery_log = logs / "ctest-discovery.log"
        discovery_log.write_text(discovery_output, encoding="utf-8")

        baseline_root = logs / "baseline"
        baseline_command = [
            sys.executable, str((root / BASELINE_RUNNER).resolve()),
            "--root", str(root), "--build-dir", str(build),
            "--output-dir", str(baseline_root), "--policy", BASELINE_POLICY,
            "--scope", "full",
        ]
        baseline_code, baseline_output = run(baseline_command, root, lane_timeout)
        baseline_stdout = logs / "baseline-run.log"
        baseline_stdout.write_text(
            f"argv={json.dumps(baseline_command, ensure_ascii=False)}\n"
            f"exit_code={baseline_code}\n{baseline_output}",
            encoding="utf-8",
        )
        baseline_manifest = baseline_root / "baseline-evidence.json"
        try:
            baseline = load_baseline(baseline_manifest)
            statuses = baseline_lane_statuses(baseline)
            model_selection = baseline_selection(baseline)
            baseline_ok = baseline_code == 0 and baseline.get("result") == "passed"
        except CollectionError as error:
            baseline = {}
            statuses = {name: False for name in required}
            model_selection = ()
            baseline_ok = False
            with baseline_stdout.open("a", encoding="utf-8") as stream:
                stream.write(f"\nbaseline_evidence_error={error}\n")

        platform = platform_identity(build)
        lanes: list[dict[str, Any]] = []
        for name in required:
            selected = model_selection if name == BASELINE_ONLY_LANE else selections[name]
            passed = baseline_ok and discovery_ok and bool(selected) and statuses.get(name, False)
            lanes.append({
                "name": name,
                "status": "passed" if passed else "failed",
                "command": baseline_command,
                "platform": platform,
                "log": "logs/baseline-run.log",
                "selected_tests": list(selected),
                "selection_sha256": selection_sha256(selected),
                "execution": "governed-full-baseline",
            })

        all_ok = (
            baseline_ok and discovery_ok
            and all(row["status"] == "passed" for row in lanes)
        )
        status = "passed" if all_ok else "failed"
        exit_code = 0 if all_ok else 1
        generated_at = datetime.datetime.now(datetime.timezone.utc).isoformat().replace(
            "+00:00", "Z"
        )
        collector_command = canonical_command(root, build, output, owner, lane_timeout)
        raw_log_sources = [discovery_log, baseline_stdout]
        if baseline_root.is_dir():
            raw_log_sources.extend(sorted(path for path in baseline_root.rglob("*") if path.is_file()))
        raw_logs: list[dict[str, Any]] = []
        seen: set[str] = set()
        for log in raw_log_sources:
            relative = log.relative_to(staging).as_posix()
            if relative in seen:
                continue
            seen.add(relative)
            digest = assembler.sha256_file(log)
            raw_logs.append({
                "path": relative, "sha256": digest,
                "identity_sha256": assembler.raw_log_identity(
                    KIND, identity["source_commit"], identity["repository_sha256"],
                    governance_hash, relative, digest, owner, generated_at,
                    collector_command, platform, exit_code, status,
                ),
                "result": "passed" if all_ok else "failed",
            })
        raw = {
            "schema": assembler.RAW_SCHEMA, "kind": KIND,
            "status": status, "exit_code": exit_code,
            "source_commit": identity["source_commit"],
            "repository_sha256": identity["repository_sha256"],
            "governance_input_sha256": governance_hash,
            "owner": owner, "generated_at": generated_at,
            "command": collector_command, "platform": platform,
            "payload": {
                "producer": PRODUCER,
                "baseline_runner": "target-machine-baseline/3",
                "build_identity": build_identity,
                "baseline_manifest": "logs/baseline/baseline-evidence.json",
                "lanes": lanes,
            },
            "logs": raw_logs,
        }
        assembler.write_object(staging / "full-validation.raw.json", raw)
        os.rename(staging, output)
        staging = None
    finally:
        if staging is not None:
            shutil.rmtree(staging, ignore_errors=True)
        try:
            lock.rmdir()
        except OSError:
            pass
    return exit_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    parser.add_argument("--build", default="build")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--owner", required=True)
    parser.add_argument("--lane-timeout", type=int, default=14400)
    args = parser.parse_args()
    root = Path(args.root).resolve()
    build = Path(args.build)
    if not build.is_absolute():
        build = root / build
    try:
        code = collect(root, build.resolve(), Path(args.output_dir).resolve(),
                       args.owner, args.lane_timeout)
    except (CollectionError, OSError, KeyError, TypeError, ValueError) as error:
        print(f"target-machine full validation evidence: ERROR: {error}", file=sys.stderr)
        return 2
    print(f"target-machine full validation evidence: {'PASS' if code == 0 else 'FAIL'}: "
          f"{Path(args.output_dir).resolve()}")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
