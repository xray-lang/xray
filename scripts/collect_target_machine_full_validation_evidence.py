#!/usr/bin/env python3
"""Collect identity-bound raw evidence for every mandatory validation lane."""

from __future__ import annotations

import argparse
import datetime
import json
import os
import platform as host_platform
import re
import signal
import shutil
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


PRODUCER = "target-machine-full-validation-evidence/1"
KIND = "full-validation"
LANE_REGEX = {
    "unit": r"^(test_semantic_plan|test_target_plan|test_xtp_format|test_runtime_generation)$",
    "regression": r"backend_diff|regression",
    "compile-error": r"compile_error|compile-error",
    "aot": r"aot_(filetests|standalone_suite|manifest_sweep)|test_xaot",
    "generated-c": r"test_xi_cgen|generated_c|generated-c",
    "vm": r"vm_regression|test_xvm|backend_diff",
    "stdlib": r"stdlib",
    "ffi": r"ffi",
    "coroutine": r"coro|coroutine",
    "concurrency": r"concurrency|thread|atomic",
    "contract": r"^(contract_freeze|meta_ownership_inventory|semantic_owner_inventory)$",
    "asan-ubsan": r"asan_focused|ubsan",
    "tsan": r"tsan_focused|tsan",
    "fuzz": r"fuzz",
    "model-stress": r"model_stress|model-stress|stress_model",
    "portability": r"freestanding|c90|portability",
    "performance": r"performance|benchmark",
    "runtime-embedding": r"runtime_(target_plan_load|generation|typed_frame).*archive",
    "installer-package": r"install_public_artifact_smoke|installed_runtime_symbol_inventory|installer",
}
TOTAL_RE = re.compile(r"Total Tests:\s*(\d+)")


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
        "toolchain": f"ctest-full-validation; compiler={compiler}",
    }


def canonical_command(root: Path, build: Path, output: Path,
                      owner: str) -> list[str]:
    return [
        sys.executable,
        str((root / "scripts/collect_target_machine_full_validation_evidence.py").resolve()),
        "--root", str(root), "--build", str(build),
        "--output-dir", str(output), "--owner", owner,
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
        return 124, stdout + stderr + "\nvalidation lane timed out\n"
    except OSError as error:
        return 127, f"validation lane could not start: {error}\n"


def discovered_test_count(output: str) -> int:
    match = TOTAL_RE.search(output)
    return int(match.group(1)) if match else 0


def lane_result(preflight_code: int, preflight_output: str,
                run_code: int) -> tuple[str, int]:
    count = discovered_test_count(preflight_output)
    passed = preflight_code == 0 and count > 0 and run_code == 0
    return ("passed" if passed else "failed", count)


def collect(root: Path, build: Path, output: Path, owner: str,
            lane_timeout: int) -> int:
    governance = assembler.read_object(root / completion.DEFAULT_MANIFEST)
    if completion.validate_manifest(governance):
        raise CollectionError("completion governance manifest is invalid")
    required = governance["validation"]["required_lanes"]
    if set(required) != set(LANE_REGEX) or len(required) != len(LANE_REGEX):
        raise CollectionError("full-validation lane authority is not exact")
    if not (build / "CTestTestfile.cmake").is_file():
        raise CollectionError("full-validation evidence requires a configured CTest build")
    identity = repository_identity(root)
    governance_hash = governance["input_identity"]["sha256"]
    actual_governance = completion.framed_tree_hash(
        root, governance["input_identity"]["files"]
    )
    if output.exists() or output.is_symlink():
        raise CollectionError("raw evidence package already exists; collection never overwrites")
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
        lanes: list[dict[str, Any]] = []
        raw_log_sources: list[tuple[str, Path, bool]] = []
        all_ok = actual_governance == governance_hash
        for name in required:
            regex = LANE_REGEX[name]
            base = ["ctest", "--test-dir", str(build), "-C", "Release", "-R", regex]
            preflight = base + ["-N"]
            preflight_code, preflight_output = run(preflight, root, 120)
            count = discovered_test_count(preflight_output)
            command = base + ["--output-on-failure"]
            if preflight_code == 0 and count > 0:
                run_code, run_output = run(command, root, lane_timeout)
            else:
                run_code, run_output = 125, "lane has no discovered tests; execution refused\n"
            status, count = lane_result(preflight_code, preflight_output, run_code)
            ok = status == "passed"
            all_ok = all_ok and ok
            relative = f"logs/{name}.log"
            log = staging / relative
            log.write_text(
                f"preflight_argv={json.dumps(preflight, ensure_ascii=False)}\n"
                f"preflight_exit={preflight_code}\ndiscovered_tests={count}\n"
                f"argv={json.dumps(command, ensure_ascii=False)}\nexit_code={run_code}\n"
                f"--- preflight ---\n{preflight_output}\n--- execution ---\n{run_output}",
                encoding="utf-8",
            )
            lanes.append({
                "name": name, "status": status, "command": command,
                "platform": platform_identity(build), "log": relative,
                "discovered_tests": count, "exit_code": run_code,
            })
            raw_log_sources.append((relative, log, ok))
        status = "passed" if all_ok else "failed"
        exit_code = 0 if all_ok else 1
        generated_at = datetime.datetime.now(datetime.timezone.utc).isoformat().replace(
            "+00:00", "Z"
        )
        collector_command = canonical_command(root, build, output, owner)
        platform = platform_identity(build)
        raw_logs: list[dict[str, Any]] = []
        for relative, log, ok in raw_log_sources:
            digest = assembler.sha256_file(log)
            raw_logs.append({
                "path": relative, "sha256": digest,
                "identity_sha256": assembler.raw_log_identity(
                    KIND, identity["source_commit"], identity["repository_sha256"],
                    governance_hash, relative, digest, owner, generated_at,
                    collector_command, platform, exit_code, status,
                ),
                "result": "passed" if ok else "failed",
            })
        raw = {
            "schema": assembler.RAW_SCHEMA, "kind": KIND,
            "status": status, "exit_code": exit_code,
            "source_commit": identity["source_commit"],
            "repository_sha256": identity["repository_sha256"],
            "governance_input_sha256": governance_hash,
            "owner": owner, "generated_at": generated_at,
            "command": collector_command, "platform": platform,
            "payload": {"producer": PRODUCER, "lanes": lanes},
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
    parser.add_argument("--lane-timeout", type=int, default=1800)
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
