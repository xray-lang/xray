#!/usr/bin/env python3
"""Require terminal-zero target-machine residue and identity-bound evidence."""

from __future__ import annotations

import argparse
import copy
import dataclasses
import datetime
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Iterable


DEFAULT_MANIFEST = "contracts/target-machine/completion-governance.json"
CHECKER = "target-machine-completion-governance/2"
SCHEMA = 2
EVIDENCE_SCHEMA = 1
MAX_SAMPLES = 8
SHA256_RE = re.compile(r"[0-9a-f]{64}")
COMMIT_RE = re.compile(r"[0-9a-f]{40}")
TEXT_SUFFIXES = {
    "", ".c", ".cc", ".cmake", ".cmd", ".h", ".in", ".json", ".md",
    ".ps1", ".py", ".sh", ".toml", ".txt", ".xr", ".yaml", ".yml",
}
GRAPH_KINDS = {
    "build-ninja", "cmake-codemodel", "compile-commands", "ctest-json",
    "include-dependencies", "install-graph",
}
FORBIDDEN_CONTROL_KEYS = {
    "allow", "allowlist", "compat", "compatibility", "fallback", "skip",
    "skip_reason", "skipped", "waiver",
}


@dataclasses.dataclass(frozen=True)
class Finding:
    code: str
    surface: str
    message: str
    count: int = 1
    samples: tuple[str, ...] = ()

    def as_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


def finding(code: str, surface: str, message: str, count: int = 1,
            samples: Iterable[str] = ()) -> Finding:
    return Finding(code, surface, message, count, tuple(samples)[:MAX_SAMPLES])


def read_json(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8", errors="strict"))
    if not isinstance(data, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return data


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def exact_sha256(value: Any) -> bool:
    return isinstance(value, str) and SHA256_RE.fullmatch(value) is not None


def command(arguments: list[str], cwd: Path) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(arguments, cwd=cwd, check=False, capture_output=True)


def framed_tree_hash(root: Path, relatives: Iterable[str]) -> str:
    digest = hashlib.sha256()
    for relative in sorted(set(relatives)):
        path = root / relative
        if not path.is_file():
            raise FileNotFoundError(relative)
        name = relative.replace("\\", "/").encode("utf-8")
        content = path.read_bytes()
        digest.update(len(name).to_bytes(8, "big"))
        digest.update(name)
        digest.update(len(content).to_bytes(8, "big"))
        digest.update(content)
    return digest.hexdigest()


def git_tracked_paths(root: Path) -> list[str] | None:
    result = command(["git", "ls-files", "-z"], root)
    if result.returncode != 0:
        return None
    return sorted(
        item.decode("utf-8", errors="strict").replace("\\", "/")
        for item in result.stdout.split(b"\0") if item
    )


def repository_identity(root: Path) -> tuple[dict[str, str], list[Finding]]:
    findings: list[Finding] = []
    head = command(["git", "rev-parse", "HEAD"], root)
    if head.returncode != 0:
        return {}, [finding("TM-COMP-IDENTITY-GIT", "identity",
                            "repository identity requires a Git worktree")]
    source_commit = head.stdout.decode("ascii", errors="strict").strip()
    status = command(["git", "status", "--porcelain", "--untracked-files=normal"], root)
    if status.returncode != 0:
        findings.append(finding("TM-COMP-IDENTITY-STATUS", "identity",
                                "cannot inspect repository cleanliness"))
    elif status.stdout:
        samples = status.stdout.decode("utf-8", errors="replace").splitlines()
        findings.append(finding("TM-COMP-IDENTITY-DIRTY", "identity",
                                "completion evidence requires a clean source worktree",
                                len(samples), samples))
    tracked = git_tracked_paths(root)
    if tracked is None:
        findings.append(finding("TM-COMP-IDENTITY-TRACKED", "identity",
                                "cannot enumerate the governed tracked tree"))
        repository_sha256 = ""
    else:
        repository_sha256 = framed_tree_hash(root, tracked)
    return {
        "source_commit": source_commit,
        "repository_sha256": repository_sha256,
    }, findings


def validate_manifest(manifest: dict[str, Any]) -> list[Finding]:
    findings: list[Finding] = []
    if manifest.get("schema") != SCHEMA or manifest.get("checker") != CHECKER:
        findings.append(finding("TM-COMP-MANIFEST-SCHEMA", "contract",
                                "completion manifest schema/checker identity is not exact"))
    forbidden_legacy = {"legacy_floor", "historical_evidence", "historical_text_evidence"}
    present = sorted(forbidden_legacy & set(manifest))
    if present:
        findings.append(finding("TM-COMP-MANIFEST-FLOOR", "contract",
                                "nonzero or historical residue floors are forbidden",
                                len(present), present))
    policy = manifest.get("policy", {})
    expected_policy = {
        "accepted_evidence_status": ["passed", "unsupported"],
        "compatibility_or_fallback": "forbidden",
        "missing_or_unclassified": "error",
        "residue_count": 0,
        "self_certifying_write": "forbidden",
        "skip_or_allowlist": "forbidden",
    }
    if policy != expected_policy:
        findings.append(finding("TM-COMP-MANIFEST-POLICY", "contract",
                                "completion policy must be fail-closed terminal zero"))
    input_identity = manifest.get("input_identity", {})
    files = input_identity.get("files")
    if (input_identity.get("algorithm") != "sha256" or not isinstance(files, list)
            or not files or len(files) != len(set(files))
            or not exact_sha256(input_identity.get("sha256"))):
        findings.append(finding("TM-COMP-MANIFEST-INPUTS", "contract",
                                "governed input identity is incomplete or ambiguous"))
    rules = manifest.get("residue_scan", {}).get("rules")
    if not isinstance(rules, list) or not rules:
        findings.append(finding("TM-COMP-MANIFEST-RULES", "contract",
                                "terminal residue rules are missing"))
    else:
        ids = [row.get("id") for row in rules if isinstance(row, dict)]
        if len(ids) != len(rules) or len(ids) != len(set(ids)):
            findings.append(finding("TM-COMP-MANIFEST-RULE-IDS", "contract",
                                    "terminal residue rule IDs are missing or duplicate"))
        for row in rules:
            try:
                re.compile(row["path_regex"])
                re.compile(row["text_regex"])
            except (KeyError, TypeError, re.error) as error:
                findings.append(finding("TM-COMP-MANIFEST-RULE-REGEX", "contract", str(error)))
    required_evidence = manifest.get("evidence", {}).get("required_files")
    expected_kinds = {
        "activation-generation", "dependency-graph", "full-validation", "installed",
        "matrix", "runtime-reachability", "symbol",
    }
    if not isinstance(required_evidence, dict) or set(required_evidence) != expected_kinds:
        findings.append(finding("TM-COMP-MANIFEST-EVIDENCE", "contract",
                                "required completion evidence kinds are incomplete"))
    return findings


def authority_findings(root: Path, manifest: dict[str, Any]) -> list[Finding]:
    findings: list[Finding] = []
    input_policy = manifest.get("input_identity", {})
    files = input_policy.get("files", [])
    try:
        actual = framed_tree_hash(root, files)
    except (OSError, TypeError) as error:
        findings.append(finding("TM-COMP-INPUT-MISSING", "contract",
                                f"cannot hash governed completion inputs: {error}"))
    else:
        if actual != input_policy.get("sha256"):
            findings.append(finding("TM-COMP-INPUT-DRIFT", "contract",
                                    "governed completion input hash does not match current sources",
                                    samples=(f"expected={input_policy.get('sha256')}",
                                             f"actual={actual}")))
    for row in manifest.get("authorities", []):
        relative = row.get("path", "")
        path = root / relative
        if not path.is_file():
            findings.append(finding("TM-COMP-AUTHORITY-MISSING", "authority",
                                    f"missing authority {row.get('id')}: {relative}"))
            continue
        text = path.read_text(encoding="utf-8", errors="strict")
        missing = [pattern for pattern in row.get("required_regex", [])
                   if re.search(pattern, text, re.MULTILINE) is None]
        if missing:
            findings.append(finding("TM-COMP-AUTHORITY-CONTRACT", "authority",
                                    f"authority {row.get('id')} lost required fail-closed anchors",
                                    len(missing), missing))
    baseline_path = root / "contracts/target-machine/baseline-manifest.json"
    if not baseline_path.is_file():
        findings.append(finding("TM-COMP-BASELINE-MISSING", "baseline",
                                "clean baseline manifest is missing"))
    else:
        baseline = read_json(baseline_path)
        qualification = baseline.get("qualification", {})
        if baseline.get("runner") != "target-machine-baseline/3":
            findings.append(finding("TM-COMP-BASELINE-RUNNER", "baseline",
                                    "baseline runner identity is not exact"))
        if qualification.get("result") != "passed":
            findings.append(finding("TM-COMP-BASELINE-FAILED", "baseline",
                                    "clean correctness/sanitizer/performance baseline has not passed",
                                    samples=(str(qualification.get("reason", "missing reason")),)))
        elif not exact_sha256(qualification.get("evidence_sha256")):
            findings.append(finding("TM-COMP-BASELINE-EVIDENCE", "baseline",
                                    "passed baseline lacks an exact retained evidence digest"))
    return findings


def fallback_discovery(root: Path, roots: Iterable[str]) -> list[str]:
    paths: set[str] = set()
    for relative in roots:
        base = root / relative
        if base.is_file():
            paths.add(relative.replace("\\", "/"))
        elif base.is_dir():
            paths.update(
                path.relative_to(root).as_posix() for path in base.rglob("*") if path.is_file()
            )
    return sorted(paths)


def scan_paths(root: Path, manifest: dict[str, Any]) -> list[str]:
    policy = manifest["residue_scan"]
    roots = policy["roots"]
    tracked = git_tracked_paths(root)
    candidates = tracked if tracked is not None else fallback_discovery(root, roots)
    result = []
    for relative in candidates:
        if any(relative == item or relative.startswith(item.rstrip("/") + "/")
               for item in roots):
            result.append(relative)
    return result


def surface_for(relative: str) -> str:
    if relative.startswith("include/"):
        return "include"
    if (relative == "CMakeLists.txt" or relative.startswith((".github/", "cmake/", "scripts/"))):
        return "build"
    return "source"


def readable_text(path: Path) -> str | None:
    if path.suffix.lower() not in TEXT_SUFFIXES:
        return None
    data = path.read_bytes()
    if b"\0" in data:
        return None
    try:
        return data.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        return None


def source_residue_findings(root: Path, manifest: dict[str, Any]) -> list[Finding]:
    policy = manifest["residue_scan"]
    definitions = set(policy.get("definition_paths", []))
    compiled = [
        (row["id"], re.compile(row["path_regex"]), re.compile(row["text_regex"]))
        for row in policy["rules"]
    ]
    hits: dict[tuple[str, str], list[str]] = {}
    counts: dict[tuple[str, str], int] = {}
    for relative in scan_paths(root, manifest):
        if relative in definitions:
            continue
        path = root / relative
        surface = surface_for(relative)
        text = readable_text(path)
        for rule_id, path_regex, text_regex in compiled:
            key = (rule_id, surface)
            if path_regex.search(relative):
                counts[key] = counts.get(key, 0) + 1
                hits.setdefault(key, []).append(f"{relative}: path")
            if text is None:
                continue
            for match in text_regex.finditer(text):
                counts[key] = counts.get(key, 0) + 1
                if len(hits.setdefault(key, [])) < MAX_SAMPLES:
                    line = text.count("\n", 0, match.start()) + 1
                    token = match.group(0).replace("\n", " ")[:120]
                    hits[key].append(f"{relative}:{line}: {token}")
    return [
        finding(f"TM-COMP-RESIDUE-{rule_id.upper()}", surface,
                f"{rule_id} residue must be terminal zero", count, hits[(rule_id, surface)])
        for (rule_id, surface), count in sorted(counts.items()) if count
    ]


def terminal_inventory_findings(root: Path, manifest: dict[str, Any]) -> list[Finding]:
    findings: list[Finding] = []
    inventories = manifest["inventories"]
    for key, relative in inventories.items():
        path = root / relative
        if not path.is_file():
            findings.append(finding("TM-COMP-INVENTORY-MISSING", "inventory",
                                    f"missing terminal inventory: {relative}"))
            continue
        data = read_json(path)
        if key == "legacy_vm":
            values = {
                "opcodes": len(data.get("opcodes", [])),
                "tagged_frame_sites": len(data.get("tagged_frame_sites", [])),
                "vm_public_api_symbols": len(data.get("vm_public_api_symbols", [])),
                "legacy_artifact_symbols": len(data.get("legacy_artifact_symbols", [])),
            }
            total = sum(values.values())
            declared = int(data.get("opcode_count", 0))
            if declared != values["opcodes"]:
                findings.append(finding("TM-COMP-INVENTORY-COUNT", "inventory",
                                        f"{relative} opcode count does not match its rows",
                                        samples=(f"declared={declared}",
                                                 f"rows={values['opcodes']}")))
            artifact = data.get("artifact", {})
            if artifact.get("extension") or artifact.get("reader") or artifact.get("writer"):
                values["legacy_artifact_owner"] = 1
                total += 1
        elif key == "legacy_product":
            values = {
                "total": int(data.get("total", 0)),
                "owner_count": int(data.get("owner_count", 0)),
            }
            total = values["total"]
        else:
            values = {
                "rows": len(data.get("rows", [])),
                "mixed_representation_types": len(data.get("mixed_representation_types", {})),
            }
            total = sum(values.values())
            declared = int(data.get("row_count", 0))
            if declared != values["rows"]:
                findings.append(finding("TM-COMP-INVENTORY-COUNT", "inventory",
                                        f"{relative} row count does not match its rows",
                                        samples=(f"declared={declared}",
                                                 f"rows={values['rows']}")))
        if total:
            samples = [f"{name}={value}" for name, value in values.items() if value]
            findings.append(finding(f"TM-COMP-INVENTORY-{key.upper().replace('_', '-')}",
                                    "inventory", f"{relative} is a nonzero transitional inventory",
                                    total, samples))
    return findings


def dual_owner_findings(root: Path, manifest: dict[str, Any]) -> list[Finding]:
    relative = manifest["dual_owner"]["inventory"]
    path = root / relative
    if not path.is_file():
        return [finding("TM-COMP-DUAL-OWNER-MISSING", "dual-owner", f"missing {relative}")]
    data = read_json(path)
    rows = data.get("operations")
    if not isinstance(rows, list) or data.get("operation_count") != len(rows):
        return [finding("TM-COMP-DUAL-OWNER-SCHEMA", "dual-owner",
                        "semantic owner inventory row count is invalid")]
    adapter = manifest["dual_owner"]["mechanical_adapter"]
    ids: set[str] = set()
    errors: list[str] = []
    for row in rows:
        if not isinstance(row, dict):
            errors.append("malformed row")
            continue
        operation_id = str(row.get("operation_id", "<missing>"))
        if operation_id in ids:
            errors.append(f"{operation_id}: duplicate")
        ids.add(operation_id)
        owner = row.get("current_shared_owner")
        if not isinstance(owner, str) or not owner or not (root / owner).is_file():
            errors.append(f"{operation_id}: missing canonical source owner")
        if row.get("current_vm_owner") != adapter or row.get("current_aot_owner") != adapter:
            errors.append(f"{operation_id}: executor owns observable semantics")
        if not row.get("independent_oracle"):
            errors.append(f"{operation_id}: missing independent oracle")
    if not errors:
        return []
    return [finding("TM-COMP-DUAL-OWNER", "dual-owner",
                    "observable operations must have one source-backed owner and mechanical adapters",
                    len(errors), errors)]


def forbidden_control_findings(data: Any, kind: str) -> list[Finding]:
    matches: list[str] = []

    def walk(value: Any, prefix: str) -> None:
        if isinstance(value, dict):
            for key, child in value.items():
                normalized = str(key).strip().lower().replace("-", "_")
                if normalized in FORBIDDEN_CONTROL_KEYS:
                    matches.append(f"{prefix}{key}")
                walk(child, f"{prefix}{key}.")
        elif isinstance(value, list):
            for index, child in enumerate(value):
                walk(child, f"{prefix}{index}.")
        elif isinstance(value, str) and value.strip().lower() in {
            "allowed", "allowlisted", "compat", "fallback", "skip", "skipped", "waived"
        }:
            matches.append(prefix.rstrip("."))

    walk(data, "")
    if not matches:
        return []
    return [finding("TM-COMP-EVIDENCE-CONTROL", kind,
                    "completion evidence contains a skip, compatibility, fallback, or waiver control",
                    len(matches), matches)]


def retained_path(evidence_root: Path, relative: Any) -> Path | None:
    if not isinstance(relative, str) or not relative or Path(relative).is_absolute():
        return None
    candidate = (evidence_root / relative).resolve()
    try:
        candidate.relative_to(evidence_root.resolve())
    except ValueError:
        return None
    return candidate


def log_identity(kind: str, source_commit: str, repository_sha256: str,
                 relative: str, digest: str) -> str:
    value = "\0".join((kind, source_commit, repository_sha256, relative, digest))
    return sha256_bytes(value.encode("utf-8"))


def common_evidence_findings(data: dict[str, Any], kind: str, evidence_root: Path,
                             identity: dict[str, str], input_sha256: str) -> tuple[list[Finding], set[str]]:
    findings = forbidden_control_findings(data, kind)
    if data.get("schema") != EVIDENCE_SCHEMA or data.get("kind") != kind:
        findings.append(finding("TM-COMP-EVIDENCE-SCHEMA", kind,
                                f"{kind} evidence schema/kind is not exact"))
    if data.get("status") != "passed":
        findings.append(finding("TM-COMP-EVIDENCE-STATUS", kind,
                                f"{kind} evidence has not passed"))
    for field in ("source_commit", "repository_sha256"):
        if data.get(field) != identity.get(field):
            findings.append(finding("TM-COMP-EVIDENCE-IDENTITY", kind,
                                    f"{kind} evidence {field} is not bound to the current source"))
    if data.get("governance_input_sha256") != input_sha256:
        findings.append(finding("TM-COMP-EVIDENCE-INPUT", kind,
                                f"{kind} evidence is not bound to the governed input hash"))
    if not data.get("owner"):
        findings.append(finding("TM-COMP-EVIDENCE-OWNER", kind,
                                f"{kind} evidence has no accountable owner"))
    try:
        datetime.datetime.fromisoformat(str(data.get("generated_at", "")).replace("Z", "+00:00"))
    except ValueError:
        findings.append(finding("TM-COMP-EVIDENCE-TIME", kind,
                                f"{kind} evidence has no ISO generation timestamp"))
    logs = data.get("logs")
    verified: set[str] = set()
    if not isinstance(logs, list) or not logs:
        findings.append(finding("TM-COMP-EVIDENCE-LOGS", kind,
                                f"{kind} evidence has no retained logs"))
        return findings, verified
    for index, row in enumerate(logs):
        if not isinstance(row, dict):
            findings.append(finding("TM-COMP-EVIDENCE-LOG-ROW", kind,
                                    f"{kind} log row {index} is malformed"))
            continue
        relative = row.get("path")
        path = retained_path(evidence_root, relative)
        digest = row.get("sha256")
        if path is None or not path.is_file():
            findings.append(finding("TM-COMP-EVIDENCE-LOG-MISSING", kind,
                                    f"{kind} retained log is missing: {relative!r}"))
            continue
        if not exact_sha256(digest) or sha256_file(path) != digest:
            findings.append(finding("TM-COMP-EVIDENCE-LOG-DIGEST", kind,
                                    f"{kind} retained log digest does not match: {relative}"))
            continue
        expected = log_identity(kind, identity["source_commit"],
                                identity["repository_sha256"], str(relative), digest)
        if row.get("identity_sha256") != expected or row.get("result") != "passed":
            findings.append(finding("TM-COMP-EVIDENCE-LOG-IDENTITY", kind,
                                    f"{kind} retained log is not identity-bound and passed: {relative}"))
            continue
        verified.add(str(relative))
    return findings, verified


def check_log_reference(findings: list[Finding], kind: str, verified: set[str],
                        value: Any, context: str) -> None:
    if value not in verified:
        findings.append(finding("TM-COMP-EVIDENCE-LOG-REFERENCE", kind,
                                f"{context} does not reference an identity-bound passed log"))


def dependency_findings(data: dict[str, Any], verified: set[str],
                        required_targets: set[str]) -> list[Finding]:
    findings: list[Finding] = []
    graphs = data.get("graphs")
    if not isinstance(graphs, dict) or set(graphs) != GRAPH_KINDS:
        findings.append(finding("TM-COMP-GRAPH-COVERAGE", "dependency-graph",
                                "dependency evidence must cover every required graph kind"))
    else:
        for name, row in graphs.items():
            if (not isinstance(row, dict) or row.get("status") != "passed"
                    or row.get("legacy_edge_count") != 0):
                findings.append(finding("TM-COMP-GRAPH-RESIDUE", "dependency-graph",
                                        f"{name} has not proved zero legacy edges"))
            else:
                check_log_reference(findings, "dependency-graph", verified, row.get("log"), name)
    targets = set(data.get("targets", []))
    missing = sorted(required_targets - targets)
    if missing:
        findings.append(finding("TM-COMP-GRAPH-TARGETS", "dependency-graph",
                                "dependency evidence omits required deliverables",
                                len(missing), missing))
    return findings


def resolve_external_path(evidence_root: Path, value: Any) -> Path | None:
    if not isinstance(value, str) or not value:
        return None
    path = Path(value)
    return path.resolve() if path.is_absolute() else (evidence_root / path).resolve()


def symbol_findings(data: dict[str, Any], verified: set[str], evidence_root: Path,
                    manifest: dict[str, Any]) -> list[Finding]:
    findings: list[Finding] = []
    rows = data.get("binaries")
    required = set(manifest["installed"]["required_deliverables"])
    if not isinstance(rows, list):
        return [finding("TM-COMP-SYMBOL-ROWS", "symbol", "binary symbol rows are missing")]
    names = {str(row.get("name")) for row in rows if isinstance(row, dict)}
    missing = sorted(required - names)
    if missing:
        findings.append(finding("TM-COMP-SYMBOL-TARGETS", "symbol",
                                "symbol evidence omits required deliverables", len(missing), missing))
    for row in rows:
        if not isinstance(row, dict):
            findings.append(finding("TM-COMP-SYMBOL-ROW", "symbol", "malformed binary row"))
            continue
        path = resolve_external_path(evidence_root, row.get("path"))
        if path is None or not path.is_file():
            findings.append(finding("TM-COMP-SYMBOL-BINARY", "symbol",
                                    f"missing audited binary: {row.get('path')!r}"))
        elif not exact_sha256(row.get("sha256")) or sha256_file(path) != row.get("sha256"):
            findings.append(finding("TM-COMP-SYMBOL-BINARY-DIGEST", "symbol",
                                    f"binary digest mismatch: {row.get('name')}"))
        if row.get("forbidden_symbol_count") != 0:
            findings.append(finding("TM-COMP-SYMBOL-RESIDUE", "symbol",
                                    f"legacy symbols remain in {row.get('name')}",
                                    int(row.get("forbidden_symbol_count", 1))))
        check_log_reference(findings, "symbol", verified, row.get("symbol_log"),
                            str(row.get("name")))
    return findings


def installed_findings(data: dict[str, Any], verified: set[str], evidence_root: Path,
                       manifest: dict[str, Any]) -> list[Finding]:
    findings: list[Finding] = []
    if data.get("empty_stage_replay") != "passed" or data.get("no_work_replay") != "passed":
        findings.append(finding("TM-COMP-INSTALLED-REPLAY", "installed",
                                "installed evidence lacks empty-stage and no-work replays"))
    required_deliverables = set(manifest["installed"]["required_deliverables"])
    required_headers = set(manifest["installed"]["required_public_headers"])
    for field, required in (("deliverables", required_deliverables),
                            ("public_headers", required_headers)):
        missing = sorted(required - set(data.get(field, [])))
        if missing:
            findings.append(finding("TM-COMP-INSTALLED-COVERAGE", "installed",
                                    f"installed evidence omits {field}", len(missing), missing))
    check_log_reference(findings, "installed", verified, data.get("inventory_log"),
                        "installed inventory")
    install_root = resolve_external_path(evidence_root, data.get("install_root"))
    if install_root is None or not install_root.is_dir():
        findings.append(finding("TM-COMP-INSTALLED-ROOT", "installed",
                                "installed evidence root is missing"))
        return findings
    sdk_manifest_path = install_root / "share/xray/install/aot-sdk-closure.json"
    sdk_manifest = read_json(sdk_manifest_path) if sdk_manifest_path.is_file() else None
    if (not isinstance(sdk_manifest, dict)
            or set(sdk_manifest) != {"schema", "generator", "entries"}
            or sdk_manifest.get("schema") != 1
            or sdk_manifest.get("generator") != "xray-aot-sdk-header-closure/1"
            or not isinstance(sdk_manifest.get("entries"), list)
            or not sdk_manifest["entries"]):
        findings.append(finding("TM-COMP-INSTALLED-SDK-CLOSURE", "installed",
                                "exact AOT SDK closure manifest is missing or invalid"))
    else:
        expected: dict[str, str] = {}
        invalid = False
        for row in sdk_manifest["entries"]:
            if (not isinstance(row, dict)
                    or set(row) != {"install_path", "sha256", "source_path"}
                    or not isinstance(row.get("install_path"), str)
                    or not exact_sha256(row.get("sha256"))
                    or not isinstance(row.get("source_path"), str)
                    or row["install_path"] in expected):
                invalid = True
                continue
            expected[row["install_path"]] = row["sha256"]
        actual = {
            path.relative_to(install_root).as_posix(): sha256_file(path)
            for base in (install_root / "include/xray", install_root / "lib/xray/sdk")
            if base.is_dir()
            for path in base.rglob("*") if path.is_file()
        }
        if invalid or actual != expected:
            samples = sorted(set(expected) ^ set(actual))[:MAX_SAMPLES]
            samples.extend(
                relative for relative in sorted(set(expected) & set(actual))
                if expected[relative] != actual[relative]
            )
            findings.append(finding("TM-COMP-INSTALLED-SDK-CLOSURE", "installed",
                                    "installed SDK is not the exact declared header closure",
                                    len(samples), samples[:MAX_SAMPLES]))
    path_regex = re.compile(manifest["installed"]["forbidden_path_regex"])
    text_regex = re.compile(manifest["installed"]["forbidden_text_regex"])
    samples: list[str] = []
    count = 0
    for path in sorted(item for item in install_root.rglob("*") if item.is_file()):
        relative = path.relative_to(install_root).as_posix()
        if path_regex.search(relative):
            count += 1
            samples.append(f"{relative}: path")
        text = readable_text(path)
        if text is not None:
            for match in text_regex.finditer(text):
                count += 1
                if len(samples) < MAX_SAMPLES:
                    samples.append(f"{relative}: {match.group(0)}")
    if count:
        findings.append(finding("TM-COMP-INSTALLED-RESIDUE", "installed",
                                "legacy paths or text remain in the installed tree",
                                count, samples))
    return findings


def runtime_findings(data: dict[str, Any], verified: set[str],
                     manifest: dict[str, Any]) -> list[Finding]:
    findings: list[Finding] = []
    expected = manifest["runtime_reachability"]["required_lanes"]
    lanes = data.get("lanes")
    if not isinstance(lanes, dict) or set(lanes) != set(expected):
        findings.append(finding("TM-COMP-RUNTIME-LANES", "runtime-reachability",
                                "runtime reachability lanes are incomplete"))
    else:
        for name, result in expected.items():
            row = lanes[name]
            if (not isinstance(row, dict) or row.get("result") != result
                    or row.get("command_replayed") is not True):
                findings.append(finding("TM-COMP-RUNTIME-RESULT", "runtime-reachability",
                                        f"runtime lane {name} did not prove {result}"))
            else:
                check_log_reference(findings, "runtime-reachability", verified,
                                    row.get("log"), name)
    for field in ("activation_before_verify", "forbidden_link_symbol_count"):
        if data.get(field) != 0:
            findings.append(finding("TM-COMP-RUNTIME-RESIDUE", "runtime-reachability",
                                    f"runtime evidence requires {field}=0"))
    if data.get("external_activation_canary") != "passed":
        findings.append(finding("TM-COMP-RUNTIME-CANARY", "runtime-reachability",
                                "external runtime activation canary has not passed"))
    return findings


def matrix_findings(data: dict[str, Any], verified: set[str], root: Path,
                    manifest: dict[str, Any]) -> list[Finding]:
    findings: list[Finding] = []
    policy_path = root / manifest["matrix"]["policy"]
    if not policy_path.is_file():
        return [finding("TM-COMP-MATRIX-POLICY", "matrix", "matrix policy is missing")]
    policy = read_json(policy_path)
    policy_rows = policy.get("rows")
    rows = data.get("rows")
    if not isinstance(policy_rows, list) or not isinstance(rows, list):
        return [finding("TM-COMP-MATRIX-ROWS", "matrix", "matrix policy/evidence rows are missing")]
    dimensions = manifest["matrix"]["required_dimensions"]
    catalog = data.get("axis_catalog")
    if not isinstance(catalog, dict) or set(catalog) != set(dimensions):
        findings.append(finding("TM-COMP-MATRIX-AXES", "matrix",
                                "full matrix axis catalog is incomplete"))
    by_id = {str(row.get("id")): row for row in rows if isinstance(row, dict)}
    if len(by_id) != len(rows):
        findings.append(finding("TM-COMP-MATRIX-IDS", "matrix",
                                "matrix evidence IDs are missing or duplicate"))
    qualifying = set(manifest["matrix"]["qualifying_tiers"])
    routes: set[str] = set()
    fingerprint_fields = {
        "artifact_fingerprint", "baseline_fingerprint", "binary_fingerprint",
        "manifest_fingerprint", "source_fingerprint",
    }
    for expected in policy_rows:
        row_id = str(expected.get("id"))
        row = by_id.get(row_id)
        if row is None:
            findings.append(finding("TM-COMP-MATRIX-MISSING-ROW", "matrix",
                                    f"missing matrix evidence row {row_id}"))
            continue
        for dimension in dimensions:
            expected_value = expected.get(dimension)
            actual_value = row.get(dimension)
            if dimension == "artifact_route":
                if actual_value != expected_value and expected_value not in row.get("policy_routes", []):
                    findings.append(finding("TM-COMP-MATRIX-DIMENSION", "matrix",
                                            f"{row_id} changed policy {dimension}"))
            elif actual_value != expected_value:
                findings.append(finding("TM-COMP-MATRIX-DIMENSION", "matrix",
                                        f"{row_id} changed policy {dimension}"))
        tier = expected.get("support_tier")
        expected_status = "passed" if tier in qualifying else "unsupported"
        if row.get("status") != expected_status:
            findings.append(finding("TM-COMP-MATRIX-STATUS", "matrix",
                                    f"{row_id} must be {expected_status} for tier {tier}"))
        if expected_status == "unsupported":
            if not row.get("reason") or not row.get("owner"):
                findings.append(finding("TM-COMP-MATRIX-BOUNDARY", "matrix",
                                        f"{row_id} lacks an explicit unsupported boundary"))
            continue
        missing = [field for field in fingerprint_fields if not exact_sha256(row.get(field))]
        if missing:
            findings.append(finding("TM-COMP-MATRIX-FINGERPRINT", "matrix",
                                    f"{row_id} lacks exact evidence fingerprints",
                                    len(missing), missing))
        required_values = {
            "positive_activation": "activated-and-passed",
            "negative_mismatch": "rejected-before-activation",
            "artifact_retention": "retained",
        }
        for field, value in required_values.items():
            if row.get(field) != value:
                findings.append(finding("TM-COMP-MATRIX-ACTIVATION", "matrix",
                                        f"{row_id} lacks {field}={value}"))
        for field in ("oracle", "performance_policy", "owner", "last_verified"):
            if not row.get(field):
                findings.append(finding("TM-COMP-MATRIX-EVIDENCE", "matrix",
                                        f"{row_id} lacks {field}"))
        check_log_reference(findings, "matrix", verified, row.get("log"), row_id)
        routes.update(str(item) for item in row.get("artifact_routes", []))
    extras = sorted(set(by_id) - {str(row.get("id")) for row in policy_rows})
    if extras:
        findings.append(finding("TM-COMP-MATRIX-EXTRA-ROW", "matrix",
                                "unclassified matrix evidence rows are forbidden",
                                len(extras), extras))
    missing_routes = sorted(set(manifest["matrix"]["required_artifact_routes"]) - routes)
    if missing_routes:
        findings.append(finding("TM-COMP-MATRIX-ROUTES", "matrix",
                                "qualifying matrix evidence omits required artifact routes",
                                len(missing_routes), missing_routes))
    return findings


def activation_findings(data: dict[str, Any], verified: set[str],
                        manifest: dict[str, Any]) -> list[Finding]:
    findings: list[Finding] = []
    lifecycle = data.get("lifecycle")
    expected_lifecycle = {"activate", "drain", "rollback", "unload"}
    if not isinstance(lifecycle, dict) or set(lifecycle) != expected_lifecycle:
        findings.append(finding("TM-COMP-ACTIVATION-LIFECYCLE", "activation-generation",
                                "generation lifecycle coverage is incomplete"))
    else:
        for name, row in lifecycle.items():
            if not isinstance(row, dict) or row.get("result") != "passed":
                findings.append(finding("TM-COMP-ACTIVATION-RESULT", "activation-generation",
                                        f"generation lifecycle {name} has not passed"))
            else:
                check_log_reference(findings, "activation-generation", verified,
                                    row.get("log"), name)
    mismatches = data.get("negative_mismatches")
    expected_mismatches = {"capability", "fingerprint", "provider", "schema", "target"}
    if not isinstance(mismatches, dict) or set(mismatches) != expected_mismatches:
        findings.append(finding("TM-COMP-ACTIVATION-MISMATCHES", "activation-generation",
                                "artifact mismatch rejection coverage is incomplete"))
    else:
        for name, result in mismatches.items():
            if result != "rejected-before-activation":
                findings.append(finding("TM-COMP-ACTIVATION-MISMATCH", "activation-generation",
                                        f"{name} mismatch was not rejected before activation"))
    identities = data.get("identities")
    if (not isinstance(identities, dict)
            or set(identities) != {"artifact", "generation", "semantic", "target"}
            or any(not exact_sha256(value) for value in identities.values())):
        findings.append(finding("TM-COMP-ACTIVATION-IDENTITY", "activation-generation",
                                "activation/generation identities are incomplete"))
    if data.get("activation_before_verify") != 0 or data.get("runtime_only_compiler_symbols") != 0:
        findings.append(finding("TM-COMP-ACTIVATION-RESIDUE", "activation-generation",
                                "activation-before-verify and runtime-only compiler reachability must be zero"))
    routes = set(data.get("artifact_routes", []))
    missing = sorted(set(manifest["matrix"]["required_artifact_routes"]) - routes)
    if missing:
        findings.append(finding("TM-COMP-ACTIVATION-ROUTES", "activation-generation",
                                "activation evidence omits required artifact routes",
                                len(missing), missing))
    route_proofs = data.get("route_proofs")
    required_route_proofs = {"hosted-fragment", "target-plan-to-native"}
    if not isinstance(route_proofs, dict) or set(route_proofs) != required_route_proofs:
        findings.append(finding("TM-COMP-ACTIVATION-ROUTE-PROOFS", "activation-generation",
                                "native artifact route proofs are incomplete"))
    else:
        for name, row in route_proofs.items():
            if not isinstance(row, dict) or row.get("result") != "passed":
                findings.append(finding("TM-COMP-ACTIVATION-ROUTE-PROOF",
                                        "activation-generation",
                                        f"artifact route {name} did not pass"))
            else:
                check_log_reference(findings, "activation-generation", verified,
                                    row.get("log"), name)
    return findings


def full_validation_findings(data: dict[str, Any], verified: set[str],
                             manifest: dict[str, Any]) -> list[Finding]:
    findings: list[Finding] = []
    rows = data.get("lanes")
    required = set(manifest["validation"]["required_lanes"])
    if not isinstance(rows, list):
        return [finding("TM-COMP-VALIDATION-LANES", "full-validation",
                        "full validation lanes are missing")]
    by_name = {str(row.get("name")): row for row in rows if isinstance(row, dict)}
    if len(by_name) != len(rows):
        findings.append(finding("TM-COMP-VALIDATION-IDS", "full-validation",
                                "full validation lane names are missing or duplicate"))
    missing = sorted(required - set(by_name))
    if missing:
        findings.append(finding("TM-COMP-VALIDATION-COVERAGE", "full-validation",
                                "full validation evidence omits mandatory lanes",
                                len(missing), missing))
    extras = sorted(set(by_name) - required)
    if extras:
        findings.append(finding("TM-COMP-VALIDATION-EXTRA", "full-validation",
                                "unclassified validation lanes are forbidden",
                                len(extras), extras))
    for name, row in by_name.items():
        if row.get("status") != "passed" or not row.get("command") or not row.get("platform"):
            findings.append(finding("TM-COMP-VALIDATION-RESULT", "full-validation",
                                    f"validation lane {name} lacks a passed command/platform result"))
        check_log_reference(findings, "full-validation", verified, row.get("log"), name)
    return findings


def load_completion_evidence(root: Path, evidence_root: Path, manifest: dict[str, Any],
                             identity: dict[str, str]) -> list[Finding]:
    findings: list[Finding] = []
    required = manifest["evidence"]["required_files"]
    input_sha256 = manifest["input_identity"]["sha256"]
    loaded: dict[str, tuple[dict[str, Any], set[str]]] = {}
    for kind, relative in sorted(required.items()):
        path = evidence_root / relative
        if not path.is_file():
            findings.append(finding("TM-COMP-EVIDENCE-MISSING", kind,
                                    f"missing completion evidence: {path}"))
            continue
        try:
            data = read_json(path)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            findings.append(finding("TM-COMP-EVIDENCE-PARSE", kind, str(error)))
            continue
        common, verified = common_evidence_findings(
            data, kind, evidence_root, identity, input_sha256
        )
        findings.extend(common)
        loaded[kind] = (data, verified)
    targets = set(manifest["installed"]["required_deliverables"])
    validators = {
        "dependency-graph": lambda data, logs: dependency_findings(data, logs, targets),
        "symbol": lambda data, logs: symbol_findings(data, logs, evidence_root, manifest),
        "installed": lambda data, logs: installed_findings(data, logs, evidence_root, manifest),
        "runtime-reachability": lambda data, logs: runtime_findings(data, logs, manifest),
        "matrix": lambda data, logs: matrix_findings(data, logs, root, manifest),
        "activation-generation": lambda data, logs: activation_findings(data, logs, manifest),
        "full-validation": lambda data, logs: full_validation_findings(data, logs, manifest),
    }
    for kind, validator in validators.items():
        if kind in loaded:
            findings.extend(validator(*loaded[kind]))
    return findings


def audit(root: Path, manifest: dict[str, Any], evidence_root: Path) -> dict[str, Any]:
    findings = validate_manifest(manifest)
    identity, identity_rows = repository_identity(root)
    findings.extend(identity_rows)
    findings.extend(authority_findings(root, manifest))
    findings.extend(source_residue_findings(root, manifest))
    findings.extend(terminal_inventory_findings(root, manifest))
    findings.extend(dual_owner_findings(root, manifest))
    if identity.get("source_commit") and identity.get("repository_sha256"):
        findings.extend(load_completion_evidence(root, evidence_root, manifest, identity))
    findings.sort(key=lambda row: (row.surface, row.code, row.message))
    summary: dict[str, int] = {}
    for row in findings:
        summary[row.surface] = summary.get(row.surface, 0) + row.count
    return {
        "checker": CHECKER,
        "ok": not findings,
        "identity": identity,
        "evidence_root": str(evidence_root),
        "summary": summary,
        "findings": [row.as_dict() for row in findings],
    }


def print_human(report: dict[str, Any]) -> None:
    result = "PASS" if report["ok"] else "FAIL"
    print(f"target-machine completion governance: {result}")
    identity = report.get("identity", {})
    if identity:
        print(f"  source_commit={identity.get('source_commit', '<unknown>')}")
        print(f"  repository_sha256={identity.get('repository_sha256', '<unknown>')}")
    if report["summary"]:
        print("  residue/evidence summary:")
        for surface, count in sorted(report["summary"].items()):
            print(f"    {surface}: {count}")
    for row in report["findings"]:
        print(f"  [{row['code']}] {row['message']} (count={row['count']})")
        for sample in row["samples"]:
            print(f"    - {sample}")


def fixture_identity() -> dict[str, str]:
    return {"source_commit": "a" * 40, "repository_sha256": "b" * 64}


def fixture_envelope(root: Path, kind: str, identity: dict[str, str],
                     input_sha256: str) -> dict[str, Any]:
    relative = f"logs/{kind}.log"
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"{kind}: passed\n", encoding="utf-8")
    digest = sha256_file(path)
    return {
        "schema": EVIDENCE_SCHEMA,
        "kind": kind,
        "status": "passed",
        "source_commit": identity["source_commit"],
        "repository_sha256": identity["repository_sha256"],
        "governance_input_sha256": input_sha256,
        "owner": "self-test",
        "generated_at": "2026-08-11T00:00:00Z",
        "logs": [{
            "path": relative,
            "sha256": digest,
            "identity_sha256": log_identity(
                kind, identity["source_commit"], identity["repository_sha256"], relative, digest
            ),
            "result": "passed",
        }],
    }


def expect_mutation(results: list[str], label: str, rows: list[Finding], code: str) -> None:
    if not any(row.code == code for row in rows):
        raise AssertionError(f"mutation {label} did not trigger {code}: {rows}")
    results.append(f"{label}->{code}")


def self_test(manifest_path: Path) -> int:
    try:
        manifest = read_json(manifest_path)
        manifest_errors = validate_manifest(manifest)
        if manifest_errors:
            raise AssertionError(f"manifest invalid: {manifest_errors}")
        results: list[str] = []
        with tempfile.TemporaryDirectory(prefix="xray-completion-governance-") as directory:
            root = Path(directory)
            for relative in manifest["residue_scan"]["roots"]:
                path = root / relative
                if Path(relative).suffix:
                    path.parent.mkdir(parents=True, exist_ok=True)
                else:
                    path.mkdir(parents=True, exist_ok=True)
            (root / "CMakeLists.txt").write_text("# clean\n", encoding="utf-8")
            (root / "src/clean.c").write_text("int clean(void) { return 0; }\n", encoding="utf-8")
            if source_residue_findings(root, manifest):
                raise AssertionError("clean residue fixture was rejected")
            mutations = [
                ("xrc-source", "src/xrc_owner.c", 'const char *x = ".xrc";\n',
                 "TM-COMP-RESIDUE-LEGACY-XRC", "source"),
                ("vm-include", "include/xray_vm.h", "typedef int XrayVMConfig;\n",
                 "TM-COMP-RESIDUE-LEGACY-VM", "include"),
                ("opcode-build", "CMakeLists.txt", "set(LEGACY OP_ADD)\n",
                 "TM-COMP-RESIDUE-LEGACY-OPCODE", "build"),
                ("tagged-frame", "src/frame.c", "typedef struct XrBcCallFrame XrBcCallFrame;\n",
                 "TM-COMP-RESIDUE-LEGACY-TAGGED-FRAME", "source"),
                ("xaot-plan", "src/aot_plan.c", "typedef int XaotFuncAbi;\n",
                 "TM-COMP-RESIDUE-LEGACY-XAOT-PLAN", "source"),
            ]
            for label, relative, content, code, surface in mutations:
                path = root / relative
                old = path.read_bytes() if path.is_file() else None
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding="utf-8")
                rows = source_residue_findings(root, manifest)
                expect_mutation(results, label,
                                [row for row in rows if row.surface == surface], code)
                if old is None:
                    path.unlink()
                else:
                    path.write_bytes(old)

            identity = fixture_identity()
            input_sha256 = "c" * 64
            evidence_root = root / "evidence"
            evidence_root.mkdir()
            envelope = fixture_envelope(evidence_root, "symbol", identity, input_sha256)
            common, logs = common_evidence_findings(
                envelope, "symbol", evidence_root, identity, input_sha256
            )
            if common or not logs:
                raise AssertionError(f"clean common evidence rejected: {common}")
            (evidence_root / "logs/symbol.log").write_text("mutated\n", encoding="utf-8")
            common, _ = common_evidence_findings(
                envelope, "symbol", evidence_root, identity, input_sha256
            )
            expect_mutation(results, "identity-log", common, "TM-COMP-EVIDENCE-LOG-DIGEST")

            dependency = fixture_envelope(evidence_root, "dependency-graph", identity, input_sha256)
            log = dependency["logs"][0]["path"]
            dependency["graphs"] = {
                name: {"status": "passed", "legacy_edge_count": 0, "log": log}
                for name in GRAPH_KINDS
            }
            dependency["targets"] = manifest["installed"]["required_deliverables"]
            common, verified = common_evidence_findings(
                dependency, "dependency-graph", evidence_root, identity, input_sha256
            )
            if common or dependency_findings(
                    dependency, verified, set(manifest["installed"]["required_deliverables"])):
                raise AssertionError("clean dependency fixture rejected")
            broken = copy.deepcopy(dependency)
            broken["graphs"]["build-ninja"]["legacy_edge_count"] = 1
            expect_mutation(results, "dependency-graph",
                            dependency_findings(broken, verified, set(broken["targets"])),
                            "TM-COMP-GRAPH-RESIDUE")

            symbol = fixture_envelope(evidence_root, "symbol", identity, input_sha256)
            binary = evidence_root / "bin/xray.bin"
            binary.parent.mkdir()
            binary.write_bytes(b"xray")
            symbol_log = symbol["logs"][0]["path"]
            symbol["binaries"] = [{
                "name": name, "path": "bin/xray.bin", "sha256": sha256_file(binary),
                "forbidden_symbol_count": 0, "symbol_log": symbol_log,
            } for name in manifest["installed"]["required_deliverables"]]
            common, verified = common_evidence_findings(
                symbol, "symbol", evidence_root, identity, input_sha256
            )
            if common or symbol_findings(symbol, verified, evidence_root, manifest):
                raise AssertionError("clean symbol fixture rejected")
            broken = copy.deepcopy(symbol)
            broken["binaries"][0]["forbidden_symbol_count"] = 1
            expect_mutation(results, "symbol", symbol_findings(
                broken, verified, evidence_root, manifest), "TM-COMP-SYMBOL-RESIDUE")

            installed = fixture_envelope(evidence_root, "installed", identity, input_sha256)
            install_root = evidence_root / "install"
            for header in manifest["installed"]["required_public_headers"]:
                path = install_root / header
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("/* runtime */\n", encoding="utf-8")
            installed.update({
                "empty_stage_replay": "passed", "no_work_replay": "passed",
                "deliverables": manifest["installed"]["required_deliverables"],
                "public_headers": manifest["installed"]["required_public_headers"],
                "install_root": "install", "inventory_log": installed["logs"][0]["path"],
            })
            sdk_header = install_root / "include/xray/runtime.h"
            write_json(install_root / "share/xray/install/aot-sdk-closure.json", {
                "schema": 1, "generator": "xray-aot-sdk-header-closure/1",
                "entries": [{
                    "source_path": "include/runtime.h",
                    "install_path": "include/xray/runtime.h",
                    "sha256": sha256_file(sdk_header),
                }],
            })
            common, verified = common_evidence_findings(
                installed, "installed", evidence_root, identity, input_sha256
            )
            if common or installed_findings(installed, verified, evidence_root, manifest):
                raise AssertionError("clean installed fixture rejected")
            (install_root / "legacy.xrc").write_bytes(b"legacy")
            expect_mutation(results, "installed", installed_findings(
                installed, verified, evidence_root, manifest), "TM-COMP-INSTALLED-RESIDUE")
            (install_root / "legacy.xrc").unlink()
            (install_root / "lib/xray/sdk/src/extra.h").parent.mkdir(parents=True)
            (install_root / "lib/xray/sdk/src/extra.h").write_text("/* extra */\n",
                                                                    encoding="utf-8")
            expect_mutation(results, "installed-sdk", installed_findings(
                installed, verified, evidence_root, manifest),
                "TM-COMP-INSTALLED-SDK-CLOSURE")

            runtime = fixture_envelope(
                evidence_root, "runtime-reachability", identity, input_sha256
            )
            runtime_log = runtime["logs"][0]["path"]
            runtime.update({
                "lanes": {name: {"result": value, "command_replayed": True, "log": runtime_log}
                          for name, value in manifest["runtime_reachability"]["required_lanes"].items()},
                "activation_before_verify": 0, "forbidden_link_symbol_count": 0,
                "external_activation_canary": "passed",
            })
            common, verified = common_evidence_findings(
                runtime, "runtime-reachability", evidence_root, identity, input_sha256
            )
            if common or runtime_findings(runtime, verified, manifest):
                raise AssertionError("clean runtime fixture rejected")
            broken = copy.deepcopy(runtime)
            broken["lanes"]["xrc-negative"]["result"] = "activated-and-passed"
            expect_mutation(results, "runtime", runtime_findings(
                broken, verified, manifest), "TM-COMP-RUNTIME-RESULT")

            matrix_root = root / "matrix-root"
            matrix_policy_path = matrix_root / "contracts/target-machine/validation-matrix.json"
            matrix_policy_path.parent.mkdir(parents=True)
            matrix_policy_row = {
                "id": "TM-MATRIX-FIXTURE", "target": "fixture-target",
                "provider": "fixture-provider", "artifact_route": "source",
                "executor_or_generation": "vm+aot", "build_or_sanitizer": "Release",
                "support_tier": "supported",
            }
            write_json(matrix_policy_path, {"rows": [matrix_policy_row]})
            matrix_manifest = copy.deepcopy(manifest)
            matrix_manifest["matrix"]["policy"] = (
                "contracts/target-machine/validation-matrix.json"
            )
            matrix = fixture_envelope(evidence_root, "matrix", identity, input_sha256)
            matrix_log = matrix["logs"][0]["path"]
            matrix["axis_catalog"] = {
                name: [matrix_policy_row[name]]
                for name in matrix_manifest["matrix"]["required_dimensions"]
            }
            matrix["rows"] = [{
                **matrix_policy_row,
                "status": "passed",
                "source_fingerprint": "e" * 64,
                "binary_fingerprint": "e" * 64,
                "artifact_fingerprint": "e" * 64,
                "manifest_fingerprint": "e" * 64,
                "baseline_fingerprint": "e" * 64,
                "positive_activation": "activated-and-passed",
                "negative_mismatch": "rejected-before-activation",
                "artifact_retention": "retained",
                "oracle": "fixture oracle",
                "performance_policy": "fixture policy",
                "owner": "fixture owner",
                "last_verified": "2026-08-11",
                "log": matrix_log,
                "artifact_routes": matrix_manifest["matrix"]["required_artifact_routes"],
            }]
            common, verified = common_evidence_findings(
                matrix, "matrix", evidence_root, identity, input_sha256
            )
            if common or matrix_findings(
                    matrix, verified, matrix_root, matrix_manifest):
                raise AssertionError("clean matrix fixture rejected")
            broken = copy.deepcopy(matrix)
            broken["rows"][0]["status"] = "failed"
            expect_mutation(results, "matrix", matrix_findings(
                broken, verified, matrix_root, matrix_manifest), "TM-COMP-MATRIX-STATUS")

            activation = fixture_envelope(
                evidence_root, "activation-generation", identity, input_sha256
            )
            activation_log = activation["logs"][0]["path"]
            activation.update({
                "lifecycle": {name: {"result": "passed", "log": activation_log}
                              for name in ("activate", "drain", "rollback", "unload")},
                "negative_mismatches": {name: "rejected-before-activation"
                                        for name in ("capability", "fingerprint", "provider",
                                                     "schema", "target")},
                "identities": {name: "d" * 64
                               for name in ("artifact", "generation", "semantic", "target")},
                "activation_before_verify": 0, "runtime_only_compiler_symbols": 0,
                "artifact_routes": manifest["matrix"]["required_artifact_routes"],
                "route_proofs": {
                    name: {"result": "passed", "log": activation_log}
                    for name in ("hosted-fragment", "target-plan-to-native")
                },
            })
            common, verified = common_evidence_findings(
                activation, "activation-generation", evidence_root, identity, input_sha256
            )
            if common or activation_findings(activation, verified, manifest):
                raise AssertionError("clean activation fixture rejected")
            broken = copy.deepcopy(activation)
            broken["lifecycle"]["rollback"]["result"] = "failed"
            expect_mutation(results, "activation-generation", activation_findings(
                broken, verified, manifest), "TM-COMP-ACTIVATION-RESULT")

            validation = fixture_envelope(evidence_root, "full-validation", identity, input_sha256)
            validation_log = validation["logs"][0]["path"]
            validation["lanes"] = [{
                "name": name, "status": "passed", "command": f"run {name}",
                "platform": "fixture", "log": validation_log,
            } for name in manifest["validation"]["required_lanes"]]
            common, verified = common_evidence_findings(
                validation, "full-validation", evidence_root, identity, input_sha256
            )
            if common or full_validation_findings(validation, verified, manifest):
                raise AssertionError("clean validation fixture rejected")
            broken = copy.deepcopy(validation)
            broken["lanes"][0]["status"] = "failed"
            expect_mutation(results, "full-validation", full_validation_findings(
                broken, verified, manifest), "TM-COMP-VALIDATION-RESULT")

            owner_root = root / "owner"
            (owner_root / "contracts/target-machine").mkdir(parents=True)
            (owner_root / "src/shared").mkdir(parents=True)
            (owner_root / "src/shared/owner.h").write_text("/* owner */\n", encoding="utf-8")
            owner_manifest = {"dual_owner": {
                "inventory": "contracts/target-machine/semantic-owner-inventory.json",
                "mechanical_adapter": "representation adapter",
            }}
            owner_data = {"operation_count": 1, "operations": [{
                "operation_id": "shared.fixture", "current_shared_owner": "src/shared/owner.h",
                "current_vm_owner": "representation adapter",
                "current_aot_owner": "representation adapter", "independent_oracle": "fixture",
            }]}
            write_json(owner_root / owner_manifest["dual_owner"]["inventory"], owner_data)
            if dual_owner_findings(owner_root, owner_manifest):
                raise AssertionError("clean owner fixture rejected")
            owner_data["operations"][0]["current_vm_owner"] = "vm semantic owner"
            write_json(owner_root / owner_manifest["dual_owner"]["inventory"], owner_data)
            expect_mutation(results, "dual-owner", dual_owner_findings(
                owner_root, owner_manifest), "TM-COMP-DUAL-OWNER")

            inventory_root = root / "inventory"
            (inventory_root / "contracts/target-machine").mkdir(parents=True)
            inventory_manifest = {"inventories": {
                "legacy_vm": "contracts/target-machine/legacy-vm-inventory.json",
                "legacy_product": "contracts/target-machine/legacy-product-residue.json",
                "aot_plan": "contracts/target-machine/aot-plan-destination-inventory.json",
            }}
            write_json(inventory_root / inventory_manifest["inventories"]["legacy_vm"], {
                "opcode_count": 0, "opcodes": [], "tagged_frame_sites": [],
                "vm_public_api_symbols": [], "legacy_artifact_symbols": [], "artifact": {},
            })
            write_json(inventory_root / inventory_manifest["inventories"]["legacy_product"], {
                "total": 0, "owner_count": 0,
            })
            write_json(inventory_root / inventory_manifest["inventories"]["aot_plan"], {
                "row_count": 0, "rows": [], "mixed_representation_types": {},
            })
            if terminal_inventory_findings(inventory_root, inventory_manifest):
                raise AssertionError("clean terminal inventories rejected")
            write_json(inventory_root / inventory_manifest["inventories"]["legacy_vm"], {
                "opcode_count": 1, "opcodes": [{"opcode": "mutated"}],
                "tagged_frame_sites": [], "vm_public_api_symbols": [],
                "legacy_artifact_symbols": [], "artifact": {},
            })
            expect_mutation(results, "terminal-inventory", terminal_inventory_findings(
                inventory_root, inventory_manifest), "TM-COMP-INVENTORY-LEGACY-VM")

        required_labels = {
            "xrc-source", "vm-include", "opcode-build", "tagged-frame", "xaot-plan",
            "identity-log", "dependency-graph", "symbol", "installed", "installed-sdk", "runtime",
            "matrix", "activation-generation", "full-validation", "dual-owner",
            "terminal-inventory",
        }
        observed = {item.split("->", 1)[0] for item in results}
        if observed != required_labels:
            raise AssertionError(f"mutation coverage mismatch: {sorted(observed)}")
    except (AssertionError, OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"target-machine completion governance self-test: FAIL: {error}", file=sys.stderr)
        return 1
    print(f"target-machine completion governance self-test: PASS ({len(results)} mutations)")
    for item in results:
        print(f"  {item}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    parser.add_argument("--manifest", default=DEFAULT_MANIFEST)
    parser.add_argument("--evidence-root")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    root = Path(args.root).resolve()
    manifest_path = Path(args.manifest)
    if not manifest_path.is_absolute():
        manifest_path = root / manifest_path
    try:
        manifest = read_json(manifest_path)
        if args.self_test:
            return self_test(manifest_path)
        evidence_root = Path(args.evidence_root) if args.evidence_root else Path(
            manifest["evidence"]["default_directory"]
        )
        if not evidence_root.is_absolute():
            evidence_root = root / evidence_root
        report = audit(root, manifest, evidence_root.resolve())
    except (OSError, ValueError, KeyError, json.JSONDecodeError, re.error) as error:
        print(f"target-machine completion governance: ERROR: {error}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print_human(report)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
