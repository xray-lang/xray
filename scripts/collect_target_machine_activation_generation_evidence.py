#!/usr/bin/env python3
"""Collect identity-bound activation/generation evidence from real product gates."""

from __future__ import annotations

import argparse
import datetime
import json
import os
import platform as host_platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "scripts") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts"))
import assemble_target_machine_completion_evidence as assembler
import check_target_machine_completion as completion


TEST_LIB = ROOT / "tests" / "lib"
if str(TEST_LIB) not in sys.path:
    sys.path.insert(0, str(TEST_LIB))
from xraytest import binary as binlib  # noqa: E402


PRODUCER = "target-machine-activation-generation-evidence/1"
KIND = "activation-generation"
COMPILER_SYMBOL_PREFIXES = (
    "xa_", "xi_", "xicgen_", "xcompiler_", "xr_semantic_plan_build",
    "xr_target_plan_build",
)


class CollectionError(ValueError):
    pass


def repository_identity(root: Path) -> dict[str, str]:
    identity, findings = completion.repository_identity(root)
    if findings:
        raise CollectionError("source identity is not collectable: " + "; ".join(
            row.message for row in findings
        ))
    return identity


def executable(build: Path, relative: str) -> Path:
    path = build / relative
    if os.name == "nt" and path.suffix != ".exe":
        path = path.with_suffix(".exe")
    return path


def canonical_command(root: Path, build: Path, output: Path,
                      owner: str) -> list[str]:
    return [
        sys.executable,
        str((root / "scripts" /
             "collect_target_machine_activation_generation_evidence.py").resolve()),
        "--root", str(root), "--build", str(build),
        "--output-dir", str(output), "--owner", owner,
    ]


def platform_identity(build: Path) -> dict[str, str]:
    compiler = "unknown-compiler"
    cache = build / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_C_COMPILER:") and "=" in line:
                compiler = line.split("=", 1)[1]
                break
    return {
        "os": host_platform.system() or "unknown-os",
        "arch": host_platform.machine() or "unknown-arch",
        "toolchain": f"runtime-generation; compiler={compiler}",
    }


def run(command: list[str], cwd: Path, timeout: int = 600) -> tuple[int, str]:
    try:
        process = subprocess.run(
            command, cwd=cwd, check=False, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=timeout,
        )
        return process.returncode, process.stdout + process.stderr
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout.decode("utf-8", errors="replace") \
            if isinstance(error.stdout, bytes) else (error.stdout or "")
        stderr = error.stderr.decode("utf-8", errors="replace") \
            if isinstance(error.stderr, bytes) else (error.stderr or "")
        return 124, stdout + stderr + "\ncommand timed out\n"
    except OSError as error:
        return 127, f"command could not start: {error}\n"


def exact_identities(path: Path) -> dict[str, str] | None:
    try:
        data = assembler.read_object(path)
    except (assembler.AssemblyError, OSError):
        return None
    expected = {"artifact", "generation", "semantic", "target"}
    if data.get("schema") != 1 or set(data) != expected | {"schema"}:
        return None
    identities = {name: data[name] for name in expected}
    if any(not completion.exact_sha256(value) for value in identities.values()):
        return None
    return identities


def runtime_compiler_symbols(archive: Path) -> tuple[int, str]:
    if not archive.is_file():
        return 1, f"runtime archive missing: {archive}\n"
    symbols = binlib.defined_symbol_names(archive)
    if symbols is None:
        return 1, f"symbol inspector unavailable for {archive}\n"
    forbidden = sorted({
        symbol for symbol in symbols if symbol.startswith(COMPILER_SYMBOL_PREFIXES)
    })
    return len(forbidden), (
        f"archive={archive}\ndefined_symbols={len(symbols)}\n"
        f"runtime_only_compiler_symbols={len(forbidden)}\n" +
        "\n".join(forbidden) + "\n"
    )


def evaluate(codes: dict[str, int], identities: dict[str, str] | None,
             compiler_symbols: int, required_routes: list[str]) -> tuple[dict[str, Any], bool]:
    lifecycle_ok = codes["generation"] == 0
    archive_ok = codes["archive"] == 0 and identities is not None
    xtp_ok = codes["xtp"] == 0
    cli_ok = codes["cli"] == 0
    lifecycle = {
        name: {"result": "passed" if lifecycle_ok else "failed",
               "log": "logs/runtime-generation.log"}
        for name in ("activate", "drain", "rollback", "unload")
    }
    negative_mismatches = {
        "capability": "rejected-before-activation" if xtp_ok else "failed",
        "fingerprint": "rejected-before-activation" if lifecycle_ok else "failed",
        "provider": "rejected-before-activation" if xtp_ok else "failed",
        "schema": "rejected-before-activation" if xtp_ok else "failed",
        "target": "rejected-before-activation" if xtp_ok else "failed",
    }
    routes: list[str] = []
    if cli_ok:
        routes.extend(("source-to-xsm", "xsm-to-xtp", "xtp-to-vm"))
    if archive_ok and compiler_symbols == 0:
        routes.append("runtime-only-embed")
    if lifecycle_ok:
        routes.append("generation-lifecycle")
    payload = {
        "producer": PRODUCER,
        "lifecycle": lifecycle,
        "negative_mismatches": negative_mismatches,
        "identities": identities or {},
        "activation_before_verify": 0 if lifecycle_ok and archive_ok else 1,
        "runtime_only_compiler_symbols": compiler_symbols,
        "artifact_routes": sorted(set(routes)),
        "missing_artifact_routes": sorted(set(required_routes) - set(routes)),
    }
    passed = (
        lifecycle_ok and archive_ok and xtp_ok and cli_ok and compiler_symbols == 0
        and not payload["missing_artifact_routes"]
        and all(value == "rejected-before-activation"
                for value in negative_mismatches.values())
    )
    return payload, passed


def collect(root: Path, build: Path, output: Path, owner: str) -> int:
    governance = assembler.read_object(root / completion.DEFAULT_MANIFEST)
    if completion.validate_manifest(governance):
        raise CollectionError("completion governance manifest is invalid")
    identity = repository_identity(root)
    governance_hash = governance["input_identity"]["sha256"]
    actual_governance = completion.framed_tree_hash(
        root, governance["input_identity"]["files"]
    )
    if output.exists() or output.is_symlink():
        raise CollectionError("raw evidence package already exists; collection never overwrites")
    generation = executable(build, "tests/unit/test_runtime_generation")
    archive = executable(build, "tests/unit/test_runtime_target_plan_load_archive")
    xtp = executable(build, "tests/unit/test_xtp_format")
    xray = executable(build, "xray")
    required = (generation, archive, xtp, xray)
    if any(not path.is_file() for path in required):
        missing = ", ".join(str(path) for path in required if not path.is_file())
        raise CollectionError(f"activation evidence requires built executables: {missing}")
    output.parent.mkdir(parents=True, exist_ok=True)
    lock = output.with_name(f".{output.name}.collect-lock")
    try:
        lock.mkdir()
    except FileExistsError as error:
        raise CollectionError("another activation collection owns the output") from error
    staging: Path | None = None
    try:
        staging = Path(tempfile.mkdtemp(
            prefix=f".{output.name}-staging-", dir=output.parent
        ))
        logs = staging / "logs"
        logs.mkdir()
        identity_path = staging / "runtime-identities.json"
        commands = {
            "generation": [str(generation)],
            "archive": [str(archive), "--evidence-json", str(identity_path)],
            "xtp": [str(xtp)],
            "cli": [
                sys.executable,
                str(root / "tests/cli/run_target_artifact_boundary_tests.py"),
                "--binary", str(xray), "--xtp-writer", str(xtp),
            ],
        }
        codes: dict[str, int] = {}
        outputs: dict[str, str] = {}
        for name, command in commands.items():
            codes[name], outputs[name] = run(command, root)
            (logs / f"{name if name != 'generation' else 'runtime-generation'}.log").write_text(
                f"argv={json.dumps(command, ensure_ascii=False)}\n"
                f"exit_code={codes[name]}\n{outputs[name]}", encoding="utf-8",
            )
        identities = exact_identities(identity_path)
        runtime_archive = build / ("xray_vm.lib" if os.name == "nt"
                                   else "libxray_vm.a")
        compiler_symbols, symbol_text = runtime_compiler_symbols(runtime_archive)
        (logs / "runtime-symbols.log").write_text(symbol_text, encoding="utf-8")
        payload, product_passed = evaluate(
            codes, identities, compiler_symbols,
            governance["matrix"]["required_artifact_routes"],
        )
        payload["runtime_archive_symbol_log"] = "logs/runtime-symbols.log"
        payload["identity_log"] = "logs/runtime-identities.json"
        shutil.copyfile(identity_path, logs / "runtime-identities.json") \
            if identity_path.is_file() else (logs / "runtime-identities.json").write_text(
                "runtime identity output missing\n", encoding="utf-8"
            )
        passed = product_passed and actual_governance == governance_hash
        status = "passed" if passed else "failed"
        exit_code = 0 if passed else 1
        generated_at = datetime.datetime.now(datetime.timezone.utc).isoformat().replace(
            "+00:00", "Z"
        )
        command = canonical_command(root, build, output, owner)
        platform = platform_identity(build)
        raw_logs: list[dict[str, Any]] = []
        for path in sorted(logs.iterdir()):
            relative = f"logs/{path.name}"
            digest = assembler.sha256_file(path)
            raw_logs.append({
                "path": relative, "sha256": digest,
                "identity_sha256": assembler.raw_log_identity(
                    KIND, identity["source_commit"], identity["repository_sha256"],
                    governance_hash, relative, digest, owner, generated_at,
                    command, platform, exit_code, status,
                ),
                "result": "passed" if passed else "failed",
            })
        raw = {
            "schema": assembler.RAW_SCHEMA, "kind": KIND,
            "status": status, "exit_code": exit_code,
            "source_commit": identity["source_commit"],
            "repository_sha256": identity["repository_sha256"],
            "governance_input_sha256": governance_hash,
            "owner": owner, "generated_at": generated_at,
            "command": command, "platform": platform,
            "payload": payload, "logs": raw_logs,
        }
        assembler.write_object(staging / "activation-generation.raw.json", raw)
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
    try:
        code = collect(root, build.resolve(), Path(args.output_dir).resolve(), args.owner)
    except (CollectionError, OSError, KeyError, TypeError, ValueError) as error:
        print(f"target-machine activation evidence: ERROR: {error}", file=sys.stderr)
        return 2
    print(f"target-machine activation evidence: {'PASS' if code == 0 else 'FAIL'}: "
          f"{Path(args.output_dir).resolve()}")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
