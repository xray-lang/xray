#!/usr/bin/env python3
"""Explicit spec frontend regression receipts; this never certifies G13 execution.

The immutable receipts distinguish actual language rejects from historical
limitations and schematic-context observations. No source-comment classifier,
mode default, baseline rewrite, or run/AOT success is inferred here.
"""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import check_spec_xray_snippets as g13

ROLES = {"PARSE", "CHECK", "TRUE_ERROR", "KNOWN_LIMITATION", "DEFERRED"}
PRIMARY = re.compile(r"^(.+):(\d+):(\d+):\s*(.*)$")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def within(root, relative):
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError("receipt path escapes repository")
    return path


def validate_case(case):
    if not case.get("id") or case.get("language") not in ("cn", "en") or case.get("role") not in ROLES:
        raise ValueError("invalid explicit receipt identity, language or role")
    if case.get("mode") not in ("parse", "check") or not case.get("responsibility"):
        raise ValueError("frontend mode/responsibility must be explicit")
    if bool(case.get("source")) == bool(case.get("snapshot")):
        raise ValueError("exactly one source fence or original snapshot is required")
    if not re.fullmatch(r"[0-9a-f]{64}", case.get("body_sha256", "")):
        raise ValueError("missing complete body identity")
    expected = case.get("expected", {})
    if case["role"] == "DEFERRED":
        selected = case.get("source", {})
        expected_id = "json-boundary-and-path"
        if case.get("dependency") != "Task338" or case.get("required_for_task342") is not False or selected.get("path") != "spec/source/sections/015-14-built-in-type-methods.md" or selected.get("fence_id") != expected_id:
            raise ValueError("only the explicitly approved Task338 dependency can be deferred")
    else:
        if case.get("required_for_task342") is not True or expected.get("exit") not in (0, 1):
            raise ValueError("required receipt needs an actual exit oracle")
        positive = case["role"] in ("PARSE", "CHECK")
        if expected["exit"] != (0 if positive else 1):
            raise ValueError("receipt role disagrees with frontend outcome")
        if positive and case["mode"] != ("parse" if case["role"] == "PARSE" else "check"):
            raise ValueError("positive role disagrees with explicit frontend mode")
        if not positive:
            first = expected.get("first_diagnostic", {})
            if not all(key in first for key in ("path", "line", "column", "code", "message")):
                raise ValueError("reject/observation needs the complete exact first diagnostic")
            if first["path"] != "@fixture" or first["line"] < 1 or first["column"] < 1:
                raise ValueError("reject oracle must name its actual fixture and position")
            if not first["message"] or (first["code"] is not None and not re.fullmatch(r"E\d{4}", first["code"])):
                raise ValueError("invalid actual diagnostic code/message")
        if case["role"] == "KNOWN_LIMITATION" and not case.get("normative_oracle"):
            raise ValueError("known observation must retain its unmet normative responsibility")


def resolve_case(root, case, documents):
    validate_case(case)
    if case.get("snapshot"):
        path = within(root, case["snapshot"])
        code = path.read_text(encoding="utf-8")
        if code.endswith("\n"):
            code = code[:-1]
        origin = dict(source=str(path), line=1)
    else:
        selector = case["source"]
        path = within(root, selector["path"])
        if path not in documents:
            blocks, errors = g13.parse_document(path.read_text(encoding="utf-8"), str(path))
            if errors:
                raise ValueError("source fence extraction failed: " + repr(errors))
            documents[path] = blocks
        blocks = [block for block in documents[path] if not block["tagged"] or block["language"] == case["language"]]
        if selector.get("fence_id"):
            selected = [block for block in blocks if block["id"] == selector["fence_id"]]
            if len(selected) != 1:
                raise ValueError("source ID missing or ambiguous")
            block = selected[0]
        else:
            selected = [block for block in blocks if sha(block["code"].encode()) == case["body_sha256"]]
            if len(selected) != selector.get("matching_bodies") or not 0 <= selector.get("occurrence", -1) < len(selected):
                raise ValueError("anonymous source receipt is missing or ambiguous")
            block = selected[selector["occurrence"]]
        code = block["code"]
        origin = dict(source=str(path), line=block["line"], fence_id=block["id"], normative_markers=block["markers"])
    if sha(code.encode()) != case["body_sha256"]:
        raise ValueError("source body changed without an explicit receipt")
    return code, origin


def command_result(command, cwd, stdlib):
    binary = Path(command[0])
    result = dict(command=[str(x) for x in command], cwd=str(cwd), stdlib=str(stdlib), exit=None)
    try:
        result["binary_sha256_before"] = sha(binary.read_bytes())
        env = dict(os.environ, XRAY_STDLIB_PATH=str(stdlib), PYTHONUTF8="1")
        process = subprocess.run(result["command"], cwd=cwd, env=env, capture_output=True, timeout=30)
        result["exit"] = process.returncode
        for key, value in (("stdout", process.stdout), ("stderr", process.stderr)):
            result[key] = value.decode("utf-8").replace("\r\n", "\n")
        result["binary_sha256_after"] = sha(binary.read_bytes())
    except (OSError, UnicodeError) as error:
        result["tool_error"] = str(error)
    except subprocess.TimeoutExpired:
        result["timeout_seconds"] = 30
    return result


def judge(case, result, fixture):
    if result.get("tool_error") or result.get("timeout_seconds") or result.get("exit") not in (0, 1) or not result.get("binary_sha256_before") or result.get("binary_sha256_before") != result.get("binary_sha256_after"):
        return "TOOL_FAIL", None
    diagnostics = []
    for line in (result.get("stderr", "") + "\n" + result.get("stdout", "")).splitlines():
        matched = PRIMARY.match(line)
        if not matched:
            continue
        path, row, column, message = matched.groups()
        code = re.search(r"\bE\d{4}\b", message)
        same = os.path.normcase(os.path.abspath(path)) == os.path.normcase(str(fixture.resolve()))
        diagnostics.append(dict(path="@fixture" if same else path, line=int(row), column=int(column), message=message, code=code.group() if code else None))
    first = diagnostics[0] if diagnostics else None
    if case["role"] == "DEFERRED":
        return "DEFERRED_UNACCEPTED", first
    expected = case["expected"]
    if result["exit"] != expected["exit"]:
        return "LIMITATION_CHANGED" if case["role"] == "KNOWN_LIMITATION" else "FRONTEND_FAIL", first
    if expected["exit"] == 0:
        return (case["role"] + "_PASS" if not result.get("stdout") and not result.get("stderr") else "FRONTEND_FAIL"), first
    if len(diagnostics) != 1 or first != expected["first_diagnostic"]:
        return "FIRST_DIAGNOSTIC_FAIL", first
    return ("KNOWN_LIMITATION_STABLE" if case["role"] == "KNOWN_LIMITATION" else "TRUE_ERROR_PASS"), first


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xray", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--language", choices=("cn", "en"), required=True)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--report", type=Path)
    args = parser.parse_args(argv)
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    errors, rows, prepared, documents = [], [], [], {}
    if manifest.get("schema") != 1 or not manifest.get("fixed_source_commit"):
        errors.append("invalid receipt manifest")
    cases = [case for case in manifest.get("cases", []) if case.get("language") == args.language]
    if not cases or len({case.get("id") for case in cases}) != len(cases):
        errors.append("empty or duplicate receipt scope")
    protected = manifest.get("protected_receipts", {}).get(args.language, [])
    received = [receipt.get("receipt_id") for case in cases for receipt in case.get("receipts", [])]
    if not protected or len(set(protected)) != len(protected) or collections.Counter(protected) != collections.Counter(received):
        errors.append("protected original/current frontend responsibilities were lost or duplicated")
    for case in cases:
        try:
            code, origin = resolve_case(args.root, case, documents)
            prepared.append((case, code, origin))
        except (OSError, ValueError, KeyError, TypeError) as error:
            errors.append(case.get("id", "<missing>") + ": " + str(error))
    # A stale receipt fails before any compiler invocation. No partial scope
    # can masquerade as a complete admission regression result.
    if not errors:
        with tempfile.TemporaryDirectory(prefix="spec_frontend.") as temporary:
            for index, (case, code, origin) in enumerate(prepared):
                fixture = Path(temporary) / (str(index) + ".xr")
                fixture.write_text(code + "\n", encoding="utf-8")
                command = [str(args.xray.resolve()), "check"] + (["-S"] if case["mode"] == "parse" else []) + [str(fixture)]
                result = command_result(command, args.root, args.root / "stdlib")
                status, first = judge(case, result, fixture)
                rows.append(dict(id=case["id"], role=case["role"], origin=origin, body_sha256=case["body_sha256"], status=status, first_diagnostic=first, command=result, normative_g13_pass=False))
    failures = [row for row in rows if row["status"] not in ("PARSE_PASS", "CHECK_PASS", "TRUE_ERROR_PASS", "KNOWN_LIMITATION_STABLE", "DEFERRED_UNACCEPTED")]
    report = dict(schema=1, language=args.language, fixed_source_commit=manifest.get("fixed_source_commit"), counts=dict(collections.Counter(row["status"] for row in rows)), errors=errors, results=rows, gate_pass=not errors and not failures, certifies_full_g13=False, runtime_passes=0, known_limitations=sum(row["role"] == "KNOWN_LIMITATION" for row in rows), deferred=sum(row["role"] == "DEFERRED" for row in rows))
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("spec frontend regression (not full G13): " + json.dumps(report["counts"], sort_keys=True))
    for error in errors:
        print("RECEIPT_FAIL " + error)
    for row in failures:
        print(row["status"], row["id"], row["first_diagnostic"])
    return 0 if report["gate_pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
