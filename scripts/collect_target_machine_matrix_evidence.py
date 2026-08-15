#!/usr/bin/env python3
"""Collect identity-bound raw validation-matrix evidence."""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import platform as host_platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath
from typing import Any

import assemble_target_machine_completion_evidence as assembler
import check_target_machine_completion as completion


PRODUCER = "target-machine-matrix-evidence/1"
KIND = "matrix"
ROW_RESULT_SCHEMA = 2
ROW_RESULT_FIELDS = {
    "schema", "row_id", "status", "source_commit", "repository_sha256",
    "governance_input_sha256", "policy_sha256", "command", "platform",
    "target", "provider", "artifact_route", "executor_or_generation",
    "build_or_sanitizer",
    "exit_code", "positive_activation", "negative_mismatch",
    "artifact_retention", "artifact_fingerprint", "binary_fingerprint",
    "artifact", "artifact_sha256", "binary", "binary_sha256",
    "source_fingerprint", "last_verified", "log", "log_sha256",
    "normalized_log_sha256", "identity_sha256",
}
ROUTES = {
    "source": ("source-to-xsm", "xsm-to-xtp", "xtp-to-vm"),
    "native": ("hosted-fragment", "target-plan-to-native"),
    "source+native": (
        "generation-lifecycle", "hosted-fragment", "runtime-only-embed",
        "source-to-xsm", "target-plan-to-native", "xsm-to-xtp", "xtp-to-vm",
    ),
}


class CollectionError(ValueError):
    pass


def repository_identity(root: Path) -> dict[str, str]:
    identity, findings = completion.repository_identity(root)
    if findings:
        detail = "; ".join(row.message for row in findings)
        raise CollectionError(f"source identity is not collectable: {detail}")
    return identity


def platform_identity() -> dict[str, str]:
    return {
        "os": host_platform.system() or "unknown-os",
        "arch": host_platform.machine() or "unknown-arch",
        "toolchain": os.environ.get("CC", "host-default"),
    }


def local_target() -> str:
    system = host_platform.system().lower()
    machine = host_platform.machine().lower()
    if system == "windows" and machine in {"amd64", "x86_64"}:
        return "windows-x86_64"
    if system == "darwin" and machine in {"arm64", "aarch64"}:
        return "macos-arm64"
    if system == "linux" and machine in {"amd64", "x86_64"}:
        return "linux-x86_64"
    return f"{system}-{machine}"


def canonical_command(root: Path, results: Path, output: Path,
                      owner: str) -> list[str]:
    return [
        sys.executable,
        str((root / "scripts" /
             "collect_target_machine_matrix_evidence.py").resolve()),
        "--root", str(root), "--row-results-dir", str(results),
        "--output-dir", str(output), "--owner", owner,
    ]


def sha256_json(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=False, allow_nan=False).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def row_result_identity(row: dict[str, Any]) -> str:
    return sha256_json({key: row[key] for key in sorted(ROW_RESULT_FIELDS)
                        if key != "identity_sha256"})


def retained_path(root: Path, value: Any, label: str) -> Path:
    if not isinstance(value, str) or not value or "\\" in value:
        raise CollectionError(f"{label} is not a canonical relative POSIX path")
    relative = PurePosixPath(value)
    if (relative.is_absolute() or relative.as_posix() != value
            or any(part in {"", ".", ".."} for part in relative.parts)):
        raise CollectionError(f"{label} is not a canonical relative POSIX path")
    candidate = (root / value).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError as error:
        raise CollectionError(f"{label} escapes the result root") from error
    return candidate


def exact_row_command(command: Any) -> list[str]:
    if not isinstance(command, str) or not command.strip():
        raise CollectionError("matrix row command is empty")
    if command.startswith("CI:"):
        return [command]
    try:
        argv = shlex.split(command, posix=True)
    except ValueError as error:
        raise CollectionError(f"matrix row command is malformed: {error}") from error
    if not argv or any(not token for token in argv):
        raise CollectionError("matrix row command has no exact argv")
    return argv


def executable_on_host(row: dict[str, Any]) -> bool:
    return (row.get("target") == local_target()
            and not str(row.get("command", "")).startswith("CI:"))


def run_exact_command(argv: list[str], root: Path,
                      timeout_seconds: int) -> tuple[int, str]:
    try:
        result = subprocess.run(
            argv, cwd=root, check=False, capture_output=True, text=True,
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
        return 124, stdout + stderr + "\nmatrix command timed out\n"
    except OSError as error:
        return 127, f"cannot execute matrix command: {error}\n"


def canonical_command_log(argv: list[str], text: str) -> str:
    """Remove only CTest-owned path and elapsed-time fields from a transcript."""
    normalized = text.replace("\r\n", "\n").replace("\r", "\n")
    executable = Path(argv[0]).name.lower() if argv else ""
    if executable not in {"ctest", "ctest.exe"}:
        return normalized
    lines = normalized.splitlines(keepends=True)
    project_lines = [line for line in lines if line.startswith("Test project ")]
    total_lines = [line for line in lines
                   if line.startswith("Total Test time (real) =")]
    if len(project_lines) != 1 or len(total_lines) != 1:
        raise CollectionError(
            "CTest transcript requires one project field and one total elapsed field"
        )
    start_rows: list[int] = []
    start_indices: list[int] = []
    result_rows: list[tuple[int, int, int]] = []
    result_indices: list[int] = []
    summary_rows: list[tuple[int, int]] = []
    project_index = -1
    summary_index = -1
    total_index = -1
    canonical: list[str] = []
    for index, line in enumerate(lines):
        ending = "\n" if line.endswith("\n") else ""
        body = line[:-1] if ending else line
        if body.startswith("Test project "):
            if not body.removeprefix("Test project ").strip():
                raise CollectionError("CTest project field is empty")
            project_index = index
            body = "Test project <build>"
        elif (start := re.fullmatch(
                r"\s*Start\s+(\d+):\s+\S(?:.*\S)?\s*", body)) is not None:
            start_rows.append(int(start.group(1)))
            start_indices.append(index)
        elif (result := re.fullmatch(
                r"\s*(\d+)/(\d+)\s+Test\s+#(\d+):\s+.*\S\s+"
                r"(?:Passed|\*\*\*Failed|Not Run)\s+"
                r"(?:<\s*)?\d+(?:\.\d+)?\s+sec", body)) is not None:
            result_rows.append(tuple(int(result.group(i)) for i in range(1, 4)))
            result_indices.append(index)
            body = re.sub(
                r"\s+(?:<\s*)?\d+(?:\.\d+)?\s+sec$", " <elapsed>", body,
            )
        elif body.startswith("Total Test time (real) ="):
            if re.fullmatch(
                    r"Total Test time \(real\) =\s+(?:<\s*)?"
                    r"\d+(?:\.\d+)?\s+sec", body) is None:
                raise CollectionError("CTest total elapsed field is not canonical")
            total_index = index
            body = "Total Test time (real) = <elapsed>"
        elif "sec*proc" in body:
            label = re.fullmatch(
                r"(?P<label>\s*\S(?:.*\S)?)\s*=\s+"
                r"(?:<\s*)?\d+(?:\.\d+)?\s+sec\*proc\s+"
                r"\((?P<count>\d+)\s+(?P<unit>tests?)\)", body,
            )
            if label is None:
                raise CollectionError("CTest label elapsed field is not canonical")
            body = (f"{label.group('label')} = <elapsed>*proc "
                    f"({label.group('count')} {label.group('unit')})")
        elif (summary := re.fullmatch(
                r"(\d+)% tests passed, (\d+) tests failed out of (\d+)",
                body)) is not None:
            summary_rows.append((int(summary.group(2)), int(summary.group(3))))
            summary_index = index
        elif re.match(r"^\s*\d+/\d+\s+Test\s+#\d+:", body):
            raise CollectionError("CTest result row has a noncanonical elapsed field")
        elif re.search(r"(?:<\s*)?\d+(?:\.\d+)?\s+(?:ms|sec|seconds?)\b", body):
            raise CollectionError("CTest transcript has an unclassified volatile field")
        canonical.append(body + ending)
    if not start_rows or len(start_rows) != len(result_rows):
        raise CollectionError("CTest transcript has incomplete start/result framing")
    expected_count = len(start_rows)
    if start_rows != [row[2] for row in result_rows]:
        raise CollectionError("CTest start/result case order differs")
    if [row[0] for row in result_rows] != list(range(1, expected_count + 1)) \
            or any(row[1] != expected_count for row in result_rows):
        raise CollectionError("CTest result row numbering is not complete")
    if len(summary_rows) != 1 or summary_rows[0] != (0, expected_count):
        raise CollectionError("CTest success summary is missing or inconsistent")
    framing = [project_index, *(
        position
        for pair in zip(start_indices, result_indices)
        for position in pair
    ), summary_index, total_index]
    if framing != sorted(framing) or len(set(framing)) != len(framing):
        raise CollectionError("CTest transcript framing order is not canonical")
    return "".join(canonical)


def command_log_sha256(argv: list[str], text: str) -> str:
    return hashlib.sha256(canonical_command_log(argv, text).encode("utf-8")).hexdigest()


def validate_policy(policy: dict[str, Any], governance: dict[str, Any]) -> list[dict[str, Any]]:
    rows = policy.get("rows")
    if policy.get("schema") != 1 or not isinstance(rows, list) or not rows:
        raise CollectionError("validation matrix schema/rows are not exact")
    dimensions = governance["matrix"]["required_dimensions"]
    if dimensions != [
        "target", "provider", "artifact_route", "executor_or_generation",
        "build_or_sanitizer",
    ]:
        raise CollectionError("matrix dimensions are not the governed exact catalog")
    seen: set[str] = set()
    qualifying = set(governance["matrix"]["qualifying_tiers"])
    covered_routes: set[str] = set()
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise CollectionError(f"matrix row {index} is malformed")
        row_id = row.get("id")
        if not isinstance(row_id, str) or not row_id or row_id in seen:
            raise CollectionError("matrix row IDs are missing or duplicate")
        seen.add(row_id)
        for field in (*dimensions, "support_tier", "command", "owner", "oracle",
                      "baseline", "timeout_seconds"):
            if row.get(field) in (None, ""):
                raise CollectionError(f"{row_id} lacks policy field {field}")
        exact_row_command(row["command"])
        if row["artifact_route"] not in ROUTES:
            if row["support_tier"] in qualifying:
                raise CollectionError(f"{row_id} has no governed artifact-route derivation")
        elif row["support_tier"] in qualifying:
            covered_routes.update(ROUTES[row["artifact_route"]])
    required = set(governance["matrix"]["required_artifact_routes"])
    if covered_routes != required:
        raise CollectionError("qualifying matrix rows do not derive the exact route catalog")
    return rows


def load_result(results: Path, row: dict[str, Any], identity: dict[str, str],
                governance_hash: str, policy_hash: str,
                baseline_hash: str) -> tuple[dict[str, Any] | None, str]:
    path = results / f"{row['id']}.json"
    if not path.is_file():
        return None, "independent row result is missing"
    try:
        result = assembler.read_object(path)
    except (assembler.AssemblyError, OSError, ValueError) as error:
        return None, f"independent row result is unreadable: {error}"
    if set(result) != ROW_RESULT_FIELDS or result.get("schema") != ROW_RESULT_SCHEMA:
        return None, "independent row result fields/schema are not exact"
    expected = {
        "row_id": row["id"],
        "source_commit": identity["source_commit"],
        "repository_sha256": identity["repository_sha256"],
        "governance_input_sha256": governance_hash,
        "policy_sha256": policy_hash,
        "command": exact_row_command(row["command"]),
        "target": row["target"], "provider": row["provider"],
        "artifact_route": row["artifact_route"],
        "executor_or_generation": row["executor_or_generation"],
        "build_or_sanitizer": row["build_or_sanitizer"],
        "status": "passed", "exit_code": 0,
        "positive_activation": "activated-and-passed",
        "negative_mismatch": "rejected-before-activation",
        "artifact_retention": "retained",
        "source_fingerprint": identity["repository_sha256"],
    }
    for field, value in expected.items():
        if result.get(field) != value:
            return None, f"independent row result changed {field}"
    for field in ("artifact_fingerprint", "binary_fingerprint"):
        if not completion.exact_sha256(result.get(field)):
            return None, f"independent row result lacks {field}"
    platform = result.get("platform")
    if (not isinstance(platform, dict) or set(platform) != {"os", "arch", "toolchain"}
            or any(not isinstance(value, str) or not value for value in platform.values())):
        return None, "independent row platform is incomplete"
    if result.get("identity_sha256") != row_result_identity(result):
        return None, "independent row result identity is stale"
    log = retained_path(results, result.get("log"), f"{row['id']} result log")
    if (not log.is_file() or not completion.exact_sha256(result.get("log_sha256"))
            or assembler.sha256_file(log) != result["log_sha256"]):
        return None, "independent row retained log is missing or stale"
    try:
        log_text = log.read_text(encoding="utf-8", errors="strict")
        normalized_log_sha256 = command_log_sha256(
            exact_row_command(row["command"]), log_text
        )
    except (CollectionError, UnicodeError) as error:
        return None, f"independent row retained log is not canonical: {error}"
    if (not completion.exact_sha256(result.get("normalized_log_sha256"))
            or result["normalized_log_sha256"] != normalized_log_sha256):
        return None, "independent row normalized log identity is stale"
    artifact = retained_path(results, result.get("artifact"),
                             f"{row['id']} artifact")
    binary = retained_path(results, result.get("binary"), f"{row['id']} binary")
    retained = ((artifact, "artifact_sha256", "artifact_fingerprint"),
                (binary, "binary_sha256", "binary_fingerprint"))
    for retained_file, digest_field, fingerprint_field in retained:
        digest = result.get(digest_field)
        if (not retained_file.is_file() or not completion.exact_sha256(digest)
                or assembler.sha256_file(retained_file) != digest
                or result[fingerprint_field] != digest):
            return None, f"independent row {fingerprint_field} is not retained exactly"
    try:
        verified_at = datetime.datetime.fromisoformat(
            str(result.get("last_verified", "")).replace("Z", "+00:00")
        )
    except ValueError:
        verified_at = None
    if (verified_at is None or verified_at.tzinfo is None
            or verified_at.utcoffset() is None):
        return None, "independent row verification time is missing"
    if not completion.exact_sha256(baseline_hash):
        return None, "policy baseline fingerprint is unavailable"
    return result, ""


def collect(root: Path, results: Path, output: Path, owner: str) -> int:
    governance = assembler.read_object(root / completion.DEFAULT_MANIFEST)
    if completion.validate_manifest(governance):
        raise CollectionError("completion governance manifest is invalid")
    identity = repository_identity(root)
    governance_hash = governance["input_identity"]["sha256"]
    actual_governance = completion.framed_tree_hash(
        root, governance["input_identity"]["files"]
    )
    policy_path = root / governance["matrix"]["policy"]
    policy = assembler.read_object(policy_path)
    rows = validate_policy(policy, governance)
    policy_hash = assembler.sha256_file(policy_path)
    generated_at = datetime.datetime.now(datetime.timezone.utc).isoformat().replace(
        "+00:00", "Z"
    )
    command = canonical_command(root, results, output, owner)
    platform = platform_identity()
    if output.exists() or output.is_symlink():
        raise CollectionError("raw evidence package already exists; collection never overwrites")
    output.parent.mkdir(parents=True, exist_ok=True)
    lock = output.with_name(f".{output.name}.collect-lock")
    try:
        lock.mkdir()
    except FileExistsError as error:
        raise CollectionError("another matrix collection owns the output") from error
    staging: Path | None = None
    try:
        staging = Path(tempfile.mkdtemp(prefix=f".{output.name}-staging-",
                                        dir=output.parent))
        (staging / "logs").mkdir()
        evidence_rows: list[dict[str, Any]] = []
        raw_logs: list[dict[str, Any]] = []
        qualifying = set(governance["matrix"]["qualifying_tiers"])
        all_ok = actual_governance == governance_hash
        for row in rows:
            dimensions = {name: row[name]
                          for name in governance["matrix"]["required_dimensions"]}
            if row["support_tier"] not in qualifying:
                evidence_rows.append({
                    "id": row["id"], **dimensions, "status": "unsupported",
                    "owner": row["owner"],
                    "reason": f"policy support tier {row['support_tier']} is not qualifying",
                })
                continue
            baseline = retained_path(root, row["baseline"], f"{row['id']} baseline")
            baseline_hash = (assembler.sha256_file(baseline)
                             if baseline.is_file() else "")
            result, failure = load_result(
                results, row, identity, governance_hash, policy_hash, baseline_hash
            )
            execution_code: int | None = None
            execution_text = ""
            if executable_on_host(row):
                execution_code, execution_text = run_exact_command(
                    exact_row_command(row["command"]), root, int(row["timeout_seconds"])
                )
                if execution_code != 0:
                    failure = f"local exact command exited {execution_code}"
                    result = None
                elif result is not None:
                    row_command = exact_row_command(row["command"])
                    try:
                        execution_digest = command_log_sha256(
                            row_command, execution_text
                        )
                    except CollectionError as error:
                        failure = f"local CTest transcript is not canonical: {error}"
                        result = None
                    else:
                        if result["normalized_log_sha256"] != execution_digest:
                            failure = (
                                "local exact command outcome differs from the "
                                "independent result"
                            )
                            result = None
            row_ok = result is not None
            all_ok = all_ok and row_ok
            relative = f"logs/{row['id']}.log"
            log = staging / relative
            if row_ok:
                source = retained_path(results, result["log"], f"{row['id']} result log")
                shutil.copyfile(source, log)
            else:
                log.write_text(
                    f"row={row['id']}\nresult=failed\nreason={failure}\n"
                    f"local_execution_exit={execution_code}\n{execution_text}",
                    encoding="utf-8",
                )
            digest = assembler.sha256_file(log)
            raw_logs.append({"path": relative, "sha256": digest,
                             "identity_sha256": "",
                             "result": "passed" if row_ok else "failed"})
            retained_evidence: dict[str, str] = {}
            if row_ok:
                for label in ("artifact", "binary"):
                    source = retained_path(results, result[label],
                                           f"{row['id']} {label}")
                    retained_relative = f"logs/{row['id']}-{label}.bin"
                    retained_file = staging / retained_relative
                    shutil.copyfile(source, retained_file)
                    retained_digest = assembler.sha256_file(retained_file)
                    raw_logs.append({
                        "path": retained_relative, "sha256": retained_digest,
                        "identity_sha256": "", "result": "passed",
                    })
                    retained_evidence[f"{label}_evidence"] = retained_relative
            evidence = {
                "id": row["id"], **dimensions,
                "status": "passed" if row_ok else "failed",
                "owner": row["owner"], "oracle": row["oracle"],
                "performance_policy": f"exit within {row['timeout_seconds']} seconds",
                "positive_activation": ("activated-and-passed" if row_ok else "failed"),
                "negative_mismatch": ("rejected-before-activation" if row_ok else "failed"),
                "artifact_retention": "retained",
                "artifact_fingerprint": (result["artifact_fingerprint"]
                                         if row_ok else ""),
                "baseline_fingerprint": baseline_hash,
                "binary_fingerprint": (result["binary_fingerprint"]
                                       if row_ok else ""),
                "manifest_fingerprint": policy_hash,
                "source_fingerprint": identity["repository_sha256"],
                "last_verified": result["last_verified"] if row_ok else generated_at,
                "log": relative,
                "raw_log_sha256": digest,
                "normalized_log_sha256": (result["normalized_log_sha256"]
                                            if row_ok else ""),
                "artifact_routes": list(ROUTES[row["artifact_route"]]),
                **retained_evidence,
            }
            if not row_ok:
                evidence["reason"] = failure
            evidence_rows.append(evidence)
        status = "passed" if all_ok else "failed"
        exit_code = 0 if all_ok else 1
        for log in raw_logs:
            log["identity_sha256"] = assembler.raw_log_identity(
                KIND, identity["source_commit"], identity["repository_sha256"],
                governance_hash, log["path"], log["sha256"], owner,
                generated_at, command, platform, exit_code, status,
            )
        axis_catalog = {
            name: sorted({str(row[name]) for row in rows})
            for name in governance["matrix"]["required_dimensions"]
        }
        raw = {
            "schema": assembler.RAW_SCHEMA, "kind": KIND,
            "status": status, "exit_code": exit_code,
            "source_commit": identity["source_commit"],
            "repository_sha256": identity["repository_sha256"],
            "governance_input_sha256": governance_hash,
            "owner": owner, "generated_at": generated_at,
            "command": command, "platform": platform,
            "payload": {"producer": PRODUCER, "axis_catalog": axis_catalog,
                        "rows": evidence_rows},
            "logs": raw_logs,
        }
        assembler.write_object(staging / "matrix.raw.json", raw)
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
    parser.add_argument("--row-results-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--owner", required=True)
    args = parser.parse_args()
    root = Path(args.root).resolve()
    results = Path(args.row_results_dir).resolve()
    output = Path(args.output_dir).resolve()
    try:
        code = collect(root, results, output, args.owner)
    except (CollectionError, OSError, KeyError, TypeError, ValueError) as error:
        print(f"target-machine matrix evidence: ERROR: {error}", file=sys.stderr)
        return 2
    print(f"target-machine matrix evidence: {'PASS' if code == 0 else 'FAIL'}: {output}")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
