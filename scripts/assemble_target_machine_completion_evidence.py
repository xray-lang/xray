#!/usr/bin/env python3
"""Assemble terminal target-machine evidence from identity-bound raw lane records."""

from __future__ import annotations

import argparse
import copy
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath
from typing import Any

import check_target_machine_completion as completion


ASSEMBLER = "target-machine-completion-evidence-assembler/1"
SCHEMA = 1
RAW_SCHEMA = 1
FORBIDDEN_KEYS = {
    "allow", "allowlist", "compat", "compatibility", "fallback", "skip",
    "skip_reason", "skipped", "waiver",
}
BUNDLE_FIELDS = {
    "schema", "assembler", "source_commit", "repository_sha256",
    "governance_input_sha256", "lanes",
}
RAW_FIELDS = {
    "schema", "kind", "status", "exit_code", "source_commit",
    "repository_sha256", "governance_input_sha256", "owner",
    "generated_at", "command", "platform", "payload", "logs",
}
RAW_LOG_FIELDS = {"path", "sha256", "identity_sha256", "result"}


class AssemblyError(ValueError):
    pass


def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise AssemblyError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def reject_constant(value: str) -> None:
    raise AssemblyError(f"non-finite JSON number: {value}")


def read_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(
            path.read_text(encoding="utf-8", errors="strict"),
            object_pairs_hook=unique_object,
            parse_constant=reject_constant,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise AssemblyError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise AssemblyError(f"{path} must contain a JSON object")
    return value


def write_object(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True,
                               allow_nan=False) + "\n",
                    encoding="utf-8")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def contains_forbidden_control(value: Any, prefix: str = "") -> list[str]:
    matches: list[str] = []
    if isinstance(value, dict):
        for key, child in value.items():
            normalized = str(key).strip().lower().replace("-", "_")
            if normalized in FORBIDDEN_KEYS:
                matches.append(f"{prefix}{key}")
            matches.extend(contains_forbidden_control(child, f"{prefix}{key}."))
    elif isinstance(value, list):
        for index, child in enumerate(value):
            matches.extend(contains_forbidden_control(child, f"{prefix}{index}."))
    return matches


def repository_identity(root: Path, source_commit: str,
                        repository_sha256: str) -> dict[str, str]:
    current, findings = completion.repository_identity(root)
    if findings:
        detail = "; ".join(row.message for row in findings)
        raise AssemblyError(f"current source identity is not publishable: {detail}")
    if not completion.COMMIT_RE.fullmatch(source_commit):
        raise AssemblyError("current source commit is not exact")
    if not completion.exact_sha256(repository_sha256):
        raise AssemblyError("current repository tree hash is not exact")
    expected = {
        "source_commit": source_commit,
        "repository_sha256": repository_sha256,
    }
    if current != expected:
        raise AssemblyError("source commit/tree do not match the requested publication identity")
    return expected


def retained_path(root: Path, value: Any, label: str) -> Path:
    if not isinstance(value, str) or not value or "\\" in value:
        raise AssemblyError(f"{label} must be a canonical relative POSIX path")
    relative = PurePosixPath(value)
    if (relative.is_absolute() or relative.as_posix() != value
            or any(part in {"", ".", ".."} for part in relative.parts)):
        raise AssemblyError(f"{label} must be a canonical relative POSIX path")
    candidate = (root / value).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError as error:
        raise AssemblyError(f"{label} escapes its raw root") from error
    return candidate


def validate_relative_path(value: Any, label: str) -> None:
    retained_path(Path.cwd(), value, label)


def validate_terminal_inventories(root: Path,
                                  governance: dict[str, Any]) -> None:
    findings = completion.terminal_inventory_findings(root, governance)
    if findings:
        detail = "; ".join(row.message for row in findings)
        raise AssemblyError(f"terminal inventories are not zero: {detail}")


def validate_command(row: dict[str, Any], kind: str) -> None:
    if row.get("status") != "passed" or row.get("exit_code") != 0:
        raise AssemblyError(f"{kind} raw lane did not exit successfully")
    if not isinstance(row.get("command"), list) or not row["command"] or any(
        not isinstance(token, str) or not token for token in row["command"]
    ):
        raise AssemblyError(f"{kind} raw lane command is not exact argv")
    platform = row.get("platform")
    required = {"os", "arch", "toolchain"}
    if not isinstance(platform, dict) or set(platform) != required or any(
        not isinstance(platform[name], str) or not platform[name]
        for name in required
    ):
        raise AssemblyError(f"{kind} raw lane platform identity is incomplete")


def validate_timestamp(value: Any, kind: str) -> None:
    if not isinstance(value, str) or not value:
        raise AssemblyError(f"{kind} raw manifest timestamp is invalid")
    try:
        parsed = datetime.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise AssemblyError(f"{kind} raw manifest timestamp is invalid") from error
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise AssemblyError(f"{kind} raw manifest timestamp has no timezone")


def raw_log_identity(kind: str, source_commit: str, repository_sha256: str,
                     governance_sha256: str, relative: str, digest: str,
                     owner: str, generated_at: str, command: list[str],
                     platform: dict[str, str], exit_code: int,
                     status: str) -> str:
    command_json = json.dumps(command, ensure_ascii=False, separators=(",", ":"))
    platform_json = json.dumps(platform, sort_keys=True, separators=(",", ":"))
    framed = "\0".join((kind, source_commit, repository_sha256,
                         governance_sha256, relative, digest, owner,
                         generated_at, command_json, platform_json,
                         str(exit_code), status))
    return hashlib.sha256(framed.encode("utf-8")).hexdigest()


def rewrite_log_references(value: Any, replacements: dict[str, str]) -> Any:
    if isinstance(value, dict):
        return {
            key: rewrite_log_references(child, replacements)
            for key, child in value.items()
        }
    if isinstance(value, list):
        return [rewrite_log_references(child, replacements) for child in value]
    if isinstance(value, str):
        return replacements.get(value, value)
    return value


def validate_raw_manifest(path: Path, raw_root: Path, kind: str,
                          identity: dict[str, str], governance_sha256: str,
                          stage: Path) -> dict[str, Any]:
    manifest_digest = sha256_file(path)
    row = read_object(path)
    if sha256_file(path) != manifest_digest:
        raise AssemblyError(f"{kind} raw manifest changed while it was read")
    if set(row) != RAW_FIELDS:
        raise AssemblyError(f"{kind} raw manifest fields are not exact")
    if row.get("schema") != RAW_SCHEMA or row.get("kind") != kind:
        raise AssemblyError(f"{kind} raw manifest schema/kind is not exact")
    controls = contains_forbidden_control(row)
    if controls:
        raise AssemblyError(f"{kind} raw manifest contains forbidden controls: {controls}")
    for field in ("source_commit", "repository_sha256"):
        if row.get(field) != identity[field]:
            raise AssemblyError(f"{kind} raw manifest {field} is stale")
    if row.get("governance_input_sha256") != governance_sha256:
        raise AssemblyError(f"{kind} raw manifest governance input is stale")
    if not isinstance(row.get("owner"), str) or not row["owner"]:
        raise AssemblyError(f"{kind} raw manifest has no owner")
    validate_timestamp(row.get("generated_at"), kind)
    validate_command(row, kind)
    payload = row.get("payload")
    if not isinstance(payload, dict):
        raise AssemblyError(f"{kind} raw manifest payload is missing")
    if any(key in payload for key in (
        "schema", "kind", "status", "source_commit", "repository_sha256",
        "governance_input_sha256", "owner", "generated_at", "logs",
        "assembly",
    )):
        raise AssemblyError(f"{kind} payload shadows the evidence envelope")
    logs = row.get("logs")
    if not isinstance(logs, list) or not logs:
        raise AssemblyError(f"{kind} raw manifest has no retained logs")
    final_logs: list[dict[str, Any]] = []
    replacements: dict[str, str] = {}
    seen: set[str] = set()
    for index, log in enumerate(logs):
        if not isinstance(log, dict):
            raise AssemblyError(f"{kind} raw log {index} is malformed")
        if set(log) != RAW_LOG_FIELDS:
            raise AssemblyError(f"{kind} raw log {index} fields are not exact")
        relative = log.get("path")
        source = retained_path(raw_root, relative, f"{kind} log path")
        if str(relative) in seen:
            raise AssemblyError(f"{kind} raw log path is duplicated: {relative}")
        seen.add(str(relative))
        if not source.is_file():
            raise AssemblyError(f"{kind} raw log is missing: {relative}")
        digest = log.get("sha256")
        if not completion.exact_sha256(digest) or sha256_file(source) != digest:
            raise AssemblyError(f"{kind} raw log digest is stale: {relative}")
        expected = raw_log_identity(
            kind, identity["source_commit"], identity["repository_sha256"],
            governance_sha256, str(relative), digest, row["owner"],
            row["generated_at"], row["command"], row["platform"],
            row["exit_code"], row["status"],
        )
        if log.get("identity_sha256") != expected or log.get("result") != "passed":
            raise AssemblyError(f"{kind} raw log identity/result is invalid: {relative}")
        destination_relative = f"logs/{kind}/{index:03d}-{digest}.log"
        destination = stage / destination_relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        if sha256_file(destination) != digest:
            raise AssemblyError(f"{kind} copied log digest changed: {relative}")
        final_logs.append({
            "path": destination_relative,
            "sha256": digest,
            "identity_sha256": completion.log_identity(
                kind, identity["source_commit"], identity["repository_sha256"],
                destination_relative, digest,
            ),
            "result": "passed",
        })
        replacements[str(relative)] = destination_relative
    envelope: dict[str, Any] = {
        "schema": completion.EVIDENCE_SCHEMA,
        "kind": kind,
        "status": "passed",
        "source_commit": identity["source_commit"],
        "repository_sha256": identity["repository_sha256"],
        "governance_input_sha256": governance_sha256,
        "owner": row["owner"],
        "generated_at": row["generated_at"],
        "logs": final_logs,
        "assembly": {
            "assembler": ASSEMBLER,
            "raw_manifest_sha256": manifest_digest,
            "command": copy.deepcopy(row["command"]),
            "platform": copy.deepcopy(row["platform"]),
            "exit_code": 0,
        },
    }
    envelope.update(rewrite_log_references(copy.deepcopy(payload), replacements))
    return envelope


def run_final_verifier(root: Path, manifest_path: Path, evidence_root: Path) -> None:
    checker = root / "scripts/check_target_machine_completion.py"
    result = subprocess.run([
        sys.executable, str(checker), "--root", str(root), "--manifest",
        str(manifest_path), "--evidence-root", str(evidence_root), "--json",
    ], cwd=root, check=False, capture_output=True, text=True, encoding="utf-8")
    if result.returncode != 0:
        detail = result.stdout.strip() or result.stderr.strip()
        raise AssemblyError(f"assembled evidence failed final verification: {detail}")


def assemble(root: Path, manifest_path: Path, bundle_path: Path,
             output: Path) -> None:
    governance = read_object(manifest_path)
    manifest_findings = completion.validate_manifest(governance)
    if manifest_findings:
        raise AssemblyError("completion governance manifest is invalid")
    bundle = read_object(bundle_path)
    if set(bundle) != BUNDLE_FIELDS:
        raise AssemblyError("raw bundle fields are not exact")
    if bundle.get("schema") != SCHEMA or bundle.get("assembler") != ASSEMBLER:
        raise AssemblyError("raw bundle schema/assembler identity is not exact")
    controls = contains_forbidden_control(bundle)
    if controls:
        raise AssemblyError(f"raw bundle contains forbidden controls: {controls}")
    identity = repository_identity(
        root, bundle.get("source_commit", ""),
        bundle.get("repository_sha256", ""),
    )
    governance_sha256 = completion.governance_input_sha256(root, governance)
    if bundle.get("governance_input_sha256") != governance_sha256:
        raise AssemblyError("raw bundle governance input is stale")
    validate_terminal_inventories(root, governance)
    lanes = bundle.get("lanes")
    required = governance["evidence"]["required_files"]
    if not isinstance(lanes, dict) or set(lanes) != set(required):
        raise AssemblyError("raw bundle must contain exactly the seven required evidence kinds")
    final_names = list(required.values())
    for name in final_names:
        validate_relative_path(name, "final evidence path")
    if len(final_names) != len(set(final_names)):
        raise AssemblyError("final evidence paths are not unique canonical relative paths")
    lane_paths = list(lanes.values())
    for name in lane_paths:
        validate_relative_path(name, "raw lane manifest path")
    if len(set(lane_paths)) != len(lane_paths):
        raise AssemblyError("raw bundle lane paths must be unique")
    if output.exists() or output.is_symlink():
        raise AssemblyError("output path already exists; evidence publication never overwrites")
    raw_root = bundle_path.parent.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    lock = output.with_name(f".{output.name}.publish-lock")
    try:
        lock.mkdir()
    except FileExistsError as error:
        raise AssemblyError("another evidence publication owns the output path") from error
    temporary: Path | None = None
    try:
        if output.exists() or output.is_symlink():
            raise AssemblyError("output path appeared before evidence staging")
        temporary = Path(tempfile.mkdtemp(prefix=f".{output.name}-staging-",
                                          dir=output.parent))
        for kind, final_name in sorted(required.items()):
            raw_manifest = retained_path(raw_root, lanes[kind],
                                         f"{kind} raw manifest")
            if not raw_manifest.is_file():
                raise AssemblyError(f"{kind} raw manifest is missing")
            final = validate_raw_manifest(
                raw_manifest, raw_root, kind, identity, governance_sha256,
                temporary,
            )
            write_object(temporary / final_name, final)
        run_final_verifier(root, manifest_path, temporary)
        if output.exists() or output.is_symlink():
            raise AssemblyError("output path appeared before evidence publication")
        os.rename(temporary, output)
        temporary = None
    finally:
        if temporary is not None:
            shutil.rmtree(temporary, ignore_errors=True)
        try:
            lock.rmdir()
        except OSError:
            pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    parser.add_argument("--manifest", default=completion.DEFAULT_MANIFEST)
    parser.add_argument("--bundle", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    root = Path(args.root).resolve()
    manifest = Path(args.manifest)
    if not manifest.is_absolute():
        manifest = root / manifest
    bundle = Path(args.bundle).resolve()
    output = Path(args.output).resolve()
    try:
        assemble(root, manifest.resolve(), bundle, output)
    except (AssemblyError, OSError, KeyError, TypeError) as error:
        print(f"target-machine completion evidence assembler: FAIL: {error}",
              file=sys.stderr)
        return 1
    print(f"target-machine completion evidence assembler: PASS: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
