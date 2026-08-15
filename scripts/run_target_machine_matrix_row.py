#!/usr/bin/env python3
"""Produce one independently verified target-machine matrix row result."""

from __future__ import annotations

import argparse
import datetime
import json
import os
import platform as host_platform
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

import assemble_target_machine_completion_evidence as assembler
import check_target_machine_completion as completion
import collect_target_machine_matrix_evidence as matrix


PRODUCER = "target-machine-matrix-row-result/1"
QUALIFYING_TIERS = frozenset({"supported", "ci-only"})


class RowError(ValueError):
    pass


def read_cache(build: Path) -> dict[str, str]:
    path = build / "CMakeCache.txt"
    if not path.is_file():
        raise RowError("matrix build has no CMakeCache.txt")
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="strict").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key_type, value = line.split("=", 1)
        key = key_type.split(":", 1)[0]
        if key:
            values[key] = value
    return values


def cmake_compiler_identity(build: Path) -> tuple[str, str]:
    candidates = sorted((build / "CMakeFiles").glob("*/CMakeCCompiler.cmake"))
    if len(candidates) != 1:
        raise RowError("matrix build has no unique C compiler identity")
    text = candidates[0].read_text(encoding="utf-8", errors="strict")

    def value(name: str) -> str:
        match = re.search(
            rf'^set\({re.escape(name)}\s+"([^"]*)"\)$', text, re.MULTILINE
        )
        return match.group(1) if match else ""

    compiler_id = value("CMAKE_C_COMPILER_ID")
    compiler_version = value("CMAKE_C_COMPILER_VERSION")
    if not compiler_id or not compiler_version:
        raise RowError("matrix build compiler identity is incomplete")
    return compiler_id, compiler_version


def provider_identity(target: str, compiler_id: str) -> str:
    if compiler_id == "MSVC":
        return "msvc"
    if compiler_id == "GNU":
        return "gcc"
    if compiler_id == "AppleClang":
        return "host-clang"
    if compiler_id == "Clang":
        return "host-clang" if target == "macos-arm64" else "clang"
    return ""


def require_build_contract(root: Path, build: Path,
                           row: dict[str, Any]) -> tuple[str, str]:
    if build.resolve() != (root / "build").resolve():
        raise RowError("matrix row command is bound to the governed build directory")
    cache = read_cache(build)
    if cache.get("CMAKE_GENERATOR") != "Ninja":
        raise RowError("matrix build generator is not Ninja")
    if cache.get("XRAY_STDLIB_VM_FASTPATHS") != "OFF":
        raise RowError("matrix build must set XRAY_STDLIB_VM_FASTPATHS=OFF")
    profile = row["build_or_sanitizer"]
    if profile == "Release":
        if cache.get("CMAKE_BUILD_TYPE") != "Release":
            raise RowError("matrix Release row does not use a Release build")
        if any(cache.get(flag) == "ON" for flag in
               ("ENABLE_ASAN", "ENABLE_UBSAN", "ENABLE_TSAN")):
            raise RowError("matrix Release row unexpectedly enables a sanitizer")
    elif profile == "ASan-UBSan":
        if cache.get("ENABLE_ASAN") != "ON" or cache.get("ENABLE_UBSAN") != "ON":
            raise RowError("matrix ASan-UBSan row lacks both sanitizers")
        if cache.get("ENABLE_TSAN") == "ON":
            raise RowError("matrix ASan-UBSan row also enables TSan")
    elif profile == "TSan":
        if cache.get("ENABLE_TSAN") != "ON":
            raise RowError("matrix TSan row lacks TSan")
        if any(cache.get(flag) == "ON" for flag in ("ENABLE_ASAN", "ENABLE_UBSAN")):
            raise RowError("matrix TSan row mixes incompatible sanitizers")
    else:
        raise RowError(f"matrix row build profile is not executable: {profile}")
    compiler_id, compiler_version = cmake_compiler_identity(build)
    provider = provider_identity(row["target"], compiler_id)
    if not provider or provider != row["provider"]:
        raise RowError(
            f"matrix provider mismatch: policy={row['provider']} build={provider or compiler_id}"
        )
    return compiler_id, compiler_version


def executable(build: Path, relative: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    path = build / f"{relative}{suffix}"
    if not path.is_file():
        raise RowError(f"matrix executable is missing: {path}")
    return path.resolve()


def run_command(argv: list[str], cwd: Path,
                timeout_seconds: int) -> tuple[int, str]:
    try:
        result = subprocess.run(
            argv, cwd=cwd, check=False, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=timeout_seconds,
        )
        return result.returncode, result.stdout + result.stderr
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout or ""
        stderr = error.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode("utf-8", errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", errors="replace")
        return 124, stdout + stderr + "\nmatrix row timed out\n"
    except OSError as error:
        return 127, f"cannot execute matrix row command: {error}\n"


def exact_version(binary: Path, root: Path,
                  identity: dict[str, str]) -> dict[str, Any]:
    code, text = run_command([str(binary), "--version", "--json"], root, 30)
    if code != 0:
        raise RowError("matrix xray binary identity command failed")
    try:
        version = json.loads(text)
    except json.JSONDecodeError as error:
        raise RowError("matrix xray binary identity is not JSON") from error
    if (version.get("commit") != identity["source_commit"]
            or version.get("dirty") is not False):
        raise RowError("matrix xray binary does not match the clean source identity")
    return version


def mutate_artifact(source: Path, destination: Path) -> None:
    payload = bytearray(source.read_bytes())
    if len(payload) < 64:
        raise RowError("matrix XTP artifact is too small to mutate safely")
    payload[-1] ^= 0x5A
    destination.write_bytes(payload)


def verify_activation(binary: Path, writer: Path, work: Path,
                      timeout_seconds: int) -> tuple[Path, Path]:
    xsm = work / "matrix-runtime.xsm"
    xtp = work / "matrix-runtime.xtp"
    write_argv = [str(writer), "--write-runtime-artifacts", str(xsm), str(xtp)]
    write_code, write_text = run_command(write_argv, work, timeout_seconds)
    if write_code != 0 or not xsm.is_file() or not xtp.is_file():
        raise RowError(f"matrix runtime artifact writer failed\n{write_text}")
    positive_argv = [str(binary), "run", str(xtp), "--semantic-plan", str(xsm),
                     "--timings"]
    positive_code, positive_text = run_command(
        positive_argv, work, timeout_seconds
    )
    if (positive_code != 0 or re.search(r"(?m)^42$", positive_text) is None
            or "semantic_verify_ns=" not in positive_text
            or "target_verify_ns=" not in positive_text
            or "activation_ns=" not in positive_text):
        raise RowError(f"matrix positive activation failed\n{positive_text}")
    malformed = work / "matrix-runtime-mismatch.xtp"
    mutate_artifact(xtp, malformed)
    negative_argv = [str(binary), "run", str(malformed),
                     "--semantic-plan", str(xsm), "--timings"]
    negative_code, negative_text = run_command(
        negative_argv, work, timeout_seconds
    )
    if (negative_code == 0 or "activation_ns=" in negative_text
            or re.search(r"(?m)^42$", negative_text) is not None):
        raise RowError("matrix target mismatch was not rejected before activation")
    return xsm, xtp


def row_by_id(policy: dict[str, Any], row_id: str) -> dict[str, Any]:
    rows = policy.get("rows")
    matches = [row for row in rows if isinstance(row, dict)
               and row.get("id") == row_id] if isinstance(rows, list) else []
    if len(matches) != 1:
        raise RowError("matrix row identity is missing or ambiguous")
    row = matches[0]
    if row.get("support_tier") not in QUALIFYING_TIERS:
        raise RowError("matrix row is not in a qualifying support tier")
    return row


def produce(root: Path, build: Path, row_id: str,
            output: Path, owner: str) -> None:
    governance = assembler.read_object(root / completion.DEFAULT_MANIFEST)
    if completion.validate_manifest(governance):
        raise RowError("completion governance manifest is invalid")
    identity = matrix.repository_identity(root)
    governance_hash = governance["input_identity"]["sha256"]
    if completion.framed_tree_hash(
            root, governance["input_identity"]["files"]) != governance_hash:
        raise RowError("completion governance input identity is stale")
    policy_path = root / governance["matrix"]["policy"]
    policy = assembler.read_object(policy_path)
    rows = matrix.validate_policy(policy, governance)
    row = row_by_id({"rows": rows}, row_id)
    if owner != row["owner"]:
        raise RowError("matrix row producer owner does not match policy authority")
    if row["target"] != matrix.local_target():
        raise RowError(
            f"matrix row target is not this host: {row['target']} != {matrix.local_target()}"
        )
    compiler_id, compiler_version = require_build_contract(root, build, row)
    binary = executable(build, "xray")
    writer = executable(build, "tests/unit/test_xtp_format")
    exact_version(binary, root, identity)
    timeout_seconds = int(row["timeout_seconds"])
    argv = matrix.exact_row_command(row["command"])
    command_code, command_text = run_command(argv, root, timeout_seconds)
    if command_code != 0:
        raise RowError(
            f"matrix exact command exited {command_code}\n{command_text}"
        )
    if output.exists() or output.is_symlink():
        raise RowError("matrix row result already exists; producer never overwrites")
    output.parent.mkdir(parents=True, exist_ok=True)
    lock = output.with_name(f".{output.name}.row-lock")
    try:
        lock.mkdir()
    except FileExistsError as error:
        raise RowError("another matrix row producer owns the output") from error
    staging: Path | None = None
    try:
        staging = Path(tempfile.mkdtemp(
            prefix=f".{output.name}-staging-", dir=output.parent
        ))
        work = staging / "work"
        work.mkdir()
        xsm, xtp = verify_activation(binary, writer, work, timeout_seconds)
        (staging / "logs").mkdir()
        (staging / "artifacts").mkdir()
        (staging / "binaries").mkdir()
        log_relative = f"logs/{row_id}.log"
        artifact_relative = f"artifacts/{row_id}.xtp"
        binary_relative = f"binaries/{row_id}{'.exe' if os.name == 'nt' else ''}"
        log = staging / log_relative
        artifact = staging / artifact_relative
        retained_binary = staging / binary_relative
        log.write_text(command_text, encoding="utf-8")
        shutil.copyfile(xtp, artifact)
        shutil.copyfile(binary, retained_binary)
        generated_at = datetime.datetime.now(datetime.timezone.utc).isoformat().replace(
            "+00:00", "Z"
        )
        platform = {
            "os": host_platform.system() or "unknown-os",
            "arch": host_platform.machine() or "unknown-arch",
            "toolchain": f"{compiler_id}-{compiler_version}",
        }
        artifact_sha = assembler.sha256_file(artifact)
        binary_sha = assembler.sha256_file(retained_binary)
        result = {
            "schema": matrix.ROW_RESULT_SCHEMA,
            "row_id": row_id, "status": "passed",
            "source_commit": identity["source_commit"],
            "repository_sha256": identity["repository_sha256"],
            "governance_input_sha256": governance_hash,
            "policy_sha256": assembler.sha256_file(policy_path),
            "command": argv, "platform": platform,
            "target": row["target"], "provider": row["provider"],
            "artifact_route": row["artifact_route"],
            "executor_or_generation": row["executor_or_generation"],
            "build_or_sanitizer": row["build_or_sanitizer"],
            "exit_code": 0,
            "positive_activation": "activated-and-passed",
            "negative_mismatch": "rejected-before-activation",
            "artifact_retention": "retained",
            "artifact_fingerprint": artifact_sha,
            "binary_fingerprint": binary_sha,
            "artifact": artifact_relative, "artifact_sha256": artifact_sha,
            "binary": binary_relative, "binary_sha256": binary_sha,
            "source_fingerprint": identity["repository_sha256"],
            "last_verified": generated_at,
            "log": log_relative, "log_sha256": assembler.sha256_file(log),
            "normalized_log_sha256": matrix.command_log_sha256(argv, command_text),
            "identity_sha256": "",
        }
        result["identity_sha256"] = matrix.row_result_identity(result)
        assembler.write_object(staging / f"{row_id}.json", result)
        shutil.rmtree(work)
        if output.exists() or output.is_symlink():
            raise RowError("matrix row result appeared before publication")
        os.rename(staging, output)
        staging = None
    finally:
        if staging is not None:
            shutil.rmtree(staging, ignore_errors=True)
        try:
            lock.rmdir()
        except OSError:
            pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--row-id", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--owner", required=True)
    args = parser.parse_args()
    try:
        produce(Path(args.root).resolve(), Path(args.build_dir).resolve(),
                args.row_id, Path(args.output_dir).resolve(), args.owner)
    except (RowError, assembler.AssemblyError, OSError, KeyError,
            TypeError, ValueError) as error:
        print(f"target-machine matrix row: ERROR: {error}", file=sys.stderr)
        return 2
    print(f"target-machine matrix row: PASS: {args.row_id} -> {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
