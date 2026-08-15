#!/usr/bin/env python3
"""Self-test the independent target-machine matrix row producer."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tests" / "target-machine"))
import assemble_target_machine_completion_evidence as assembler  # noqa: E402
import collect_target_machine_matrix_evidence as collector  # noqa: E402
import run_target_machine_matrix_row as runner  # noqa: E402
import test_matrix_evidence_collector as fixture  # noqa: E402


def run(arguments: list[str], cwd: Path) -> str:
    result = subprocess.run(arguments, cwd=cwd, check=False, capture_output=True,
                            text=True, encoding="utf-8")
    if result.returncode != 0:
        raise AssertionError(result.stdout + result.stderr)
    return result.stdout.strip()


def write_build(root: Path, provider: str) -> Path:
    build = root / "build"
    compiler_dir = build / "CMakeFiles" / "fixture"
    compiler_dir.mkdir(parents=True)
    compiler = {
        "msvc": ("MSVC", "19.44.35219"),
        "host-clang": ("AppleClang", "18.0.0"),
        "gcc": ("GNU", "15.1.0"),
        "clang": ("Clang", "20.1.0"),
    }[provider]
    (build / "CMakeCache.txt").write_text(
        "CMAKE_GENERATOR:INTERNAL=Ninja\n"
        "CMAKE_BUILD_TYPE:STRING=Release\n"
        "XRAY_STDLIB_VM_FASTPATHS:BOOL=OFF\n"
        "ENABLE_ASAN:BOOL=OFF\n"
        "ENABLE_UBSAN:BOOL=OFF\n"
        "ENABLE_TSAN:BOOL=OFF\n",
        encoding="utf-8",
    )
    (compiler_dir / "CMakeCCompiler.cmake").write_text(
        f'set(CMAKE_C_COMPILER_ID "{compiler[0]}")\n'
        f'set(CMAKE_C_COMPILER_VERSION "{compiler[1]}")\n',
        encoding="utf-8",
    )
    suffix = ".exe" if os.name == "nt" else ""
    binary = build / f"xray{suffix}"
    writer = build / "tests" / "unit" / f"test_xtp_format{suffix}"
    writer.parent.mkdir(parents=True)
    binary.write_bytes(b"fixture xray binary\n")
    writer.write_bytes(b"fixture writer binary\n")
    if os.name != "nt":
        binary.chmod(0o755)
        writer.chmod(0o755)
    return build


def local_provider() -> str:
    target = collector.local_target()
    if target == "windows-x86_64":
        return "msvc"
    if target == "macos-arm64":
        return "host-clang"
    if target == "linux-x86_64":
        return "gcc"
    raise AssertionError(f"unsupported self-test host {target}")


def initialize(root: Path) -> tuple[dict[str, object], Path]:
    policy = fixture.initialize(root)
    (root / ".gitignore").write_text("/build/\n", encoding="utf-8")
    matrix_path = root / "contracts/target-machine/validation-matrix.json"
    matrix_policy = assembler.read_object(matrix_path)
    row = matrix_policy["rows"][0]
    row["target"] = collector.local_target()
    row["provider"] = local_provider()
    row["command"] = "fixture-run --exact"
    assembler.write_object(matrix_path, matrix_policy)
    run(["git", "add", "."], root)
    run(["git", "commit", "-m", "local matrix row"], root)
    return policy, write_build(root, row["provider"])


def self_test() -> int:
    mutations = 0
    with tempfile.TemporaryDirectory(prefix="xray-matrix-row-") as directory:
        parent = Path(directory)
        root = parent / "repo"
        root.mkdir()
        policy, build = initialize(root)
        identity = collector.repository_identity(root)
        mode = {"value": "pass"}
        original_run = runner.run_command

        def fake_run(argv: list[str], cwd: Path,
                     timeout_seconds: int) -> tuple[int, str]:
            del cwd, timeout_seconds
            if argv == ["fixture-run", "--exact"]:
                return ((1, "fixture command failed\n")
                        if mode["value"] == "command-fail"
                        else (0, "fixture exact command passed\n"))
            if len(argv) >= 3 and argv[1:] == ["--version", "--json"]:
                dirty = mode["value"] == "dirty-binary"
                return 0, json.dumps({
                    "commit": identity["source_commit"], "dirty": dirty,
                }) + "\n"
            if "--write-runtime-artifacts" in argv:
                if mode["value"] != "writer-missing":
                    Path(argv[-2]).write_bytes(b"xsm" + b"s" * 96)
                    Path(argv[-1]).write_bytes(b"xtp" + b"t" * 96)
                return 0, "writer passed\n"
            if len(argv) >= 2 and argv[1] == "run":
                if "mismatch" in argv[2]:
                    if mode["value"] == "negative-activates":
                        return 0, "42\nactivation_ns=1\n"
                    return 1, "XR_ARTIFACT_2001: checksum mismatch\n"
                if mode["value"] == "positive-fail":
                    return 1, "positive failed\n"
                return 0, (
                    "42\nsemantic_verify_ns=1\ntarget_verify_ns=1\n"
                    "activation_ns=1\n"
                )
            return 127, f"unexpected fake argv: {argv!r}\n"

        runner.run_command = fake_run
        try:
            output = parent / "result"
            runner.produce(root, build, "TM-MATRIX-FIXTURE", output,
                           "fixture-owner")
            result_path = output / "TM-MATRIX-FIXTURE.json"
            result = assembler.read_object(result_path)
            if (set(result) != collector.ROW_RESULT_FIELDS
                    or result["identity_sha256"]
                    != collector.row_result_identity(result)):
                raise AssertionError("matrix row producer result identity is stale")
            if (not (output / result["artifact"]).is_file()
                    or not (output / result["binary"]).is_file()
                    or not (output / result["log"]).is_file()):
                raise AssertionError("matrix row producer did not retain exact evidence")

            original_collect_run = collector.run_exact_command
            collector.run_exact_command = (
                lambda argv, cwd, timeout: (0, "fixture exact command passed\n")
            )
            try:
                collected = parent / "collected"
                if collector.collect(root, output, collected, "fixture-owner") != 0:
                    raise AssertionError("matrix collector rejected producer output")
            finally:
                collector.run_exact_command = original_collect_run

            try:
                runner.produce(root, build, "TM-MATRIX-FIXTURE", output,
                               "fixture-owner")
            except runner.RowError:
                mutations += 1
            else:
                raise AssertionError("matrix row producer overwrote its output")

            for label in ("command-fail", "dirty-binary", "writer-missing",
                          "positive-fail", "negative-activates"):
                mode["value"] = label
                broken = parent / f"broken-{label}"
                try:
                    runner.produce(root, build, "TM-MATRIX-FIXTURE", broken,
                                   "fixture-owner")
                except runner.RowError:
                    if broken.exists():
                        raise AssertionError(f"{label} published partial evidence")
                    mutations += 1
                else:
                    raise AssertionError(f"matrix row mutation accepted: {label}")
            mode["value"] = "pass"

            cache = build / "CMakeCache.txt"
            original_cache = cache.read_text(encoding="utf-8")
            cache.write_text(
                original_cache.replace(
                    "XRAY_STDLIB_VM_FASTPATHS:BOOL=OFF",
                    "XRAY_STDLIB_VM_FASTPATHS:BOOL=ON",
                ), encoding="utf-8",
            )
            try:
                runner.produce(root, build, "TM-MATRIX-FIXTURE",
                               parent / "broken-fastpaths", "fixture-owner")
            except runner.RowError:
                mutations += 1
            else:
                raise AssertionError("FASTPATHS=ON matrix row was accepted")
            cache.write_text(original_cache, encoding="utf-8")

            try:
                runner.produce(root, parent / "alternate-build",
                               "TM-MATRIX-FIXTURE",
                               parent / "broken-build-root", "fixture-owner")
            except runner.RowError:
                mutations += 1
            else:
                raise AssertionError("non-governed matrix build root was accepted")

            try:
                runner.produce(root, build, "TM-MATRIX-FIXTURE",
                               parent / "broken-owner", "forged-owner")
            except runner.RowError:
                mutations += 1
            else:
                raise AssertionError("non-authoritative matrix row owner was accepted")
        finally:
            runner.run_command = original_run
    print(f"target-machine matrix row self-test: PASS ({mutations} mutations)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    return self_test() if args.self_test else 2


if __name__ == "__main__":
    raise SystemExit(main())
