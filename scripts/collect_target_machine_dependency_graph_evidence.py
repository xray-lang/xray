#!/usr/bin/env python3
"""Collect identity-bound raw dependency-graph evidence from a Ninja build."""

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
from typing import Any, Callable

import assemble_target_machine_completion_evidence as assembler
import check_target_machine_completion as completion


PRODUCER = "target-machine-dependency-graph-evidence/1"
KIND = "dependency-graph"
GRAPH_KINDS = tuple(sorted(completion.GRAPH_KINDS))
TARGET_AUTHORITIES = {
    "xray-cli": "xray",
    "libxray-exec-core": "xray_rt_coro",
    "libxray-vm": "xray_vm",
    "libxray-compiler": "xray_compiler",
}


class CollectionError(ValueError):
    pass


def read_json(path: Path) -> dict[str, Any]:
    value = assembler.read_object(path)
    return value


def run_text(arguments: list[str], cwd: Path) -> tuple[int, str]:
    try:
        result = subprocess.run(
            arguments, cwd=cwd, check=False, capture_output=True, text=True,
            encoding="utf-8", errors="replace",
        )
    except OSError as error:
        return 127, f"command could not start: {error}\n"
    return result.returncode, result.stdout + result.stderr


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


def platform_identity(build: Path) -> dict[str, str]:
    code, output = run_text(["cmake", "--version"], build)
    first = output.splitlines()[0].strip() if output.splitlines() else "unavailable"
    toolchain = f"{first}; generator=Ninja"
    if code != 0:
        toolchain = "cmake unavailable; generator=Ninja"
    return {
        "os": host_platform.system() or "unknown-os",
        "arch": host_platform.machine() or "unknown-arch",
        "toolchain": toolchain,
    }


def require_ninja_build(build: Path) -> None:
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        raise CollectionError("dependency evidence requires a configured CMake build")
    text = cache.read_text(encoding="utf-8", errors="replace")
    if "CMAKE_GENERATOR:INTERNAL=Ninja" not in text:
        raise CollectionError("dependency evidence requires the canonical Ninja generator")
    for name in ("build.ninja", "compile_commands.json"):
        if not (build / name).is_file():
            raise CollectionError(f"dependency evidence requires {name}")


def request_cmake_codemodel(root: Path, build: Path) -> tuple[bool, str]:
    query = build / ".cmake/api/v1/query/codemodel-v2"
    resolved_build = build.resolve()
    resolved_query = query.resolve()
    try:
        resolved_query.relative_to(resolved_build)
    except ValueError:
        return False, f"CMake file-api query escapes the build root: {query}\n"
    if query.is_symlink() or (query.exists() and not query.is_file()):
        return False, f"CMake file-api query is not a regular file: {query}\n"
    try:
        query.parent.mkdir(parents=True, exist_ok=True)
        query.touch(exist_ok=True)
    except OSError as error:
        return False, f"cannot create CMake file-api query: {error}\n"
    arguments = ["cmake", "-S", str(root), "-B", str(build)]
    code, output = run_text(arguments, build)
    details = (
        f"file_api_query={query}\n"
        f"refresh_argv={json.dumps(arguments)}\n"
        f"refresh_exit_code={code}\n{output}"
    )
    if code != 0:
        return False, details
    try:
        require_ninja_build(build)
    except CollectionError as error:
        return False, details + f"\npost-refresh build rejection: {error}\n"
    return True, details


def canonical_command(root: Path, build: Path, output: Path,
                      owner: str) -> list[str]:
    return [
        sys.executable, str((root / "scripts" /
                            "collect_target_machine_dependency_graph_evidence.py").resolve()),
        "--root", str(root), "--build", str(build), "--output-dir", str(output),
        "--owner", owner,
    ]


def cmake_reply(build: Path) -> tuple[list[Path], set[str], str]:
    reply = build / ".cmake/api/v1/reply"
    if not reply.is_dir():
        return [], set(), "missing .cmake/api/v1/reply\n"
    indexes = sorted(reply.glob("index-*.json"))
    if not indexes:
        return [], set(), "missing CMake file-api index\n"
    pending = [indexes[-1]]
    seen: set[Path] = set()
    targets: set[str] = set()
    codemodel = False
    errors: list[str] = []
    while pending:
        path = pending.pop()
        resolved = path.resolve()
        try:
            resolved.relative_to(reply.resolve())
        except ValueError:
            errors.append(f"CMake reply reference escapes its root: {path}")
            continue
        if resolved in seen:
            continue
        seen.add(resolved)
        if not resolved.is_file():
            errors.append(f"missing CMake reply object: {resolved}")
            continue
        try:
            value = json.loads(resolved.read_text(encoding="utf-8", errors="strict"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            errors.append(f"cannot parse CMake reply object {resolved}: {error}")
            continue

        def visit(node: Any) -> None:
            nonlocal codemodel
            if isinstance(node, dict):
                if node.get("kind") == "codemodel":
                    codemodel = True
                if isinstance(node.get("name"), str) and isinstance(node.get("jsonFile"), str):
                    targets.add(node["name"])
                reference = node.get("jsonFile")
                if isinstance(reference, str):
                    pending.append(reply / reference)
                for child in node.values():
                    visit(child)
            elif isinstance(node, list):
                for child in node:
                    visit(child)

        visit(value)
    paths = sorted(seen)
    if not codemodel:
        errors.append("CMake file-api reply has no codemodel object")
    details = [
        f"index={indexes[-1].name}", f"objects={len(paths)}",
        f"targets={','.join(sorted(targets))}",
        *[f"object={path.name} sha256={assembler.sha256_file(path)}"
          for path in paths if path.is_file()],
        *errors,
    ]
    return ([] if errors else paths), targets, "\n".join(details) + "\n"


def json_text(path: Path) -> tuple[Any | None, str]:
    if not path.is_file():
        return None, f"missing {path}\n"
    try:
        value = json.loads(path.read_text(encoding="utf-8", errors="strict"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        return None, f"cannot parse {path}: {error}\n"
    return value, f"path={path}\nsha256={assembler.sha256_file(path)}\n"


def scan_legacy(text: str, manifest: dict[str, Any]) -> list[str]:
    patterns = [
        (row["id"], re.compile(row["text_regex"]))
        for row in manifest["residue_scan"]["rules"]
    ]
    hits: list[str] = []
    for rule, pattern in patterns:
        for match in pattern.finditer(text):
            hits.append(f"{rule}:{match.group(0)[:80]}")
    return hits


def build_ninja(build: Path, manifest: dict[str, Any]) -> tuple[bool, int, str]:
    path = build / "build.ninja"
    if not path.is_file():
        return False, 0, f"missing {path}\n"
    text = path.read_text(encoding="utf-8", errors="replace")
    hits = scan_legacy(text, manifest)
    return not hits, len(hits), (
        f"path={path}\nsha256={assembler.sha256_file(path)}\n"
        f"legacy_edges={len(hits)}\n" + "\n".join(hits[:64]) + "\n"
    )


def codemodel(build: Path, manifest: dict[str, Any]) -> tuple[bool, int, str]:
    paths, target_names, details = cmake_reply(build)
    if not paths:
        return False, 0, details
    hits: list[str] = []
    for path in paths:
        hits.extend(scan_legacy(path.read_text(encoding="utf-8", errors="replace"), manifest))
    required = set(manifest["installed"]["required_deliverables"])
    proven = {
        deliverable for deliverable, target in TARGET_AUTHORITIES.items()
        if deliverable in required and target in target_names
    }
    missing = sorted(required - proven)
    return not hits and not missing, len(hits), (
        details + f"proven_deliverables={','.join(sorted(proven))}\n"
        f"missing_deliverables={','.join(missing)}\n"
        f"legacy_edges={len(hits)}\n" + "\n".join(hits[:64]) + "\n"
    )


def compile_commands(build: Path, manifest: dict[str, Any]) -> tuple[bool, int, str]:
    path = build / "compile_commands.json"
    value, details = json_text(path)
    if not isinstance(value, list):
        return False, 0, details + "compile command root is not a list\n"
    hits: list[str] = []
    for row in value:
        if isinstance(row, dict):
            command = str(row.get("command", row.get("arguments", "")))
            file = str(row.get("file", ""))
            hits.extend(scan_legacy(f"{file}\n{command}", manifest))
    return not hits, len(hits), details + f"entries={len(value)}\nlegacy_edges={len(hits)}\n" + "\n".join(hits[:64]) + "\n"


def ctest_json(build: Path, manifest: dict[str, Any]) -> tuple[bool, int, str]:
    code, output = run_text(["ctest", "--test-dir", str(build), "--show-only=json-v1"], build)
    if code != 0:
        return False, 0, output
    try:
        value = json.loads(output)
    except json.JSONDecodeError as error:
        return False, 0, f"ctest JSON parse failed: {error}\n{output}"
    if not isinstance(value, dict) or not isinstance(value.get("tests"), list):
        return False, 0, "ctest JSON lacks a tests array\n"
    hits = scan_legacy(json.dumps(value, sort_keys=True), manifest)
    return not hits, len(hits), f"tests={len(value['tests'])}\nlegacy_edges={len(hits)}\n" + "\n".join(hits[:64]) + "\n"


def include_dependencies(build: Path, manifest: dict[str, Any]) -> tuple[bool, int, str]:
    code, output = run_text(["ninja", "-C", str(build), "-t", "deps"], build)
    if code != 0:
        return False, 0, output
    if not output.strip():
        return False, 0, "ninja dependency database has no records\n"
    hits = scan_legacy(output, manifest)
    records = sum(1 for line in output.splitlines() if line and not line[0].isspace())
    return not hits, len(hits), f"records={records}\nlegacy_edges={len(hits)}\n" + "\n".join(hits[:64]) + "\n"


def install_graph(build: Path, manifest: dict[str, Any]) -> tuple[bool, int, str]:
    paths = sorted(build.rglob("cmake_install.cmake"))
    if not paths:
        return False, 0, "no generated cmake_install.cmake graph exists\n"
    text = "\n".join(path.read_text(encoding="utf-8", errors="replace") for path in paths)
    hits = scan_legacy(text, manifest)
    details = [
        f"scripts={len(paths)}", f"legacy_edges={len(hits)}",
        *[f"path={path} sha256={assembler.sha256_file(path)}" for path in paths],
        *hits[:64],
    ]
    return not hits, len(hits), "\n".join(details) + "\n"


COLLECTORS: dict[str, Callable[[Path, dict[str, Any]], tuple[bool, int, str]]] = {
    "build-ninja": build_ninja,
    "cmake-codemodel": codemodel,
    "compile-commands": compile_commands,
    "ctest-json": ctest_json,
    "include-dependencies": include_dependencies,
    "install-graph": install_graph,
}


def collect(root: Path, build: Path, output: Path, owner: str) -> int:
    governance_path = root / completion.DEFAULT_MANIFEST
    governance = read_json(governance_path)
    manifest_findings = completion.validate_manifest(governance)
    if manifest_findings:
        raise CollectionError("completion governance manifest is invalid")
    require_ninja_build(build)
    file_api_ok, file_api_details = request_cmake_codemodel(root, build)
    identity = repository_identity(root)
    governance_hash = governance["input_identity"]["sha256"]
    actual_governance = completion.framed_tree_hash(
        root, governance["input_identity"]["files"]
    )
    generated_at = datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z")
    command = canonical_command(root, build, output, owner)
    platform = platform_identity(build)
    if output.exists() or output.is_symlink():
        raise CollectionError("raw evidence package already exists; collection never overwrites")
    output.parent.mkdir(parents=True, exist_ok=True)
    lock = output.with_name(f".{output.name}.collect-lock")
    try:
        lock.mkdir()
    except FileExistsError as error:
        raise CollectionError("another dependency-graph collection owns the output") from error
    staging: Path | None = None
    try:
        staging = Path(tempfile.mkdtemp(prefix=f".{output.name}-staging-", dir=output.parent))
        logs_dir = staging / "logs"
        logs_dir.mkdir()
        graphs: dict[str, dict[str, Any]] = {}
        raw_logs: list[dict[str, Any]] = []
        passed = actual_governance == governance_hash and build.is_dir()
        for name in GRAPH_KINDS:
            if name == "cmake-codemodel" and not file_api_ok:
                ok, count, text = False, 0, file_api_details
            else:
                ok, count, text = COLLECTORS[name](build, governance)
                if name == "cmake-codemodel":
                    ok = ok and file_api_ok
                    text = file_api_details + "\n" + text
            relative = f"logs/{name}.log"
            log = staging / relative
            log.write_text(text, encoding="utf-8")
            digest = assembler.sha256_file(log)
            graphs[name] = {
                "status": "passed" if ok else "failed",
                "legacy_edge_count": count,
                "log": relative,
            }
            passed = passed and ok and count == 0
            raw_logs.append({
                "path": relative, "sha256": digest,
                "identity_sha256": "", "result": "passed" if ok else "failed",
            })
        _, observed_targets, _ = cmake_reply(build)
        targets = [
            deliverable for deliverable, target in TARGET_AUTHORITIES.items()
            if deliverable in governance["installed"]["required_deliverables"]
            and target in observed_targets
        ]
        required_targets = set(governance["installed"]["required_deliverables"])
        passed = passed and set(targets) == required_targets
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
            "payload": {
                "producer": PRODUCER, "graphs": graphs,
                "targets": targets,
            },
            "logs": raw_logs,
        }
        assembler.write_object(staging / "dependency-graph.raw.json", raw)
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
        print(f"target-machine dependency-graph evidence: ERROR: {error}", file=sys.stderr)
        return 2
    result = "PASS" if code == 0 else "FAIL"
    print(f"target-machine dependency-graph evidence: {result}: {output}")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
