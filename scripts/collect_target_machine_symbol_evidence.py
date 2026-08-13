#!/usr/bin/env python3
"""Collect identity-bound raw symbol evidence for target-machine products."""

from __future__ import annotations

import argparse
import datetime
import os
import platform as host_platform
import shutil
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


PRODUCER = "target-machine-symbol-evidence/1"
KIND = "symbol"
TARGET_FILES = {
    "xray-cli": ("xray.exe", "xray"),
    "libxray-exec-core": ("xray_rt_coro.lib", "libxray_rt_coro.a"),
    "libxray-vm": ("xray_vm.lib", "libxray_vm.a"),
    "libxray-compiler": ("xray_compiler.lib", "libxray_compiler.a"),
}
REQUIRED_AUTHORITY_SYMBOLS = {
    "libxray-compiler": {
        "xr_target_plan_build",
        "xr_xsm_encode",
        "xr_xtp_encode_plan",
        "xr_c_emission_plan_build",
    },
}
RETIRED_RUNTIME_EXACT = retired_runtime.EXACT
RETIRED_RUNTIME_PREFIXES = retired_runtime.PREFIXES


class CollectionError(ValueError):
    pass


def repository_identity(root: Path) -> dict[str, str]:
    identity, findings = completion.repository_identity(root)
    if findings:
        detail = "; ".join(row.message for row in findings)
        raise CollectionError(f"source identity is not collectable: {detail}")
    return identity


def platform_identity() -> dict[str, str]:
    inspector = binlib.find_nm() or binlib.find_dumpbin() or "unavailable"
    return {
        "os": host_platform.system() or "unknown-os",
        "arch": host_platform.machine() or "unknown-arch",
        "toolchain": f"symbol-inspector={inspector}",
    }


def canonical_command(root: Path, build: Path, output: Path,
                      owner: str) -> list[str]:
    return [
        sys.executable,
        str((root / "scripts" /
             "collect_target_machine_symbol_evidence.py").resolve()),
        "--root", str(root), "--build", str(build),
        "--output-dir", str(output), "--owner", owner,
    ]


def artifact_path(build: Path, name: str) -> Path | None:
    candidates = TARGET_FILES.get(name, ())
    for filename in candidates:
        direct = build / filename
        if direct.is_file():
            return direct
    return None


def forbidden_symbols(symbols: list[str]) -> list[str]:
    return sorted({
        symbol for symbol in symbols
        if retired_runtime.matches(symbol)
    })


def collect(root: Path, build: Path, output: Path, owner: str) -> int:
    governance = assembler.read_object(root / completion.DEFAULT_MANIFEST)
    if completion.validate_manifest(governance):
        raise CollectionError("completion governance manifest is invalid")
    identity = repository_identity(root)
    governance_hash = governance["input_identity"]["sha256"]
    actual_governance = completion.framed_tree_hash(
        root, governance["input_identity"]["files"]
    )
    generated_at = datetime.datetime.now(datetime.timezone.utc).isoformat().replace(
        "+00:00", "Z"
    )
    command = canonical_command(root, build, output, owner)
    platform = platform_identity()
    if output.exists() or output.is_symlink():
        raise CollectionError("raw evidence package already exists; collection never overwrites")
    output.parent.mkdir(parents=True, exist_ok=True)
    lock = output.with_name(f".{output.name}.collect-lock")
    try:
        lock.mkdir()
    except FileExistsError as error:
        raise CollectionError("another symbol collection owns the output") from error
    staging: Path | None = None
    try:
        staging = Path(tempfile.mkdtemp(
            prefix=f".{output.name}-staging-", dir=output.parent
        ))
        (staging / "artifacts").mkdir()
        (staging / "logs").mkdir()
        binaries: list[dict[str, Any]] = []
        raw_logs: list[dict[str, Any]] = []
        required = governance["installed"]["required_deliverables"]
        passed = actual_governance == governance_hash and set(required) == set(TARGET_FILES)
        per_row_pass: list[bool] = []
        for name in required:
            source = artifact_path(build, name)
            filename = TARGET_FILES[name][0 if os.name == "nt" else 1]
            artifact_relative = f"artifacts/{name}-{filename}"
            artifact = staging / artifact_relative
            symbols: list[str] | None = None
            missing = source is None
            if source is not None:
                shutil.copyfile(source, artifact)
                symbols = binlib.defined_symbol_names(artifact)
            legacy = forbidden_symbols(symbols or [])
            missing_authorities = sorted(
                REQUIRED_AUTHORITY_SYMBOLS.get(name, set()) - set(symbols or [])
            )
            row_pass = (not missing and symbols is not None and not legacy and
                        not missing_authorities)
            per_row_pass.append(row_pass)
            passed = passed and row_pass
            symbol_relative = f"logs/{name}.symbols.log"
            symbol_log = staging / symbol_relative
            if missing:
                symbol_text = f"artifact missing for {name} in {build}\n"
            elif symbols is None:
                symbol_text = f"no supported symbol inspector for {artifact}\n"
            else:
                symbol_text = (
                    f"artifact={source}\n"
                    f"artifact_sha256={assembler.sha256_file(artifact)}\n"
                    f"defined_symbols={len(symbols)}\n"
                    f"forbidden_symbols={len(legacy)}\n"
                    f"missing_authority_symbols={len(missing_authorities)}\n"
                    + "".join(
                        f"missing_authority={symbol}\n"
                        for symbol in missing_authorities
                    )
                    + "\n".join(symbols) + "\n"
                )
            symbol_log.write_text(symbol_text, encoding="utf-8")
            artifact_digest = (assembler.sha256_file(artifact)
                               if artifact.is_file() else "0" * 64)
            binaries.append({
                "name": name, "path": artifact_relative,
                "sha256": artifact_digest,
                "forbidden_symbol_count": len(legacy) if symbols is not None else 1,
                "symbol_log": symbol_relative,
            })
            for relative, path in ((artifact_relative, artifact),
                                   (symbol_relative, symbol_log)):
                if not path.is_file():
                    continue
                raw_logs.append({
                    "path": relative,
                    "sha256": assembler.sha256_file(path),
                    "identity_sha256": "",
                    "result": "passed" if row_pass else "failed",
                })
        status = "passed" if passed else "failed"
        exit_code = 0 if passed else 1
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
            "payload": {"producer": PRODUCER, "binaries": binaries},
            "logs": raw_logs,
        }
        assembler.write_object(staging / "symbol.raw.json", raw)
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
        print(f"target-machine symbol evidence: ERROR: {error}", file=sys.stderr)
        return 2
    print(f"target-machine symbol evidence: {'PASS' if code == 0 else 'FAIL'}: {output}")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
