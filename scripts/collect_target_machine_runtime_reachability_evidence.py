#!/usr/bin/env python3
"""Collect identity-bound raw runtime reachability evidence."""

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
import target_machine_retired_runtime_symbols as retired_runtime


ROOT = Path(__file__).resolve().parents[1]
TEST_LIB = ROOT / "tests" / "lib"
if str(TEST_LIB) not in sys.path:
    sys.path.insert(0, str(TEST_LIB))
from xraytest import binary as binlib  # noqa: E402


PRODUCER = "target-machine-runtime-reachability-evidence/1"
KIND = "runtime-reachability"
EXPECTED_RESULTS = {
    "bytecode-flag-negative": "rejected-before-activation",
    "legacy-api-negative": "rejected-before-activation",
    "runtime-only-link": "linked-and-passed",
    "source-eval-negative": "rejected-before-activation",
    "xrc-negative": "rejected-before-activation",
    "xtp-positive": "activated-and-passed",
}
LEGACY_API_RE = retired_runtime.compiled_pattern()
COMPILER_LINK_RE = re.compile(
    r"^(?:xr_parse|xr_compile|xr_semantic_plan_build$|xr_target_plan_build$|"
    r"xr_xtp_encode_plan$|xa_analyzer|xanalyzer_|xi_|xray_build|xtc_)"
)
REJECTION_MARKERS = (
    "unknown command", "unknown option", "not supported", "removed",
    "XR_ARTIFACT_2000", "XR_ARTIFACT_2005", "XR_ARTIFACT_2006",
)


class CollectionError(ValueError):
    pass


def repository_identity(root: Path) -> dict[str, str]:
    identity, findings = completion.repository_identity(root)
    if findings:
        detail = "; ".join(row.message for row in findings)
        raise CollectionError(f"source identity is not collectable: {detail}")
    if completion.COMMIT_RE.fullmatch(identity.get("source_commit", "")) is None:
        raise CollectionError("repository commit identity is not exact")
    if not completion.exact_sha256(identity.get("repository_sha256")):
        raise CollectionError("repository tree identity is not exact")
    return identity


def require_ninja_build(build: Path) -> str:
    cache = build / "CMakeCache.txt"
    if not cache.is_file() or not (build / "build.ninja").is_file():
        raise CollectionError("runtime evidence requires a configured Ninja build")
    text = cache.read_text(encoding="utf-8", errors="replace")
    if "CMAKE_GENERATOR:INTERNAL=Ninja" not in text:
        raise CollectionError("runtime evidence requires the canonical Ninja generator")
    compiler = "unknown-compiler"
    for line in text.splitlines():
        if line.startswith("CMAKE_C_COMPILER:FILEPATH="):
            compiler = line.split("=", 1)[1].strip() or compiler
            break
    return compiler


def artifact(build: Path, relative: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    normalized = relative.replace("$EXE", suffix)
    path = build / normalized
    if not path.is_file():
        raise CollectionError(f"required runtime evidence artifact is missing: {path}")
    return path.resolve()


def platform_identity(compiler: str) -> dict[str, str]:
    inspector = binlib.find_nm() or binlib.find_dumpbin() or "unavailable"
    return {
        "os": host_platform.system() or "unknown-os",
        "arch": host_platform.machine() or "unknown-arch",
        "toolchain": f"compiler={compiler}; symbol-inspector={inspector}",
    }


def canonical_command(root: Path, build: Path, output: Path,
                      owner: str) -> list[str]:
    return [
        sys.executable,
        str((root / "scripts" /
             "collect_target_machine_runtime_reachability_evidence.py").resolve()),
        "--root", str(root), "--build", str(build),
        "--output-dir", str(output), "--owner", owner,
    ]


def run_command(arguments: list[str], cwd: Path) -> tuple[int, str]:
    try:
        result = subprocess.run(
            arguments, cwd=cwd, check=False, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=180,
        )
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout.decode("utf-8", errors="replace") \
            if isinstance(error.stdout, bytes) else (error.stdout or "")
        stderr = error.stderr.decode("utf-8", errors="replace") \
            if isinstance(error.stderr, bytes) else (error.stderr or "")
        return 124, stdout + stderr + "\ncommand timed out\n"
    except OSError as error:
        return 127, f"command could not start: {error}\n"
    return result.returncode, result.stdout + result.stderr


def rejected(code: int, output: str, marker: str | None = None) -> bool:
    if code == 0:
        return False
    if marker is not None:
        return marker in output
    lowered = output.lower()
    return any(value.lower() in lowered for value in REJECTION_MARKERS)


def legacy_api_probe_command(compiler: str, source: Path, archive: Path,
                             executable: Path) -> list[str]:
    if os.name == "nt":
        return [
            compiler, "/nologo", "/std:c11", str(source), str(archive),
            f"/Fe:{executable}",
        ]
    return [compiler, "-std=c11", str(source), str(archive), "-o", str(executable)]


def write_lane_log(path: Path, commands: list[list[str]], records: list[tuple[int, str]],
                   detail: list[str]) -> None:
    lines: list[str] = []
    for index, (command, (code, output)) in enumerate(zip(commands, records)):
        lines.extend((
            f"command[{index}]={json.dumps(command, ensure_ascii=False)}",
            f"exit_code[{index}]={code}", output.rstrip(),
        ))
    lines.extend(detail)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def collect(root: Path, build: Path, output: Path, owner: str) -> int:
    governance = assembler.read_object(root / completion.DEFAULT_MANIFEST)
    if completion.validate_manifest(governance):
        raise CollectionError("completion governance manifest is invalid")
    expected = governance["runtime_reachability"]["required_lanes"]
    if expected != EXPECTED_RESULTS:
        raise CollectionError("runtime reachability lane authority is not exact")
    compiler = require_ninja_build(build)
    identity = repository_identity(root)
    governance_hash = completion.governance_input_sha256(root, governance)
    binary = artifact(build, "xray$EXE")
    writer = artifact(build, "tests/unit/test_xtp_format$EXE")
    runtime_link = artifact(build, "tests/unit/test_typed_frame_runtime_archive$EXE")
    activation_canary = artifact(
        build, "tests/unit/test_runtime_target_plan_load_archive$EXE"
    )
    archive_name = "xray_vm.lib" if os.name == "nt" else "libxray_vm.a"
    runtime_archive = artifact(build, archive_name)
    generated_at = datetime.datetime.now(datetime.timezone.utc).isoformat().replace(
        "+00:00", "Z"
    )
    command = canonical_command(root, build, output, owner)
    platform = platform_identity(compiler)
    if output.exists() or output.is_symlink():
        raise CollectionError("raw evidence package already exists; collection never overwrites")
    output.parent.mkdir(parents=True, exist_ok=True)
    lock = output.with_name(f".{output.name}.collect-lock")
    try:
        lock.mkdir()
    except FileExistsError as error:
        raise CollectionError("another runtime collection owns the output") from error
    staging: Path | None = None
    try:
        staging = Path(tempfile.mkdtemp(
            prefix=f".{output.name}-staging-", dir=output.parent
        ))
        logs = staging / "logs"
        work = staging / "work"
        logs.mkdir()
        work.mkdir()
        source = work / "legacy-source.xr"
        source.write_text('print("legacy-runtime-route")\n', encoding="utf-8")
        retired_config_init = "".join(("xray_", "vm_", "config_", "init"))
        legacy_api_source = work / "legacy-api-probe.c"
        legacy_api_source.write_text(
            f"extern void {retired_config_init}(void *);\n"
            f"int main(void) {{ {retired_config_init}(0); return 0; }}\n",
            encoding="utf-8",
        )
        xrc = work / "legacy-route.xrc"
        xsm = work / "runtime-semantic.xsm"
        xtp = work / "runtime-target.xtp"

        symbols = binlib.defined_symbol_names(runtime_archive)
        legacy_symbols = sorted(symbol for symbol in (symbols or []) if LEGACY_API_RE.match(symbol))
        compiler_symbols = sorted(
            symbol for symbol in (symbols or []) if COMPILER_LINK_RE.match(symbol)
        )

        lane_inputs: dict[str, tuple[list[list[str]], list[tuple[int, str]], bool,
                                     int, list[str]]] = {}
        bytecode_argv = [str(binary), "run", "--bytecode", str(source)]
        bytecode_record = run_command(bytecode_argv, work)
        lane_inputs["bytecode-flag-negative"] = (
            [bytecode_argv], [bytecode_record],
            rejected(*bytecode_record, marker="unknown option"),
            1 if bytecode_record[0] == 0 else 0,
            ["expected=unknown option rejection"],
        )

        legacy_api_executable = work / (
            "legacy-api-probe.exe" if os.name == "nt" else "legacy-api-probe"
        )
        legacy_api_argv = legacy_api_probe_command(
            compiler, legacy_api_source, runtime_archive, legacy_api_executable
        )
        legacy_api_record = run_command(legacy_api_argv, work)
        legacy_link_rejected = (
            legacy_api_record[0] != 0
            and retired_config_init in legacy_api_record[1]
            and not legacy_api_executable.is_file()
        )
        legacy_ok = symbols is not None and not legacy_symbols and legacy_link_rejected
        lane_inputs["legacy-api-negative"] = (
            [legacy_api_argv, ["defined-symbol-scan", str(runtime_archive)]],
            [legacy_api_record, (0 if symbols is not None else 1,
              "\n".join(symbols or ["symbol inspector unavailable"]) + "\n")],
            legacy_ok, 0,
            [f"legacy_api_link_rejected={str(legacy_link_rejected).lower()}",
             f"legacy_api_symbol_count={len(legacy_symbols)}",
             *[f"legacy_api_symbol={name}" for name in legacy_symbols]],
        )

        runtime_argv = [str(runtime_link)]
        runtime_record = run_command(runtime_argv, work)
        runtime_ok = (
            runtime_record[0] == 0
            and "runtime-only typed frame boundary passed" in runtime_record[1]
            and symbols is not None and not compiler_symbols
        )
        lane_inputs["runtime-only-link"] = (
            [runtime_argv, ["defined-symbol-scan", str(runtime_archive)]],
            [runtime_record, (0 if symbols is not None else 1,
                              "\n".join(symbols or ["symbol inspector unavailable"]) + "\n")],
            runtime_ok, 0,
            [f"forbidden_link_symbol_count={len(compiler_symbols)}",
             *[f"forbidden_link_symbol={name}" for name in compiler_symbols]],
        )

        eval_argv = [str(binary), "eval", 'print("runtime-eval-residue")']
        eval_record = run_command(eval_argv, work)
        lane_inputs["source-eval-negative"] = (
            [eval_argv], [eval_record], rejected(*eval_record),
            1 if eval_record[0] == 0 else 0,
            ["expected=source eval rejection"],
        )

        compile_argv = [str(binary), "compile", str(source), "-o", str(xrc)]
        compile_record = run_command(compile_argv, work)
        xrc_commands = [compile_argv]
        xrc_records = [compile_record]
        if compile_record[0] == 0 and xrc.is_file():
            xrc_argv = [str(binary), "run", str(xrc)]
            xrc_record = run_command(xrc_argv, work)
            xrc_commands.append(xrc_argv)
            xrc_records.append(xrc_record)
            xrc_ok = rejected(*xrc_record)
            xrc_activation = 1 if xrc_record[0] == 0 else 0
        else:
            xrc_ok = rejected(*compile_record)
            xrc_activation = 0
        lane_inputs["xrc-negative"] = (
            xrc_commands, xrc_records, xrc_ok, xrc_activation,
            ["expected=legacy XRC rejected before activation"],
        )

        write_argv = [str(writer), "--write-runtime-artifacts", str(xsm), str(xtp)]
        write_record = run_command(write_argv, work)
        xtp_commands = [write_argv]
        xtp_records = [write_record]
        xtp_ok = False
        if write_record[0] == 0 and xsm.is_file() and xtp.is_file():
            run_argv = [str(binary), "run", str(xtp), "--semantic-plan", str(xsm),
                        "--timings"]
            run_record = run_command(run_argv, work)
            xtp_commands.append(run_argv)
            xtp_records.append(run_record)
            xtp_ok = (
                run_record[0] == 0
                and re.search(r"(?m)^42$", run_record[1]) is not None
                and "semantic_verify_ns=" in run_record[1]
                and "target_verify_ns=" in run_record[1]
                and "activation_ns=" in run_record[1]
            )
        lane_inputs["xtp-positive"] = (
            xtp_commands, xtp_records, xtp_ok, 0,
            ["expected=verified XSM/XTP activation and scalar result 42"],
        )

        lanes: dict[str, dict[str, Any]] = {}
        raw_logs: list[dict[str, Any]] = []
        activation_before_verify = 0
        all_ok = True
        for name in sorted(EXPECTED_RESULTS):
            commands, records, ok, activation_count, detail = lane_inputs[name]
            activation_before_verify += activation_count
            relative = f"logs/{name}.log"
            path = staging / relative
            write_lane_log(path, commands, records, detail)
            digest = assembler.sha256_file(path)
            lanes[name] = {
                "result": EXPECTED_RESULTS[name] if ok else "failed",
                "command_replayed": True,
                "argv": commands,
                "exit_codes": [record[0] for record in records],
                "log": relative,
            }
            all_ok = all_ok and ok
            raw_logs.append({
                "path": relative, "sha256": digest,
                "identity_sha256": "", "result": "passed" if ok else "failed",
            })

        canary_argv = [str(activation_canary)]
        canary_record = run_command(canary_argv, work)
        canary_ok = (
            canary_record[0] == 0
            and "runtime scalar artifact load and execution passed" in canary_record[1]
        )
        canary_relative = "logs/external-activation-canary.log"
        canary_path = staging / canary_relative
        write_lane_log(canary_path, [canary_argv], [canary_record], [
            "expected=runtime-only external XSM/XTP verify, activate, execute",
        ])
        canary_digest = assembler.sha256_file(canary_path)
        raw_logs.append({
            "path": canary_relative, "sha256": canary_digest,
            "identity_sha256": "", "result": "passed" if canary_ok else "failed",
        })
        all_ok = (
            all_ok and canary_ok and activation_before_verify == 0
            and len(compiler_symbols) == 0
        )
        status = "passed" if all_ok else "failed"
        exit_code = 0 if all_ok else 1
        for row in raw_logs:
            row["identity_sha256"] = assembler.raw_log_identity(
                KIND, identity["source_commit"], identity["repository_sha256"],
                governance_hash, row["path"], row["sha256"], owner,
                generated_at, command, platform, exit_code, status,
            )
        raw = {
            "schema": assembler.RAW_SCHEMA, "kind": KIND,
            "status": status, "exit_code": exit_code,
            "source_commit": identity["source_commit"],
            "repository_sha256": identity["repository_sha256"],
            "governance_input_sha256": governance_hash,
            "owner": owner, "generated_at": generated_at,
            "command": command, "platform": platform,
            "payload": {
                "producer": PRODUCER, "lanes": lanes,
                "activation_before_verify": activation_before_verify,
                "forbidden_link_symbol_count": len(compiler_symbols),
                "external_activation_canary": "passed" if canary_ok else "failed",
                "external_activation_canary_argv": canary_argv,
                "external_activation_canary_log": canary_relative,
                "legacy_api_symbol_count": len(legacy_symbols),
                "legacy_api_symbols": legacy_symbols,
                "forbidden_link_symbols": compiler_symbols,
            },
            "logs": raw_logs,
        }
        assembler.write_object(staging / "runtime-reachability.raw.json", raw)
        if output.exists() or output.is_symlink():
            raise CollectionError("raw evidence package appeared before publication")
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
    args = parser.parse_args()
    root = Path(args.root).resolve()
    build = Path(args.build)
    if not build.is_absolute():
        build = root / build
    output = Path(args.output_dir).resolve()
    try:
        code = collect(root, build.resolve(), output, args.owner)
    except (CollectionError, OSError, KeyError, TypeError, ValueError) as error:
        print(f"target-machine runtime reachability evidence: ERROR: {error}",
              file=sys.stderr)
        return 2
    result = "PASS" if code == 0 else "FAIL"
    print(f"target-machine runtime reachability evidence: {result}: {output}")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
