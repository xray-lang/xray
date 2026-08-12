#!/usr/bin/env python3
"""Collect identity-bound raw evidence from a repeatable empty-stage install."""

from __future__ import annotations

import argparse
import datetime
import hashlib
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


PRODUCER = "target-machine-installed-evidence/1"
KIND = "installed"
DELIVERABLE_PATTERNS = {
    "xray-cli": ("bin/xray.exe", "bin/xray"),
    "libxray-exec-core": (
        "lib/xray/aot/*/xray_rt_coro.lib",
        "lib/xray/aot/*/libxray_rt_coro.a",
    ),
    "libxray-vm": (
        "lib/xray/vm/*/xray_vm.lib",
        "lib/xray/vm/*/libxray_vm.a",
    ),
    "libxray-compiler": (
        "lib/xray/compiler/*/xray_compiler.lib",
        "lib/xray/compiler/*/libxray_compiler.a",
    ),
}


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
        raise CollectionError("installed evidence requires a configured Ninja build")
    text = cache.read_text(encoding="utf-8", errors="replace")
    if "CMAKE_GENERATOR:INTERNAL=Ninja" not in text:
        raise CollectionError("installed evidence requires the canonical Ninja generator")
    compiler = "unknown-compiler"
    for key in ("CMAKE_C_COMPILER:FILEPATH=", "CMAKE_C_COMPILER:STRING="):
        for line in text.splitlines():
            if line.startswith(key) and line[len(key):].strip():
                compiler = line[len(key):].strip()
                break
        if compiler != "unknown-compiler":
            break
    return compiler


def platform_identity(compiler: str) -> dict[str, str]:
    return {
        "os": host_platform.system() or "unknown-os",
        "arch": host_platform.machine() or "unknown-arch",
        "toolchain": f"cmake-install; compiler={compiler}",
    }


def canonical_command(root: Path, build: Path, output: Path,
                      owner: str) -> list[str]:
    return [
        sys.executable,
        str((root / "scripts" /
             "collect_target_machine_installed_evidence.py").resolve()),
        "--root", str(root), "--build", str(build),
        "--output-dir", str(output), "--owner", owner,
    ]


def run_install(build: Path, prefix: Path) -> tuple[int, list[str], str]:
    command = ["cmake", "--install", str(build), "--prefix", str(prefix)]
    try:
        process = subprocess.run(
            command, cwd=build, check=False, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=900,
        )
        return process.returncode, command, process.stdout + process.stderr
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout.decode("utf-8", errors="replace") \
            if isinstance(error.stdout, bytes) else (error.stdout or "")
        stderr = error.stderr.decode("utf-8", errors="replace") \
            if isinstance(error.stderr, bytes) else (error.stderr or "")
        return 124, command, stdout + stderr + "\ninstall timed out\n"
    except OSError as error:
        return 127, command, f"install could not start: {error}\n"


def installed_files(prefix: Path) -> list[Path]:
    return sorted(
        (path for path in prefix.rglob("*") if path.is_file()),
        key=lambda path: path.relative_to(prefix).as_posix(),
    )


def tree_inventory(prefix: Path) -> tuple[list[dict[str, Any]], str, list[str]]:
    rows: list[dict[str, Any]] = []
    hazards: list[str] = []
    if not prefix.is_dir():
        return rows, hashlib.sha256(b"").hexdigest(), ["install root was not created"]
    for path in sorted(prefix.rglob("*"), key=lambda item: item.relative_to(prefix).as_posix()):
        relative = path.relative_to(prefix).as_posix()
        if path.is_symlink():
            hazards.append(f"{relative}: symbolic link is not retained as a regular file")
            continue
        if path.is_file():
            rows.append({
                "path": relative,
                "sha256": assembler.sha256_file(path),
                "size": path.stat().st_size,
            })
    framed = hashlib.sha256()
    for row in rows:
        framed.update(str(row["path"]).encode("utf-8"))
        framed.update(b"\0")
        framed.update(str(row["sha256"]).encode("ascii"))
        framed.update(b"\0")
        framed.update(str(row["size"]).encode("ascii"))
        framed.update(b"\0")
    return rows, framed.hexdigest(), hazards


def discover_deliverables(prefix: Path,
                          required: list[str]) -> tuple[list[str], list[str], dict[str, list[str]]]:
    present: list[str] = []
    absent: list[str] = []
    paths: dict[str, list[str]] = {}
    for name in required:
        matches: list[Path] = []
        for pattern in DELIVERABLE_PATTERNS.get(name, ()):
            matches.extend(path for path in prefix.glob(pattern) if path.is_file())
        relative = sorted({path.relative_to(prefix).as_posix() for path in matches})
        paths[name] = relative
        if relative:
            present.append(name)
        else:
            absent.append(name)
    return present, absent, paths


def discover_headers(prefix: Path,
                     required: list[str]) -> tuple[list[str], list[str]]:
    present = [relative for relative in required if (prefix / relative).is_file()]
    return present, [relative for relative in required if relative not in present]


def residue_scan(prefix: Path, governance: dict[str, Any]) -> tuple[int, list[str]]:
    path_regex = re.compile(governance["installed"]["forbidden_path_regex"])
    text_regex = re.compile(governance["installed"]["forbidden_text_regex"])
    count = 0
    samples: list[str] = []
    for path in installed_files(prefix):
        relative = path.relative_to(prefix).as_posix()
        if path_regex.search(relative):
            count += 1
            samples.append(f"{relative}: path")
        text = completion.readable_text(path)
        if text is None:
            continue
        for match in text_regex.finditer(text):
            count += 1
            samples.append(f"{relative}: {match.group(0)}")
    return count, samples


def payload_identity(prefix: Path, identity: dict[str, str]) -> tuple[bool, str]:
    path = prefix / "share/xray/install/payload-manifest.json"
    if not path.is_file():
        return False, "installed payload manifest is missing"
    try:
        payload = assembler.read_object(path)
    except assembler.AssemblyError as error:
        return False, str(error)
    commit = payload.get("commit")
    dirty = payload.get("dirty")
    if commit != identity["source_commit"] or dirty is not False:
        return False, f"payload commit={commit!r} dirty={dirty!r}"
    return True, f"payload commit={commit} dirty=false"


def write_log(path: Path, title: str, command: list[str], code: int,
              output: str) -> None:
    path.write_text(
        f"{title}\nargv={json.dumps(command, ensure_ascii=False)}\n"
        f"exit_code={code}\n{output}", encoding="utf-8",
    )


def collect(root: Path, build: Path, output: Path, owner: str) -> int:
    governance = assembler.read_object(root / completion.DEFAULT_MANIFEST)
    if completion.validate_manifest(governance):
        raise CollectionError("completion governance manifest is invalid")
    compiler = require_ninja_build(build)
    identity = repository_identity(root)
    governance_hash = governance["input_identity"]["sha256"]
    actual_governance = completion.framed_tree_hash(
        root, governance["input_identity"]["files"]
    )
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
        raise CollectionError("another installed collection owns the output") from error
    staging: Path | None = None
    try:
        staging = Path(tempfile.mkdtemp(
            prefix=f".{output.name}-staging-", dir=output.parent
        ))
        logs = staging / "logs"
        logs.mkdir()
        prefix = staging / "install"
        empty_code, empty_argv, empty_text = run_install(build, prefix)
        empty_rows, empty_hash, empty_hazards = tree_inventory(prefix)
        replay_code, replay_argv, replay_text = run_install(build, prefix)
        replay_rows, replay_hash, replay_hazards = tree_inventory(prefix)
        no_work = empty_rows == replay_rows and empty_hash == replay_hash

        required = governance["installed"]["required_deliverables"]
        if set(required) != set(DELIVERABLE_PATTERNS):
            raise CollectionError("installed deliverable authority is not exact")
        deliverables, absent_deliverables, deliverable_paths = discover_deliverables(
            prefix, required
        )
        required_headers = governance["installed"]["required_public_headers"]
        public_headers, absent_headers = discover_headers(prefix, required_headers)
        residue_count, residue_samples = residue_scan(prefix, governance)
        payload_exact, payload_detail = payload_identity(prefix, identity)
        hazards = empty_hazards + replay_hazards

        empty_log = logs / "empty-stage-install.log"
        replay_log = logs / "no-work-replay.log"
        inventory_log = logs / "installed-inventory.log"
        write_log(empty_log, "empty-stage install", empty_argv, empty_code, empty_text)
        write_log(replay_log, "no-work replay", replay_argv, replay_code, replay_text)
        inventory_lines = [
            f"published_install_root={(output / 'install').resolve()}",
            f"empty_tree_sha256={empty_hash}",
            f"replay_tree_sha256={replay_hash}",
            f"file_count={len(replay_rows)}",
            f"absent_deliverables={','.join(absent_deliverables)}",
            f"absent_public_headers={','.join(absent_headers)}",
            f"residue_count={residue_count}",
            f"payload_identity={payload_detail}",
            *[f"hazard={value}" for value in hazards],
            *[f"residue={value}" for value in residue_samples],
            *[
                f"file={row['path']} size={row['size']} sha256={row['sha256']}"
                for row in replay_rows
            ],
        ]
        inventory_log.write_text("\n".join(inventory_lines) + "\n", encoding="utf-8")

        passed = (
            actual_governance == governance_hash
            and empty_code == 0 and replay_code == 0 and bool(replay_rows)
            and no_work and not hazards and not absent_deliverables
            and not absent_headers and residue_count == 0 and payload_exact
        )
        status = "passed" if passed else "failed"
        exit_code = 0 if passed else 1
        log_results = (
            empty_code == 0,
            replay_code == 0 and no_work,
            not hazards and not absent_deliverables and not absent_headers
            and residue_count == 0 and payload_exact
            and actual_governance == governance_hash,
        )
        raw_logs: list[dict[str, Any]] = []
        for relative, path, ok in (
            ("logs/empty-stage-install.log", empty_log, log_results[0]),
            ("logs/no-work-replay.log", replay_log, log_results[1]),
            ("logs/installed-inventory.log", inventory_log, log_results[2]),
        ):
            digest = assembler.sha256_file(path)
            raw_logs.append({
                "path": relative, "sha256": digest,
                "identity_sha256": assembler.raw_log_identity(
                    KIND, identity["source_commit"], identity["repository_sha256"],
                    governance_hash, relative, digest, owner, generated_at,
                    command, platform, exit_code, status,
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
            "command": command, "platform": platform,
            "payload": {
                "producer": PRODUCER,
                "empty_stage_replay": "passed" if empty_code == 0 else "failed",
                "no_work_replay": "passed" if replay_code == 0 and no_work else "failed",
                "deliverables": deliverables,
                "public_headers": public_headers,
                "install_root": str((output / "install").resolve()),
                "inventory_log": "logs/installed-inventory.log",
                "empty_stage_log": "logs/empty-stage-install.log",
                "no_work_log": "logs/no-work-replay.log",
                "empty_tree_sha256": empty_hash,
                "installed_tree_sha256": replay_hash,
                "installed_file_count": len(replay_rows),
                "deliverable_paths": deliverable_paths,
                "absent_deliverables": absent_deliverables,
                "absent_public_headers": absent_headers,
                "residue_count": residue_count,
                "residue_samples": residue_samples,
                "payload_identity": payload_detail,
            },
            "logs": raw_logs,
        }
        assembler.write_object(staging / "installed.raw.json", raw)
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
        print(f"target-machine installed evidence: ERROR: {error}", file=sys.stderr)
        return 2
    result = "PASS" if code == 0 else "FAIL"
    print(f"target-machine installed evidence: {result}: {output}")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
