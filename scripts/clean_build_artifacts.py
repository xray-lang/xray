#!/usr/bin/env python3
"""Remove reproducible products from explicitly managed, completed attempts.

Policy version 1 names direct attempt parents, a receipt path/key/accepted values,
request directories and relevant workspace/build process paths. No receipt path
is ever used as a deletion target. The default is dry-run; --apply is explicit.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import fnmatch
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import time
import uuid

PRODUCT_SUFFIXES = {".exe", ".pdb", ".obj", ".o"}
PROTECTED_DIRS = {"src", "source", "sources", "input", "inputs"}
TERMINAL_REQUESTS = {"finished", "failed", "cancelled"}


class UnsafeAttempt(Exception):
    pass


class RunLock:
    """Keep the lock file; only the OS lock conveys ownership."""
    def __init__(self, path: Path):
        self.path, self.stream = path, None

    def acquire(self) -> bool:
        if self.path.exists() and is_reparse(self.path.lstat()):
            raise ValueError("lock file is a reparse point")
        self.stream = self.path.open("a+b")
        if self.stream.seek(0, 2) == 0:
            self.stream.write(b"\0")
            self.stream.flush()
        self.stream.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(self.stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self.close()
            return False
        return True

    def close(self) -> None:
        if self.stream is not None:
            self.stream.close()
            self.stream = None


def is_reparse(info: os.stat_result) -> bool:
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & 0x400)


def no_reparse_path(path: Path) -> None:
    for part in (path, *path.parents):
        if is_reparse(part.lstat()):
            raise UnsafeAttempt(f"reparse point: {part}")


def contained(root: Path, path: Path) -> Path:
    no_reparse_path(path)
    resolved = path.resolve(strict=True)
    if resolved == root or not resolved.is_relative_to(root):
        raise UnsafeAttempt(f"target escapes managed root: {path}")
    return resolved


def relative_path(value: str) -> Path:
    path = Path(value)
    # Reject Windows spelling even when tests run on a Unix host.
    if not value or not path.parts or path.anchor or ":" in value or "\\" in value or ".." in path.parts:
        raise ValueError(f"expected a contained relative path: {value!r}")
    return path


def signature(info: os.stat_result) -> tuple:
    return (info.st_mode, info.st_size, info.st_mtime_ns, info.st_dev, info.st_ino,
            getattr(info, "st_file_attributes", 0))


def tree_snapshot(root: Path, attempt: Path) -> dict[str, tuple]:
    """Inspect every entry without following a directory link."""
    snapshot = {}
    pending = [attempt]
    while pending:
        path = pending.pop()
        contained(root, path)
        info = path.lstat()
        if is_reparse(info) or not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)):
            raise UnsafeAttempt(f"nonregular entry: {path}")
        snapshot[path.relative_to(attempt).as_posix()] = signature(info)
        if stat.S_ISDIR(info.st_mode):
            pending.extend(path.iterdir())
    return snapshot


def receipt_valid(data: bytes, receipt: dict) -> bool:
    value = json.loads(data)
    if not isinstance(value, dict):
        raise UnsafeAttempt("receipt must be a JSON object")
    if receipt.get("kind", "key") == "all_values":
        return bool(value) and all(name in value for name in receipt["required_keys"]) and all(
            item in receipt["accepted"] for item in value.values())
    for name in receipt["key"]:
        if not isinstance(value, dict) or name not in value:
            raise UnsafeAttempt("receipt key is missing")
        value = value[name]
    return value in receipt["accepted"]


@dataclass
class Attempt:
    root: Path
    path: Path
    snapshot: dict[str, tuple]
    receipt_path: Path
    receipt: bytes
    candidates: list[Path]
    newest_ns: int
    receipt_ns: int
    seal: tuple | None = None


def collect_attempt(root: Path, path: Path, rule: dict) -> Attempt:
    snapshot = tree_snapshot(root, path)
    choices = rule["receipt"] if isinstance(rule["receipt"], list) else [rule["receipt"]]
    for choice in choices:
        sealed = choice.get("kind", "key") == "sealed"
        approval = rule.get("_validated_attempts", {}).get(os.path.normcase(str(path))) if sealed else None
        if sealed and not isinstance(approval, dict):
            continue
        marker = approval["marker"] if sealed else choice["path"]
        receipt_path = path / relative_path(marker)
        receipt_name = receipt_path.relative_to(path).as_posix()
        if receipt_name not in snapshot:
            continue
        if not stat.S_ISREG(snapshot[receipt_name][0]):
            raise UnsafeAttempt("completion receipt is not a regular file")
        receipt = receipt_path.read_bytes()
        if signature(receipt_path.lstat()) != snapshot[receipt_name]:
            raise UnsafeAttempt("receipt changed while reading")
        if sealed:
            if hashlib.sha256(receipt).hexdigest() != approval["sha256"]:
                raise UnsafeAttempt("sealed marker SHA256 does not match approval")
        elif not receipt_valid(receipt, choice):
            raise UnsafeAttempt("receipt does not prove accepted completion")
        break
    else:
        raise UnsafeAttempt("no completion receipt or external sealed approval")
    if any(stat.S_ISREG(info[0]) and info[2] > snapshot[receipt_name][2] for info in snapshot.values()):
        raise UnsafeAttempt("regular file is newer than completion marker")
    if any(Path(name).name in {"CMakeCache.txt", "build.ninja", ".ninja_deps"} for name in snapshot):
        raise UnsafeAttempt("attempt contains a CMake/Ninja cache")
    native = set(rule["native_artifacts"])
    candidates = []
    for name, info in snapshot.items():
        item = Path(name)
        if name == receipt_name or not stat.S_ISREG(info[0]) or any(part.lower() in PROTECTED_DIRS for part in item.parts[:-1]):
            continue
        if any(word in item.name.lower() for word in ("command", "stdout", "stderr", "exit")):
            continue
        if item.suffix.lower() in PRODUCT_SUFFIXES or (not item.suffix and name in native):
            candidates.append(path / item)
    return Attempt(root, path, snapshot, receipt_path, receipt, sorted(candidates),
                   max(item[2] for item in snapshot.values()), snapshot[receipt_name][2], rule.get("_seal") if sealed else None)


def verify_receipt(attempt: Attempt) -> None:
    contained(attempt.root, attempt.receipt_path)
    expected = attempt.snapshot[attempt.receipt_path.relative_to(attempt.path).as_posix()]
    if signature(attempt.receipt_path.lstat()) != expected or attempt.receipt_path.read_bytes() != attempt.receipt or signature(attempt.receipt_path.lstat()) != expected:
        raise UnsafeAttempt("completion receipt changed after discovery")
    if attempt.seal:
        path, info, content = attempt.seal
        no_reparse_path(path)
        if signature(path.lstat()) != info or path.read_bytes() != content:
            raise UnsafeAttempt("external sealed approval changed after discovery")


def revalidate(attempt: Attempt) -> None:
    if tree_snapshot(attempt.root, attempt.path) != attempt.snapshot:
        raise UnsafeAttempt("attempt metadata changed after discovery")
    verify_receipt(attempt)


def delete_attempt(attempt: Attempt, emit) -> tuple[int, int]:
    revalidate(attempt)
    deleted, logical_bytes = 0, 0
    for path in attempt.candidates:
        contained(attempt.root, path)
        name = path.relative_to(attempt.path).as_posix()
        if signature(path.lstat()) != attempt.snapshot[name]:
            raise UnsafeAttempt(f"file metadata changed before deletion: {path}")
        verify_receipt(attempt)
        size = attempt.snapshot[name][1]
        path.unlink()
        deleted += 1
        logical_bytes += size
        emit({"attempt": str(attempt.path), "file": name, "status": "deleted", "logicalBytes": size})
    return deleted, logical_bytes


def windows_processes() -> list[dict]:
    if os.name != "nt":
        raise RuntimeError("actual process inspection is supported on Windows only")
    command = ("[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new(); "
               "@(Get-CimInstance Win32_Process | Select-Object ProcessId,Name,CommandLine) | ConvertTo-Json -Compress")
    result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
                            check=True, capture_output=True, timeout=20)
    data = json.loads(result.stdout.decode("utf-8-sig"))
    return data if isinstance(data, list) else [data]


def relevant_process(process: dict, paths: list[str]) -> bool:
    if process.get("ProcessId") == os.getpid():
        return False
    name = str(process.get("Name", "")).lower()
    tools = {"cmake.exe", "ctest.exe", "ninja.exe", "cl.exe", "clang.exe", "clang-cl.exe",
             "gcc.exe", "g++.exe", "link.exe", "lld-link.exe", "xray.exe", "python.exe", "pythonw.exe"}
    if name not in tools and not (name.startswith("test") and name.endswith(".exe")):
        return False
    command = str(process.get("CommandLine") or "").replace("\\", "/").lower()
    return any(re.search(re.escape(path.replace("\\", "/").rstrip("/").lower()) + r"(?=$|[/\s\"'])", command)
               for path in paths)


def busy_reasons(policy: dict, processes=None) -> list[str]:
    reasons = []
    for directory in policy["request_dirs"]:
        path = Path(directory)
        no_reparse_path(path)
        for request in path.glob("*.json"):
            try:
                no_reparse_path(request)
                value = json.loads(request.read_text(encoding="utf-8-sig"))
                if not isinstance(value, dict) or str(value.get("state", "")).lower() not in TERMINAL_REQUESTS:
                    reasons.append(f"active, recovery or unknown resource request: {request}")
            except (OSError, ValueError, UnsafeAttempt) as error:
                reasons.append(f"unreadable resource request {request}: {error}")
    try:
        for process in (windows_processes() if processes is None else processes):
            if relevant_process(process, policy["process_paths"]):
                reasons.append(f"relevant process {process.get('ProcessId')}: {process.get('Name')}")
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        reasons.append(f"process inspection unavailable: {error}")
    return reasons


def load_policy(path: Path) -> dict:
    policy = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(policy, dict) or policy.get("version") != 1 or not isinstance(policy.get("roots"), list) or not policy["roots"] or not isinstance(policy.get("process_paths"), list) or not policy["process_paths"]:
        raise ValueError("version 1 requires nonempty roots and process_paths")
    if not isinstance(policy.get("request_dirs"), list):
        raise ValueError("request_dirs must be an explicit list")
    keep = policy.setdefault("keep_successful", 2)
    age = policy.setdefault("min_age_hours", 48)
    if type(keep) is not int or keep < 2 or not isinstance(age, (int, float)) or not math.isfinite(age) or age < 0:
        raise ValueError("keep_successful must be >= 2 and min_age_hours must be finite and >= 0")
    for name in policy["process_paths"] + policy["request_dirs"]:
        if not isinstance(name, str) or not Path(name).is_absolute():
            raise ValueError("process_paths and request_dirs must contain absolute paths")
    for rule in policy["roots"]:
        if not isinstance(rule, dict) or not isinstance(rule.get("receipt"), (dict, list)) or not rule["receipt"]:
            raise ValueError("each root requires a receipt object or nonempty alternatives list")
        root = Path(rule["path"])
        if not root.is_absolute():
            raise ValueError("managed roots must be absolute")
        no_reparse_path(root)
        if not root.is_dir():
            raise ValueError(f"managed root is not a directory: {root}")
        rule["path"] = str(root.resolve(strict=True))
        pattern = rule["attempt_glob"]
        if not isinstance(pattern, str) or not pattern or "/" in pattern or "\\" in pattern or pattern in {".", ".."}:
            raise ValueError("attempt_glob must name direct children")
        choices = rule["receipt"] if isinstance(rule["receipt"], list) else [rule["receipt"]]
        for receipt in choices:
            if not isinstance(receipt, dict):
                raise ValueError("each receipt alternative must be an object")
            kind = receipt.get("kind", "key")
            if kind not in {"key", "all_values", "sealed"}:
                raise ValueError("receipt kind must be key, all_values or sealed")
            if kind != "sealed":
                relative_path(receipt["path"])
                if not isinstance(receipt["accepted"], list) or not receipt["accepted"] or not all(isinstance(item, str) and item for item in receipt["accepted"]):
                    raise ValueError("receipt accepted values must be nonempty strings")
            if kind == "key":
                key = receipt["key"]
                receipt["key"] = [key] if isinstance(key, str) else key
                if not isinstance(receipt["key"], list) or not receipt["key"] or not all(isinstance(item, str) and item for item in receipt["key"]):
                    raise ValueError("receipt key must be a string or nonempty key list")
            if kind == "all_values" and (not isinstance(receipt.get("required_keys"), list) or not receipt["required_keys"] or
                                         not all(isinstance(item, str) and item for item in receipt["required_keys"])):
                raise ValueError("all_values requires a nonempty required_keys list")
        rule.setdefault("native_artifacts", [])
        for item in rule["native_artifacts"]:
            if relative_path(item).suffix:
                raise ValueError("native_artifacts registers exact extensionless paths only")
    roots = [Path(rule["path"]) for rule in policy["roots"]]
    if any(left == right or left.is_relative_to(right) or right.is_relative_to(left)
           for index, left in enumerate(roots) for right in roots[index + 1:]):
        raise ValueError("managed roots must not overlap")
    sealed_rules = [rule for rule in policy["roots"] if any(item.get("kind") == "sealed" for item in
                    (rule["receipt"] if isinstance(rule["receipt"], list) else [rule["receipt"]]))]
    if sealed_rules:
        seal_path = Path(policy["validated_attempts"])
        if not seal_path.is_absolute() or any(seal_path.resolve().is_relative_to(root) for root in roots):
            raise ValueError("validated_attempts must be an absolute external file")
        no_reparse_path(seal_path)
        seal_info = signature(seal_path.lstat())
        content = seal_path.read_bytes()
        approvals = json.loads(content)
        if not isinstance(approvals, dict) or signature(seal_path.lstat()) != seal_info:
            raise ValueError("validated_attempts must be an unchanged JSON object")
        normalized = {}
        for name, approval in approvals.items():
            if not Path(name).is_absolute() or not isinstance(approval, dict):
                raise ValueError("sealed approvals map absolute attempt names to marker and sha256")
            relative_path(approval["marker"])
            if not re.fullmatch(r"[0-9a-f]{64}", approval["sha256"]):
                raise ValueError("sealed approval requires a lowercase SHA256")
            normalized[os.path.normcase(str(Path(name)))]=approval
        for rule in sealed_rules:
            rule.update(_validated_attempts=normalized, _seal=(seal_path, seal_info, content))
    return policy


def cleanup(policy: dict, apply: bool, report: dict, emit, processes=None) -> None:
    reasons = busy_reasons(policy, processes)
    if reasons:
        report.update(status="BUSY", busy=reasons)
        return
    cutoff = time.time_ns() - int(policy["min_age_hours"] * 3_600_000_000_000)
    for rule in policy["roots"]:
        root = Path(rule["path"])
        attempts = []
        for path in sorted(root.iterdir()):
            if not fnmatch.fnmatchcase(path.name, rule["attempt_glob"]):
                continue
            entry = {"attempt": str(path), "status": "protected"}
            report["attempts"].append(entry)
            try:
                info = path.lstat()
                if is_reparse(info) or not stat.S_ISDIR(info.st_mode):
                    raise UnsafeAttempt("attempt is not an ordinary directory")
                attempt = collect_attempt(root, path, rule)
                attempts.append((attempt, entry))
            except (OSError, ValueError, UnsafeAttempt) as error:
                entry["reason"] = str(error)
        attempts.sort(key=lambda item: (item[0].receipt_ns, item[0].path.name), reverse=True)
        for index, (attempt, entry) in enumerate(attempts):
            if index < policy["keep_successful"]:
                entry["reason"] = "latest successful attempts"
            elif attempt.newest_ns > cutoff:
                entry["reason"] = "attempt tree is newer than min_age_hours"
            else:
                entry.update(status="eligible", files=len(attempt.candidates),
                             logicalBytes=sum(attempt.snapshot[path.relative_to(attempt.path).as_posix()][1] for path in attempt.candidates))
                report["candidateFiles"] += entry["files"]
                report["candidateLogicalBytes"] += entry["logicalBytes"]
                if not apply:
                    continue
                try:
                    count, size = delete_attempt(attempt, emit)
                    entry.update(status="cleaned", deletedFiles=count, deletedLogicalBytes=size)
                except (OSError, ValueError, UnsafeAttempt) as error:
                    entry.update(status="error", reason=str(error))
                    report["status"] = "ERROR"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args(argv)
    policy_path = args.policy.resolve(strict=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    report_path = args.report or policy_path.parent / "cleanup-reports" / f"{stamp}-{os.getpid()}-{uuid.uuid4().hex[:8]}.json"
    report = {"version": 1, "startedUTC": stamp, "policy": str(policy_path), "apply": args.apply,
              "status": "APPLIED" if args.apply else "DRY_RUN", "attempts": [], "candidateFiles": 0,
              "candidateLogicalBytes": 0, "deletedFiles": 0, "deletedLogicalBytes": 0}
    lock = RunLock(policy_path.with_suffix(".cleanup.lock"))
    report_allowed = False
    try:
        if os.path.lexists(report_path) or os.path.lexists(report_path.with_suffix(".files.jsonl")):
            raise ValueError("report or corresponding file log already exists; choose a new report path")
        policy = load_policy(policy_path)
        for rule in policy["roots"]:
            root = Path(rule["path"])
            if policy_path.is_relative_to(root) or report_path.absolute().resolve().is_relative_to(root):
                raise ValueError("policy and reports must be outside managed roots")
        report_allowed = True
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report["diskFreeBefore"] = {rule["path"]: shutil.disk_usage(rule["path"]).free for rule in policy["roots"]}
        if not lock.acquire():
            report.update(status="BUSY", busy=["another cleanup owns the OS lock"])
        else:
            events_path = report_path.with_suffix(".files.jsonl")
            report["filesLog"] = str(events_path)
            with events_path.open("x", encoding="utf-8") as events:
                def emit(event):
                    events.write(json.dumps(event, ensure_ascii=False) + "\n")
                    events.flush()
                    report["deletedFiles"] += 1
                    report["deletedLogicalBytes"] += event["logicalBytes"]
                cleanup(policy, args.apply, report, emit)
        report["diskFreeAfter"] = {rule["path"]: shutil.disk_usage(rule["path"]).free for rule in policy["roots"]}
    except (OSError, ValueError, KeyError, TypeError, UnsafeAttempt) as error:
        report.update(status="ERROR", error=str(error))
    finally:
        lock.close()
        report["finishedUTC"] = datetime.now(timezone.utc).isoformat()
        if report_allowed:
            report_path.parent.mkdir(parents=True, exist_ok=True)
            with report_path.open("x", encoding="utf-8") as stream:
                stream.write(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "deletedFiles": report["deletedFiles"],
                      "deletedLogicalBytes": report["deletedLogicalBytes"], "report": str(report_path),
                      "error": report.get("error"), "busy": report.get("busy")}))
    return 2 if report["status"] == "BUSY" else 1 if report["status"] == "ERROR" else 0


if __name__ == "__main__":
    sys.exit(main())
