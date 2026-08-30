#!/usr/bin/env python3
"""Validate and run the versioned regex absolute oracle without case filtering."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
ORACLE_ROOT = ROOT / "tests/regex/absolute/v1"
MANIFEST_PATH = ORACLE_ROOT / "manifest.json"

MANIFEST_KEYS = {
    "cases",
    "engine_snapshot",
    "exit_code",
    "hazard_inventory",
    "hazard_inventory_sha256",
    "mutation_inventory",
    "mutation_inventory_sha256",
    "negative_inventory",
    "negative_inventory_sha256",
    "oracle",
    "required_backends",
    "schema",
    "semantic_contract",
    "stderr",
    "stdout",
    "unicode_contract",
}
CASE_KEYS = {
    "domains",
    "expected_sha256",
    "expected_stdout",
    "id",
    "required_capabilities",
    "source",
    "source_sha256",
}
NEGATIVE_KEYS = {
    "activation_owner",
    "expected_kind",
    "expected_offset",
    "expected_offset_domain",
    "flags",
    "id",
    "pattern_utf8_hex",
    "surface",
}
MUTATION_KEYS = {
    "activation_owner",
    "expected_rejection",
    "id",
    "mutation",
    "target_field",
}
HAZARD_KEYS = {
    "activation_owner",
    "current_outcome",
    "id",
    "input_utf8_hex",
    "operation",
    "pattern_utf8_hex",
    "replacement_utf8_hex",
    "required_end_state",
    "state",
}
DOMAINS = {
    "captures",
    "empty_match",
    "errors",
    "lexical",
    "limits",
    "matching",
    "replace_split",
    "unicode",
}
CAPABILITIES = {
    "regex.dynamic.compile",
    "regex.empty-match.scalar-progress",
    "regex.literal.syntax",
    "regex.unicode.properties",
}
CASE_ID_PATTERN = re.compile(r"[a-z0-9]+(?:_[a-z0-9]+)*\Z")
DIGEST_PATTERN = re.compile(r"[0-9a-f]{64}\Z")
REQUIRED_CASE_SHAPES = {
    "literal_lexical": {
        "domains": ["lexical", "matching"],
        "required_capabilities": ["regex.dynamic.compile", "regex.literal.syntax"],
    },
    "literal_no_import": {
        "domains": ["lexical"],
        "required_capabilities": ["regex.literal.syntax"],
    },
    "matching_core": {
        "domains": ["captures", "errors", "limits", "matching", "replace_split", "unicode"],
        "required_capabilities": ["regex.dynamic.compile", "regex.unicode.properties"],
    },
    "semantic_edges": {
        "domains": ["captures", "empty_match", "matching", "replace_split"],
        "required_capabilities": ["regex.dynamic.compile"],
    },
    "unicode_empty_progress": {
        "domains": ["empty_match", "unicode"],
        "required_capabilities": [
            "regex.dynamic.compile",
            "regex.empty-match.scalar-progress",
        ],
    },
    "unicode_properties": {
        "domains": ["matching", "unicode"],
        "required_capabilities": ["regex.dynamic.compile", "regex.unicode.properties"],
    },
}
SEMANTIC_CONTRACT = {
    "empty_match_end_of_input": "emit-once-then-terminate",
    "empty_match_progress": "unicode-scalar-boundary",
    "find_all_zero_limit": "empty",
    "ignore_case": "ascii-only",
    "literal_suffix_flags": ["i", "m", "s"],
    "matching_priority": "leftmost-longest",
    "non_multiline_dollar": "strict-text-end",
    "split_zero_limit": "unlimited",
}
ENGINE_SNAPSHOT = {
    "empty_match_progress": "one-utf8-byte-unsafe-red-baseline",
    "error_codes": {
        "bad_class": 9,
        "bad_escape": 6,
        "bad_repeat": 4,
        "syntax": 1,
        "too_deep": 8,
        "too_many_groups": 7,
        "trailing_backslash": 5,
        "unmatched_bracket": 3,
        "unmatched_paren": 2,
        "unsupported": 10,
    },
    "instruction_schema": "array-i64-header16-instruction4-unicode3-v1",
    "limits": {"max_capture_groups": 31, "max_instructions": 10000, "max_nesting": 100},
    "owner": "stdlib/regex/regex.xr",
    "owner_sha256": "3ae33f32a1854fcafcf86f6f99c07013440ed7d221db6fde36003a3050aac7cf",
}
UNICODE_CONTRACT = {
    "case_fold": "ascii-only",
    "name": "xray-unicode-15.0-simplified-source-v1",
    "owner_digest_sha256": "d4114659635a0c6613cb2fc9bbcd4048428bd37cd3906e9ffee2eaf428b97974",
    "owners": ["src/base/xunicode.h", "src/base/xunicode.c"],
    "property_lookup": "exact-then-ascii-case-insensitive",
}
REQUIRED_NEGATIVE_IDS = {
    "constant_compile_flag_g",
    "dynamic_compile_flag_g",
    "dynamic_compile_unknown_flag",
    "invalid_repeat_range",
    "invalid_unclosed_class",
    "invalid_unclosed_group",
    "literal_duplicate_flag",
    "literal_flag_g",
    "literal_flag_u",
    "literal_flag_upper_u",
    "literal_invalid_pattern",
    "literal_unknown_flag",
    "resource_capture_limit",
    "unsupported_backreference",
    "unsupported_lookaround",
}
REQUIRED_MUTATION_IDS = {
    "bad_opcode",
    "bad_plan_flag_bits",
    "duplicate_capture",
    "fingerprint_mismatch",
    "forged_dfa_admission",
    "forged_literal_fact",
    "forged_onepass_witness",
    "forged_prefix_fact",
    "invalid_capture",
    "invalid_utf8_replace_input",
    "mismatched_plan_identity",
    "offset_count_overflow",
    "overlapping_tables",
    "pc_into_middle",
    "pc_out_of_bounds",
    "scratch_over_profile",
    "stale_schema_contract",
    "stale_unicode_contract",
    "truncated_capture_table",
    "truncated_range_table",
    "truncated_string_table",
    "unexpected_dynamic_operation",
    "zero_width_end_reentry",
}
REQUIRED_HAZARD_IDS = {"unicode_empty_replace_invalid_utf8"}
REQUIRED_TARGET_PLAN_MUTATION_ROUTES = {
    "scratch_over_profile": (
        "target_plan_admission",
        "target-plan.regex-scratch-budget",
    ),
    "unexpected_dynamic_operation": (
        "target_plan_admission",
        "target-plan.regex-dynamic-capability",
    ),
}
REQUIRED_INVENTORY_DIGESTS = {
    "hazard_inventory": "448c04402f090391a7f780ba5ede6c2efb65e50192aca9338d9fec7ce95c56ec",
    "mutation_inventory": "ecdf5d3706ac798d51e69605370b2221bda0f70fb9f262bc5c09095170ae2c64",
    "negative_inventory": "b1e9072e0c0fd5896dd1e940271a683425f6689b0452b7ca6a1e6ae0a1ea537a",
}


def _unique_json_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _load_json(payload: str) -> Any:
    return json.loads(payload, object_pairs_hook=_unique_json_object)


def _sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _owned_files_digest(paths: list[str]) -> str:
    digest = hashlib.sha256()
    for relative in paths:
        payload = (ROOT / relative).read_bytes()
        encoded = relative.encode("utf-8")
        digest.update(len(encoded).to_bytes(4, "little"))
        digest.update(encoded)
        digest.update(len(payload).to_bytes(8, "little"))
        digest.update(payload)
    return digest.hexdigest()


def _safe_relative(value: Any, base: Path, field: str, errors: list[str]) -> Path | None:
    if not isinstance(value, str) or not value or "\\" in value:
        errors.append(f"{field} must be a non-empty forward-slash relative path")
        return None
    relative = Path(value)
    if relative.is_absolute() or ".." in relative.parts:
        errors.append(f"{field} escapes the oracle root: {value!r}")
        return None
    resolved = (base / relative).resolve()
    try:
        common = Path(os.path.commonpath((str(base.resolve()), str(resolved))))
    except ValueError:
        common = Path()
    if common != base.resolve():
        errors.append(f"{field} escapes the oracle root: {value!r}")
        return None
    return resolved


def _sorted_unique_strings(value: Any, field: str, allowed: set[str],
                           errors: list[str]) -> list[str]:
    if not isinstance(value, list) or not value or any(not isinstance(x, str) for x in value):
        errors.append(f"{field} must be a non-empty string array")
        return []
    if value != sorted(set(value)):
        errors.append(f"{field} must be sorted and duplicate-free")
    unknown = sorted(set(value) - allowed)
    if unknown:
        errors.append(f"{field} has unknown values: {', '.join(unknown)}")
    return value


def _read_jsonl(path: Path, keys: set[str], label: str,
                errors: list[str]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    try:
        payload = path.read_bytes()
        if not payload or not payload.endswith(b"\n") or b"\r" in payload:
            errors.append(f"{label} must be non-empty LF-only bytes")
        lines = payload.decode("utf-8", errors="strict").splitlines()
    except (OSError, UnicodeError) as exc:
        errors.append(f"cannot read {label}: {exc}")
        return rows
    for line_number, line in enumerate(lines, start=1):
        if not line:
            errors.append(f"{label}:{line_number} is blank")
            continue
        try:
            row = _load_json(line)
        except (json.JSONDecodeError, ValueError) as exc:
            errors.append(f"{label}:{line_number} is invalid JSON: {exc}")
            continue
        if not isinstance(row, dict) or set(row) != keys:
            errors.append(f"{label}:{line_number} has a non-exact field set")
            continue
        rows.append(row)
    return rows


def _load_validated_oracle(
    manifest_path: Path = MANIFEST_PATH,
) -> tuple[dict[str, Any] | None, list[str]]:
    errors: list[str] = []
    base = manifest_path.parent.resolve()
    try:
        manifest_payload = manifest_path.read_bytes()
        if (not manifest_payload or not manifest_payload.endswith(b"\n")
                or b"\r" in manifest_payload):
            errors.append("oracle manifest must be non-empty LF-only bytes")
        manifest = _load_json(manifest_payload.decode("utf-8", errors="strict"))
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
        return None, [f"cannot read oracle manifest: {exc}"]
    if not isinstance(manifest, dict) or set(manifest) != MANIFEST_KEYS:
        return None, ["oracle manifest has a non-exact field set"]
    if type(manifest.get("schema")) is not int or manifest.get("schema") != 1:
        errors.append("oracle schema version is not exact")
    if manifest.get("oracle") != "xray-regex-absolute-oracle":
        errors.append("oracle manifest identity is not exact")
    if manifest.get("required_backends") != ["vm", "aot"]:
        errors.append("required_backends must be exactly ['vm', 'aot']")
    if (manifest.get("stdout") != "byte-exact-lf"
            or manifest.get("stderr") != "must-be-empty"
            or type(manifest.get("exit_code")) is not int
            or manifest.get("exit_code") != 0):
        errors.append("observable process contract is not exact")
    if manifest.get("semantic_contract") != SEMANTIC_CONTRACT:
        errors.append("regex semantic contract is not exact")
    if manifest.get("engine_snapshot") != ENGINE_SNAPSHOT:
        errors.append("regex engine snapshot is not exact")
    if manifest.get("unicode_contract") != UNICODE_CONTRACT:
        errors.append("regex Unicode contract is not exact")
    try:
        engine_owner_digest = _sha256((ROOT / ENGINE_SNAPSHOT["owner"]).read_bytes())
        if engine_owner_digest != ENGINE_SNAPSHOT["owner_sha256"]:
            errors.append("regex engine owner has drifted from the frozen snapshot")
        unicode_owner_digest = _owned_files_digest(UNICODE_CONTRACT["owners"])
        if unicode_owner_digest != UNICODE_CONTRACT["owner_digest_sha256"]:
            errors.append("regex Unicode owners have drifted from the frozen contract")
    except OSError as exc:
        errors.append(f"cannot verify regex contract owners: {exc}")

    cases = manifest.get("cases")
    if not isinstance(cases, list) or not cases:
        errors.append("cases must be a non-empty array")
        cases = []
    case_ids: list[str] = []
    case_sources: set[Path] = set()
    case_expected: set[Path] = set()
    covered_domains: set[str] = set()
    for index, case in enumerate(cases):
        label = f"cases[{index}]"
        if not isinstance(case, dict) or set(case) != CASE_KEYS:
            errors.append(f"{label} has a non-exact field set")
            continue
        case_id = case.get("id")
        if not isinstance(case_id, str) or CASE_ID_PATTERN.fullmatch(case_id) is None:
            errors.append(f"{label}.id is not canonical snake case")
        else:
            case_ids.append(case_id)
        domains = _sorted_unique_strings(case.get("domains"), f"{label}.domains", DOMAINS, errors)
        covered_domains.update(domains)
        _sorted_unique_strings(
            case.get("required_capabilities"),
            f"{label}.required_capabilities",
            CAPABILITIES,
            errors,
        )
        required_shape = REQUIRED_CASE_SHAPES.get(case_id)
        if (required_shape is None
                or any(case.get(key) != value for key, value in required_shape.items())):
            errors.append(f"{label} does not match its frozen semantic shape")
        source = _safe_relative(case.get("source"), base, f"{label}.source", errors)
        expected = _safe_relative(
            case.get("expected_stdout"), base, f"{label}.expected_stdout", errors
        )
        if source is not None:
            case_sources.add(source)
            if source.parent != base / "programs" or source.suffix != ".xr" or not source.is_file():
                errors.append(f"{label}.source is not a program fixture")
            elif isinstance(case_id, str) and source.name != f"{case_id}.xr":
                errors.append(f"{label}.source does not match its case id")
            else:
                try:
                    payload = source.read_bytes()
                    payload.decode("utf-8", errors="strict")
                    if not payload or not payload.endswith(b"\n") or b"\r" in payload:
                        errors.append(f"{label}.source must be non-empty LF-only bytes")
                    source_digest = case.get("source_sha256")
                    if (not isinstance(source_digest, str)
                            or DIGEST_PATTERN.fullmatch(source_digest) is None
                            or source_digest != _sha256(payload)):
                        errors.append(f"{label}.source_sha256 does not match the source")
                except (OSError, UnicodeError) as exc:
                    errors.append(f"{label}.source is not strict UTF-8: {exc}")
        if expected is not None:
            case_expected.add(expected)
            if (expected.parent != base / "expected"
                    or expected.suffix != ".stdout"
                    or not expected.is_file()):
                errors.append(f"{label}.expected_stdout is not an expected fixture")
            elif isinstance(case_id, str) and expected.name != f"{case_id}.stdout":
                errors.append(f"{label}.expected_stdout does not match its case id")
            else:
                payload = expected.read_bytes()
                if not payload or not payload.endswith(b"\n") or b"\r" in payload:
                    errors.append(f"{label}.expected_stdout must be non-empty LF-only bytes")
                expected_digest = case.get("expected_sha256")
                if (not isinstance(expected_digest, str)
                        or DIGEST_PATTERN.fullmatch(expected_digest) is None
                        or expected_digest != _sha256(payload)):
                    errors.append(f"{label}.expected_sha256 does not match the output")
    required_case_ids = sorted(REQUIRED_CASE_SHAPES)
    if case_ids != required_case_ids:
        errors.append("case ids do not match the frozen set")
    try:
        actual_sources = set((base / "programs").iterdir())
    except OSError as exc:
        errors.append(f"cannot inventory program fixtures: {exc}")
        actual_sources = set()
    try:
        actual_expected = set((base / "expected").iterdir())
    except OSError as exc:
        errors.append(f"cannot inventory expected outputs: {exc}")
        actual_expected = set()
    if case_sources != actual_sources:
        errors.append("program fixture inventory does not exactly match the manifest")
    if case_expected != actual_expected:
        errors.append("expected-output inventory does not exactly match the manifest")
    if covered_domains != DOMAINS:
        errors.append(f"executable domain coverage mismatch: {sorted(covered_domains)}")

    negative_path = _safe_relative(
        manifest.get("negative_inventory"), base, "negative_inventory", errors
    )
    mutation_path = _safe_relative(
        manifest.get("mutation_inventory"), base, "mutation_inventory", errors
    )
    hazard_path = _safe_relative(
        manifest.get("hazard_inventory"), base, "hazard_inventory", errors
    )
    negatives = (
        _read_jsonl(negative_path, NEGATIVE_KEYS, "negative_inventory", errors)
        if negative_path is not None and negative_path.is_file()
        else []
    )
    mutations = (
        _read_jsonl(mutation_path, MUTATION_KEYS, "mutation_inventory", errors)
        if mutation_path is not None and mutation_path.is_file()
        else []
    )
    hazards = (
        _read_jsonl(hazard_path, HAZARD_KEYS, "hazard_inventory", errors)
        if hazard_path is not None and hazard_path.is_file()
        else []
    )
    for label, path, digest_field in (
        ("negative_inventory", negative_path, "negative_inventory_sha256"),
        ("mutation_inventory", mutation_path, "mutation_inventory_sha256"),
        ("hazard_inventory", hazard_path, "hazard_inventory_sha256"),
    ):
        expected_digest = manifest.get(digest_field)
        if (not isinstance(expected_digest, str)
                or DIGEST_PATTERN.fullmatch(expected_digest) is None):
            errors.append(f"{digest_field} is not a canonical SHA-256 digest")
        elif path is None or not path.is_file():
            errors.append(f"{label} content cannot be read for its frozen digest")
        else:
            actual_digest = _sha256(path.read_bytes())
            if actual_digest != expected_digest:
                errors.append(f"{label} content does not match its manifest digest")
            if (actual_digest != REQUIRED_INVENTORY_DIGESTS[label]
                    or expected_digest != REQUIRED_INVENTORY_DIGESTS[label]):
                errors.append(f"{label} content does not match its frozen semantic digest")
    negative_ids = [row.get("id") for row in negatives]
    mutation_ids = [row.get("id") for row in mutations]
    hazard_ids = [row.get("id") for row in hazards]
    if (any(not isinstance(item, str) for item in negative_ids)
            or negative_ids != sorted(set(negative_ids))
            or set(negative_ids) != REQUIRED_NEGATIVE_IDS):
        errors.append("negative inventory ids do not match the frozen set")
    if (any(not isinstance(item, str) for item in mutation_ids)
            or mutation_ids != sorted(set(mutation_ids))
            or set(mutation_ids) != REQUIRED_MUTATION_IDS):
        errors.append("mutation inventory ids do not match the frozen set")
    if (any(not isinstance(item, str) for item in hazard_ids)
            or hazard_ids != sorted(set(hazard_ids))
            or set(hazard_ids) != REQUIRED_HAZARD_IDS):
        errors.append("hazard inventory ids do not match the frozen set")
    for row in negatives:
        pattern = row.get("pattern_utf8_hex")
        decoded_pattern: bytes | None = None
        try:
            if not isinstance(pattern, str):
                raise ValueError("pattern is not a string")
            if re.fullmatch(r"(?:[0-9a-f]{2})*", pattern) is None:
                raise ValueError("pattern is not canonical lowercase hex")
            decoded_pattern = bytes.fromhex(pattern)
            decoded_pattern.decode("utf-8", errors="strict")
        except (ValueError, UnicodeError):
            errors.append(f"negative inventory {row.get('id')}: pattern bytes are invalid UTF-8")
        flags = row.get("flags")
        offset = row.get("expected_offset")
        domain = row.get("expected_offset_domain")
        offset_limit = (
            len(decoded_pattern)
            if domain == "pattern_utf8" and decoded_pattern is not None
            else -1
        )
        if domain == "flags_ascii" and isinstance(flags, str):
            try:
                offset_limit = len(flags.encode("ascii", errors="strict"))
            except UnicodeError:
                offset_limit = -1
        if type(offset) is not int or offset < 0 or offset > offset_limit:
            errors.append(
                f"negative inventory {row.get('id')}: offset is not a valid UTF-8 byte offset"
            )
        elif domain == "pattern_utf8" and decoded_pattern is not None:
            try:
                decoded_pattern[:offset].decode("utf-8", errors="strict")
            except UnicodeError:
                errors.append(
                    f"negative inventory {row.get('id')}: offset splits a UTF-8 scalar"
                )
        for field in ("activation_owner", "expected_kind", "flags", "id", "surface"):
            if not isinstance(row.get(field), str):
                errors.append(f"negative inventory {row.get('id')}: {field} must be a string")
        if row.get("activation_owner") not in {"canonical_compiler", "frontend"}:
            errors.append(f"negative inventory {row.get('id')}: activation owner is not exact")
        if row.get("surface") not in {"constant_compile", "dynamic_compile", "literal"}:
            errors.append(f"negative inventory {row.get('id')}: surface is not exact")
        if not str(row.get("expected_kind", "")).startswith("regex."):
            errors.append(f"negative inventory {row.get('id')}: error kind is not stable")
    for row in mutations:
        if row.get("activation_owner") not in {
            "regex_executor",
            "regex_plan_verifier",
            "target_plan_admission",
        }:
            errors.append(f"mutation inventory {row.get('id')}: activation owner is not exact")
        for field in ("expected_rejection", "id", "mutation", "target_field"):
            if not isinstance(row.get(field), str) or not row[field]:
                errors.append(f"mutation inventory {row.get('id')}: {field} must be non-empty")
        required_route = REQUIRED_TARGET_PLAN_MUTATION_ROUTES.get(row.get("id"))
        if required_route is not None and (
            row.get("activation_owner"), row.get("expected_rejection")
        ) != required_route:
            errors.append(
                f"mutation inventory {row.get('id')}: TargetPlan admission route is not exact"
            )
    for row in hazards:
        for field in HAZARD_KEYS:
            if not isinstance(row.get(field), str):
                errors.append(f"hazard inventory {row.get('id')}: {field} must be a string")
        if (row.get("activation_owner") != "canonical_matcher"
                or row.get("state") != "red"
                or row.get("required_end_state")
                != "scalar-boundary-valid-utf8-or-typed-error-never-panic"):
            errors.append(f"hazard inventory {row.get('id')}: resolution contract is not exact")
        for field in ("input_utf8_hex", "pattern_utf8_hex", "replacement_utf8_hex"):
            encoded = row.get(field)
            try:
                if (not isinstance(encoded, str)
                        or re.fullmatch(r"(?:[0-9a-f]{2})*", encoded) is None):
                    raise ValueError("not canonical hex")
                bytes.fromhex(encoded).decode("utf-8", errors="strict")
            except (ValueError, UnicodeError):
                errors.append(f"hazard inventory {row.get('id')}: {field} is not UTF-8")
    return manifest, errors


def validate_oracle(manifest_path: Path = MANIFEST_PATH) -> list[str]:
    return _load_validated_oracle(manifest_path)[1]


def _run(argv: list[str], cwd: Path, timeout: int,
         env: dict[str, str] | None = None) -> subprocess.CompletedProcess[bytes]:
    try:
        return subprocess.run(
            argv,
            cwd=cwd,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            env=env,
        )
    except subprocess.TimeoutExpired as exc:
        return subprocess.CompletedProcess(argv, 124, exc.stdout or b"", exc.stderr or b"")


def _git_text(*args: str) -> str:
    return subprocess.check_output(
        ["git", *args], cwd=ROOT, text=True, encoding="utf-8", errors="strict"
    ).strip()


def verify_matching_binary(binary: Path) -> dict[str, Any]:
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise RuntimeError(f"xray executable is not runnable: {binary}")
    status = _git_text("status", "--porcelain", "--untracked-files=normal")
    if status:
        raise RuntimeError("source worktree is dirty; absolute evidence requires a clean commit")
    head = _git_text("rev-parse", "HEAD")
    cache = binary.parent / "CMakeCache.txt"
    if not cache.is_file():
        raise RuntimeError("xray binary is not in a worktree-local CMake build directory")
    cache_values: dict[str, str] = {}
    for line in cache.read_text(encoding="utf-8", errors="strict").splitlines():
        if "=" not in line or ":" not in line.split("=", 1)[0]:
            continue
        key, value = line.split("=", 1)
        cache_values[key.split(":", 1)[0]] = value
    if cache_values.get("CMAKE_HOME_DIRECTORY") != str(ROOT):
        raise RuntimeError("CMake build does not belong to this worktree")
    if cache_values.get("CMAKE_GENERATOR") != "Ninja":
        raise RuntimeError("absolute evidence requires the Ninja generator")
    if cache_values.get("CMAKE_BUILD_TYPE") != "Release":
        raise RuntimeError("absolute evidence requires a Release build")
    version = _run([str(binary), "--version", "--json"], ROOT, 30)
    if version.returncode != 0:
        raise RuntimeError("xray binary identity command failed")
    if version.stderr:
        raise RuntimeError("xray binary identity command wrote to stderr")
    try:
        identity = _load_json(version.stdout.decode("utf-8", errors="strict"))
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise RuntimeError(f"xray binary identity is invalid: {exc}") from exc
    if not isinstance(identity, dict):
        raise RuntimeError("xray binary identity is not an object")
    _validate_binary_identity(identity, head)
    return identity


def _validate_binary_identity(identity: dict[str, Any], head: str) -> None:
    if set(identity) != {
        "buildProfile",
        "commit",
        "dirty",
        "features",
        "product",
        "schema",
        "target",
        "version",
    }:
        raise RuntimeError("xray binary identity has a non-exact field set")
    if (type(identity.get("schema")) is not int or identity["schema"] != 1
            or identity.get("product") != "xray-lang"
            or not isinstance(identity.get("version"), str)
            or not identity["version"]
            or not isinstance(identity.get("target"), str)
            or not identity["target"]):
        raise RuntimeError("xray binary identity contract is not exact")
    if identity.get("commit") != head or identity.get("dirty") is not False:
        raise RuntimeError("xray binary does not match the clean source commit")
    if identity.get("buildProfile") != "Release":
        raise RuntimeError("xray binary does not report a Release profile")
    features = identity.get("features")
    if features not in (["vm", "aot"], ["vm", "aot", "tls"]):
        raise RuntimeError("xray binary feature identity is not exact")


def verify_native_provider(binary: Path, timeout: int,
                           env: dict[str, str]) -> dict[str, Any]:
    result = _run(
        [
            str(binary),
            "toolchain",
            "doctor",
            "--target",
            "native",
            "--profile",
            "hosted",
            "--json",
        ],
        ROOT,
        timeout,
        env,
    )
    if result.returncode != 0 or result.stderr:
        raise RuntimeError("native hosted provider identity command failed or wrote to stderr")
    try:
        data = _load_json(result.stdout.decode("utf-8", errors="strict"))
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise RuntimeError(f"native hosted provider identity is invalid: {exc}") from exc
    if not isinstance(data, dict):
        raise RuntimeError("native hosted provider identity is not an object")
    request = data.get("request")
    selection = data.get("selection")
    xray = data.get("xray")
    probe = data.get("probe")
    if (type(data.get("schema")) is not int or data["schema"] != 1
            or not isinstance(xray, dict)
            or not isinstance(xray.get("sdkDigest"), str)
            or not xray["sdkDigest"]
            or not isinstance(request, dict)
            or request.get("target") != "native"
            or request.get("profile") != "hosted"
            or not request.get("normalizedTarget")
            or not isinstance(selection, dict)
            or selection.get("ready") is not True
            or selection.get("fallbackUsed") is not False
            or not isinstance(selection.get("provider"), str)
            or not selection["provider"]
            or not isinstance(selection.get("ownership"), str)
            or not selection["ownership"]
            or not isinstance(selection.get("compiler"), str)
            or not selection["compiler"]
            or selection.get("targetAbi") != request.get("normalizedTarget")
            or not isinstance(selection.get("runtimeArtifact"), str)
            or not selection["runtimeArtifact"]
            or not isinstance(probe, dict)
            or not isinstance(probe.get("fingerprint"), str)
            or not probe["fingerprint"]
            or probe.get("cache") not in {"hit", "miss"}):
        raise RuntimeError("native hosted provider identity is not exact and ready")
    return data


def _provider_selector(provider: str) -> str:
    selectors = {
        "apple-clang": "clang",
        "gcc": "gcc",
        "llvm-clang": "clang",
        "msvc": "msvc",
        "zig": "zig",
    }
    if provider not in selectors:
        raise RuntimeError(f"native hosted provider has no exact build selector: {provider}")
    return selectors[provider]


def _toolchain_plan(stdout: bytes) -> dict[str, str]:
    try:
        lines = stdout.decode("utf-8", errors="strict").splitlines()
    except UnicodeError as exc:
        raise RuntimeError(f"native build output is not UTF-8: {exc}") from exc
    plan_lines = [line for line in lines if line.startswith("Toolchain plan:")]
    if len(plan_lines) != 1:
        raise RuntimeError("native build did not emit exactly one toolchain plan")
    pattern = re.compile(
        r"Toolchain plan: provider=(?P<provider>.*?) ownership=(?P<ownership>.*?) "
        r"target=(?P<target>.*?) compiler=(?P<compiler>.*?) runtime=(?P<runtime>.*?) "
        r"sdk=(?P<sdk>.*?) probe=(?P<probe>.*?) cache=(?P<cache>.*)\Z"
    )
    match = pattern.fullmatch(plan_lines[0])
    if match is None or any(not value for value in match.groupdict().values()):
        raise RuntimeError("native build toolchain plan is malformed")
    return match.groupdict()


def _verify_toolchain_plan(stdout: bytes, toolchain: dict[str, Any]) -> dict[str, str]:
    plan = _toolchain_plan(stdout)
    request = toolchain["request"]
    selection = toolchain["selection"]
    expected = {
        "compiler": selection["compiler"],
        "ownership": selection["ownership"],
        "probe": toolchain["probe"]["fingerprint"],
        "provider": selection["provider"],
        "runtime": selection["runtimeArtifact"],
        "sdk": toolchain["xray"]["sdkDigest"],
        "target": request["normalizedTarget"],
    }
    stable_plan = dict(plan)
    cache_state = stable_plan.pop("cache")
    if cache_state not in {"hit", "miss"}:
        raise RuntimeError("native build toolchain plan has an invalid cache observation")
    if stable_plan != expected:
        raise RuntimeError("native build toolchain plan does not match provider qualification")
    return plan


def _execute_vm(binary: Path, source: Path, timeout: int) -> subprocess.CompletedProcess[bytes]:
    return _run([str(binary), "run", str(source)], ROOT, timeout)


def _execute_aot(
    binary: Path,
    source: Path,
    output: Path,
    cache: Path,
    provider: str,
    timeout: int,
    env: dict[str, str] | None = None,
) -> tuple[subprocess.CompletedProcess[bytes], subprocess.CompletedProcess[bytes] | None]:
    build = _run(
        [
            str(binary),
            "build",
            "--native",
            "--dump-toolchain-plan",
            "--profile",
            "hosted",
            "--cache-dir",
            str(cache),
            "--toolchain",
            _provider_selector(provider),
            "-O",
            "2",
            "-o",
            str(output),
            str(source),
        ],
        ROOT,
        timeout,
        env,
    )
    if build.returncode != 0:
        return build, None
    return build, _run([str(output)], ROOT, timeout)


def run_oracle(binary: Path, timeout: int, manifest: dict[str, Any]) -> int:
    identity = verify_matching_binary(binary)
    backends = manifest["required_backends"]
    failures: list[str] = []
    provider_cache_states: set[str] = set()
    with tempfile.TemporaryDirectory(prefix="xray-regex-absolute-") as temp:
        work = Path(temp)
        provider_env = os.environ.copy()
        provider_cache = work / "provider-cache"
        cache_variable = "LOCALAPPDATA" if os.name == "nt" else "XDG_CACHE_HOME"
        provider_env[cache_variable] = str(provider_cache)
        toolchain = verify_native_provider(binary, timeout, provider_env)
        if (toolchain["xray"].get("build") != identity["commit"]
                or toolchain["xray"].get("version") != identity.get("version")):
            raise RuntimeError(
                "native provider qualification used a different Xray binary identity"
            )
        provider = toolchain["selection"]["provider"]
        for case in manifest["cases"]:
            source = ORACLE_ROOT / case["source"]
            source_payload = source.read_bytes()
            expected = (ORACLE_ROOT / case["expected_stdout"]).read_bytes()
            if (_sha256(source_payload) != case["source_sha256"]
                    or _sha256(expected) != case["expected_sha256"]):
                failures.append(f"{case['id']}: fixture changed after validation")
                continue
            source = work / f"{case['id']}.xr"
            source.write_bytes(source_payload)
            for selected in backends:
                if selected == "vm":
                    result = _execute_vm(binary, source, timeout)
                    build_stderr = b""
                else:
                    suffix = ".exe" if os.name == "nt" else ""
                    build, result = _execute_aot(
                        binary,
                        source,
                        work / f"{case['id']}{suffix}",
                        work / "aot-cache",
                        provider,
                        timeout,
                        provider_env,
                    )
                    build_stderr = build.stdout + build.stderr
                    if result is None:
                        failures.append(
                            f"{case['id']}/{selected}: native build failed rc={build.returncode}: "
                            f"{build_stderr.decode('utf-8', errors='backslashreplace')}"
                        )
                        continue
                    if build.stderr:
                        failures.append(
                            f"{case['id']}/{selected}: native build wrote to stderr: "
                            f"{build.stderr.decode('utf-8', errors='backslashreplace')}"
                        )
                    try:
                        build_toolchain = _verify_toolchain_plan(build.stdout, toolchain)
                        provider_cache_states.add(build_toolchain["cache"])
                    except RuntimeError as exc:
                        failures.append(
                            f"{case['id']}/{selected}: {exc}"
                        )
                if result.returncode != manifest["exit_code"]:
                    failures.append(f"{case['id']}/{selected}: exit code {result.returncode}")
                if result.stderr:
                    failures.append(
                        f"{case['id']}/{selected}: stderr is not empty: "
                        f"{result.stderr.decode('utf-8', errors='backslashreplace')}"
                    )
                if result.stdout != expected:
                    failures.append(
                        f"{case['id']}/{selected}: stdout mismatch\n"
                        f"expected={expected!r}\nactual={result.stdout!r}"
                    )
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    evidence_identity = {
        "build_profile": identity["buildProfile"],
        "commit": identity["commit"],
        "host_machine": platform.machine(),
        "host_system": platform.system(),
        "profile": "hosted",
        "provider": provider,
        "provider_compiler": toolchain["selection"]["compiler"],
        "provider_cache_states": sorted(provider_cache_states),
        "provider_ownership": toolchain["selection"]["ownership"],
        "provider_probe": toolchain["probe"]["fingerprint"],
        "provider_qualification_cache": toolchain["probe"]["cache"],
        "runtime_artifact": toolchain["selection"]["runtimeArtifact"],
        "sdk_digest": toolchain["xray"]["sdkDigest"],
        "target": toolchain["request"]["normalizedTarget"],
    }
    print(
        f"PASS: {len(manifest['cases'])} regex absolute cases x {len(backends)} backends "
        f"identity={json.dumps(evidence_identity, sort_keys=True, separators=(',', ':'))}"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validate-only", action="store_true")
    parser.add_argument("--xray", type=Path)
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    manifest, errors = _load_validated_oracle()
    if errors:
        for error in errors:
            print(f"FAIL: {error}", file=sys.stderr)
        return 1
    if args.validate_only:
        print("PASS: regex absolute oracle v1 manifest and inventories are exact")
        return 0
    if args.xray is None:
        parser.error("--xray is required unless --validate-only is used")
    if args.timeout < 1:
        parser.error("--timeout must be positive")
    try:
        assert manifest is not None
        return run_oracle(args.xray.resolve(), args.timeout, manifest)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
