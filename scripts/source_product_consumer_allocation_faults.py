#!/usr/bin/env python3
"""Verify every original Source0 fault ordinal through independent shard processes."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import time
import traceback
import uuid

PREFIX = "source-allocation0 "
COMPILER_OOM = {name: 7 for name in (
    "source-product", "source-Checked-reader", "closed-Checked-reader", "closed-Checked-retained",
    "detached-source-verify", "detached-closed-verify", "retained-Checked-reread",
    "retained-Checked-verify", "Lowered", "Lowered-verify", "VM-Program-take")}
COMPILER_OOM.update({"owner-new": 3, "original-input": 7, "session-new": 3})
RUNTIME_OOM = COMPILER_OOM | {name: 6 for name in (
    "instance-new", "canonical-start", "original-test-start", "poll")}
SOURCE_FAMILIES = ("src", "include", "cmake", "stdlib", "spec", "xisa", "tools")
PACKET_STAGES = ("source-Checked-reader", "closed-Checked-reader", "retained-Checked-reread")
PACKET_ROLES = ("source", "closed", "retained")
COMPILER_STAGES = ("owner-new", "original-input", "session-new", "source-product",
                   "source-Checked-reader", "closed-Checked-reader", "closed-Checked-retained",
                   "detached-source-verify", "detached-closed-verify", "retained-Checked-reread",
                   "retained-Checked-verify", "Lowered", "Lowered-verify", "VM-Program-take")


def rows(output: str, tag: str) -> list[dict[str, str]]:
    result = []
    for line in output.splitlines():
        if not line.startswith(PREFIX + tag):
            continue
        tokens = line[len(PREFIX):].split()
        if "=" not in tokens[0]:
            tokens = tokens[1:]
        fields = {}
        for token in tokens:
            key, separator, value = token.partition("=")
            if not separator or not key or not value or key in fields:
                raise ValueError("malformed or duplicate field in " + tag)
            fields[key] = value
        result.append(fields)
    return result


def unsigned(value: str) -> int:
    if not re.fullmatch(r"[0-9]+", value):
        raise ValueError("invalid unsigned field: " + value)
    return int(value)


def zero_physical(row: dict[str, str]) -> None:
    if (row.get("compiler-physical") != "0/0" or row.get("runtime-physical") != "0/0"
            or row.get("table") != "0"):
        raise ValueError("fault physical stock or table was not zero")


def releases(output: str) -> list[dict[str, str]]:
    result = rows(output, "release ")
    for row in result:
        if (row.get("compiler") != "0/0" or row.get("runtime") != "0/0"
                or row.get("table") != "0" or row.get("result") != "PASS"):
            raise ValueError("original release did not prove five physical zeros")
    return result


def packet_statuses(stage: str, status: int) -> list[int]:
    if stage not in COMPILER_STAGES:
        if stage not in RUNTIME_OOM:
            raise ValueError("unknown packet observation boundary")
        return [0, 0, 0]
    at = COMPILER_STAGES.index(stage)
    return [status if name == stage else 0 for name in PACKET_STAGES
            if COMPILER_STAGES.index(name) <= at]


def packet_contract(output: str, summary: dict[str, str], statuses: list[int],
                    reference: list[dict[str, str]] | None = None) -> None:
    records = rows(output, "packet-input ")
    if (len(records) != len(statuses) or unsigned(summary["packet-reads"]) != len(statuses)
            or summary.get("input-packet-bytes") != ("PRESERVED" if statuses else "NOT_REACHED")):
        raise ValueError("packet preservation summary does not match the reached readers")
    for index, (record, status) in enumerate(zip(records, statuses)):
        if (record.get("role") != PACKET_ROLES[index] or record.get("preserved") != "1"
                or not 0 < unsigned(record["bytes"]) <= 65536 or unsigned(record["status"]) != status):
            raise ValueError("complete packet bytes, reader order or actual status was not preserved")
        if reference is not None and record["bytes"] != reference[index]["bytes"]:
            raise ValueError("packet width differs from the unfaulted original input")
    if len(records) == 3 and records[1]["bytes"] != records[2]["bytes"]:
        raise ValueError("the retained Closed packet changed length")


def normal_contract(output: str) -> tuple[int, int, int]:
    normal = rows(output, "normal=")
    if len(normal) != 1 or normal[0].get("normal") != "PASS":
        raise ValueError("missing unique original normal PASS")
    row = normal[0]
    if row.get("full-eight-source-FI") != "NOT_RUN" or row.get("native") != "NOT_RUN":
        raise ValueError("normal widened its qualification boundary")
    n, r = unsigned(row["compiler-sites"]), unsigned(row["runtime-sites"])
    if not n or not r:
        raise ValueError("normal allocation denominator is empty")
    for field in ("allocated", "peak", "work"):
        unsigned(row[field])
    end = output.index(PREFIX + "normal=")
    before = output[:end]
    packet_contract(before, row, [0, 0, 0])
    trace = rows(before, "runtime-call ")
    if (len(trace) != 43 or [unsigned(call["record"]) for call in trace] != list(range(43))
            or any(call.get("hit") != "0" or unsigned(call["status"]) not in (0, 1) for call in trace)):
        raise ValueError("original normal did not complete its 43 unfaulted records")
    if len(releases(before)) != 1:
        raise ValueError("normal owner was not released exactly once")
    return n, r, len(trace)


def fault_contract(row: dict[str, str], kind: str, site: int, normal: tuple[int, int, int]) -> None:
    tag, frozen = ("compiler-fault", "frozen-N") if kind == "compiler" else ("runtime-fault", "frozen-R")
    total = normal[0] if kind == "compiler" else normal[1]
    if row.get(tag) != "PASS" or unsigned(row[frozen]) != total or unsigned(row["fault-site"]) != site:
        raise ValueError("original fault PASS was assigned to a different ordinal or denominator")
    zero_physical(row)
    if row.get("output-preserved") != "1" or row.get("cleanup-uncharged") != "1":
        raise ValueError("original output or uncharged cleanup obligation failed")
    expected = (COMPILER_OOM if kind == "compiler" else RUNTIME_OOM).get(row["failure-stage"])
    actual = "actual-status" if kind == "compiler" else "status"
    if expected is None or unsigned(row[actual]) != expected or unsigned(row["expected-oom"]) != expected:
        raise ValueError("fault returned a different domain status")
    if any(row.get(k) != "NOT_RUN" for k in ("full-eight-source-FI", "axes", "native")):
        raise ValueError("fault widened an original OPEN boundary")
    unsigned(row["compiler-sites"]); unsigned(row["runtime-sites"])
    if kind == "compiler":
        if (row.get("hit-transitions") != "1" or row.get("diagnostic-match") != "1"
                or row.get("runtime-FI") != "NOT_RUN"):
            raise ValueError("compiler fault lacks its unique real hit or diagnostic obligation")
        for field, limit in (("allocated", 67108864), ("peak", 8388608), ("work", 128000000)):
            if unsigned(row[field]) > limit:
                raise ValueError("compiler fault exceeded its unchanged finite budget")
    else:
        if row.get("hit-windows") != "1" or row.get("downstream") != "NOT_ENTERED":
            raise ValueError("runtime fault lacks its unique real hit or stopped downstream")
        unsigned(row["record"])
        if row.get("borrowed-oom-shape") != ("1" if row["failure-stage"] == "poll" else "0"):
            raise ValueError("runtime poll borrowed OOM obligation differs")


# These are the operations assigned by the original immutable runtime body.
RUNTIME_OPERATION = {
    "owner-new": "finite-owner", "original-input": "original-input", "session-new": "session",
    "source-product": "source-product", "source-Checked-reader": "source-packet",
    "closed-Checked-reader": "closed-packet", "closed-Checked-retained": "closed-packet",
    "detached-source-verify": "producers-destroyed", "detached-closed-verify": "producers-destroyed",
    "retained-Checked-reread": "producers-destroyed", "retained-Checked-verify": "producers-destroyed",
    "Lowered": "Lowered", "Lowered-verify": "Lowered", "VM-Program-take": "VM-Program-take",
    "instance-new": "Program-sealed", "canonical-start": "canonical-entry",
    "original-test-start": "original-checkAnswer"}
RUNTIME_DIAGNOSTIC = re.compile(
    r"source-allocation0 failure operation=(?P<operation>[^\s]+) owner-status=(?P<owner>[0-9]+) "
    r"status=(?P<status>[0-9]+) source-stage=[0-9]+ source-status=[0-9]+ module=[0-9]+ "
    r"line=-?[0-9]+ column=-?[0-9]+ xir-status=[0-9]+ function=[0-9]+ block=[0-9]+ "
    r"instruction=[0-9]+ reason=[0-9]+ call-status=(?P<call>[0-9]+) message=[^\r\n\x00]*")


def runtime_blocks(output: str) -> list[tuple[dict[str, str], list[dict[str, str]]]]:
    lines = output.splitlines()
    normal_ends = [i for i, line in enumerate(lines) if line.startswith(PREFIX + "normal=")]
    if len(normal_ends) != 1:
        raise ValueError("runtime stream has no unique normal boundary")
    blocks, trace, fault = [], [], None
    for line in lines[normal_ends[0] + 1:]:
        if line.startswith(PREFIX + "runtime-call "):
            if fault is not None:
                raise ValueError("runtime raw call entered downstream of the stopped fault")
            trace += rows(line, "runtime-call ")
        elif line.startswith(PREFIX + "runtime-fault="):
            if fault is not None:
                raise ValueError("duplicate fault summary before its ordinal")
            fault = rows(line, "runtime-fault=")[0]
        elif line.startswith(PREFIX + "fault-ordinal "):
            if fault is None:
                raise ValueError("runtime ordinal has no original fault summary")
            blocks.append((fault, trace)); trace, fault = [], None
        elif line.startswith(PREFIX + "fault-summary ") and (trace or fault is not None):
            raise ValueError("shard summary has unassigned runtime raw records")
    if trace or fault is not None:
        raise ValueError("runtime raw records have no completed ordinal boundary")
    return blocks


def runtime_trace_contract(trace: list[dict[str, str]], fault: dict[str, str]) -> dict[str, str]:
    last = unsigned(fault["record"]); site = unsigned(fault["fault-site"])
    if len(trace) != last + 1 or any(unsigned(row["record"]) != i for i, row in enumerate(trace)):
        raise ValueError("runtime records are not continuous from zero through the original fault record")
    boundary = 0
    for i, row in enumerate(trace):
        begin, end = unsigned(row["site-begin"]), unsigned(row["site-end"])
        if begin != boundary or end < begin:
            raise ValueError("runtime allocation windows are not continuous")
        boundary = end
        if row.get("hit") != ("1" if i == last else "0"):
            raise ValueError("runtime ordinal does not have a unique last-record hit")
        if i != last and (end > site or unsigned(row["status"]) not in (0, 1)):
            raise ValueError("an earlier runtime record crossed the fault site or failed")
    hit = trace[-1]; stage = fault["failure-stage"]
    expected = RUNTIME_OOM.get(stage)
    if (expected is None or hit.get("stage") != stage or unsigned(hit["status"]) != expected
            or unsigned(hit["expected-oom"]) != expected or hit["status"] != fault["status"]
            or hit["expected-oom"] != fault["expected-oom"]
            or not unsigned(hit["site-begin"]) <= site < unsigned(hit["site-end"])):
        raise ValueError("runtime hit stage/status/domain/window is not bound to its original fault summary")
    return hit


def stderr_contract(stderr: bytes, kind: str, output: str) -> None:
    if kind != "runtime":
        if stderr:
            raise ValueError("unexpected normal/compiler stderr; raw diagnostics retained")
        return
    lines = stderr.decode("utf-8").splitlines()
    blocks = runtime_blocks(output)
    if len(lines) != len(blocks):
        raise ValueError("runtime stderr is not exactly one original diagnostic per ordinal")
    for line, (fault, trace) in zip(lines, blocks):
        hit = runtime_trace_contract(trace, fault)
        match = RUNTIME_DIAGNOSTIC.fullmatch(line)
        if match is None or "Sanitizer" in line or "runtime error:" in line:
            raise ValueError("runtime stderr contains unknown or sanitizer information")
        stage = fault["failure-stage"]
        operation = RUNTIME_OPERATION.get(stage)
        if stage == "poll":
            repeat = unsigned(hit["repeat"])
            if repeat == 4294967295:
                operation = "canonical-entry"
            elif repeat in (0, 1):
                operation = "original-checkAnswer"
            else:
                raise ValueError("poll diagnostic has an invalid original repeat role")
        if operation is None or match["operation"] != operation:
            raise ValueError("runtime diagnostic operation is not the original fault-stage operation")
        if stage == "owner-new" and unsigned(match["owner"]) != 3:
            raise ValueError("runtime owner diagnostic does not report owner OOM")
        if stage in COMPILER_OOM and stage not in ("owner-new", "original-input", "session-new"):
            if unsigned(match["status"]) != 7:
                raise ValueError("runtime XIR diagnostic does not report XIR OOM")
        if stage in ("instance-new", "canonical-start", "original-test-start", "poll"):
            if unsigned(match["call"]) != 6:
                raise ValueError("runtime Call diagnostic does not report Call OOM")


def shard_contract(output: str, kind: str, index: int, jobs: int,
                   normal: tuple[int, int, int]) -> list[int]:
    if normal_contract(output) != normal:
        raise ValueError("shard normal N/R/records drifted from two fresh normals")
    summaries = rows(output, "fault-summary ")
    if len(summaries) != 1:
        raise ValueError("missing unique shard summary")
    summary = summaries[0]; zero_physical(summary)
    total = normal[0] if kind == "compiler" else normal[1]
    expected_count = 0 if index >= total else (total - 1 - index) // jobs + 1
    fields = {"kind": kind, "frozen-N": str(normal[0]), "frozen-R": str(normal[1]),
              "normal-records": str(normal[2]), "shard": str(index), "shards": str(jobs),
              "ordinal-start": str(index), "ordinal-limit": str(total), "ordinal-step": str(jobs),
              "covered": str(expected_count), "scope": "FOCUSED" if jobs == total else "SHARD"}
    if any(summary.get(key) != value for key, value in fields.items()):
        raise ValueError("shard summary denominator, scope or exact ordinal domain differs")
    ordinals = rows(output, "fault-ordinal ")
    faults = rows(output, kind + "-fault=")
    other = "runtime" if kind == "compiler" else "compiler"
    if (len(ordinals) != expected_count or len(faults) != expected_count
            or rows(output, other + "-fault=")):
        raise ValueError("missing, duplicated or cross-domain original fault/ordinal")
    covered = []
    for position, (ordinal, fault) in enumerate(zip(ordinals, faults)):
        site = index + position * jobs; zero_physical(ordinal)
        for key in ("kind", "frozen-N", "frozen-R", "shard", "shards"):
            if ordinal.get(key) != fields[key]:
                raise ValueError("ordinal belongs to a different shard or census")
        if unsigned(ordinal["ordinal"]) != site:
            raise ValueError("ordinal sequence does not equal its exact range")
        fault_contract(fault, kind, site, normal); covered.append(site)
    tail = output.split(PREFIX + "normal=", 1)[1].split("\n", 1)[1]
    chunks = tail.split(PREFIX + "fault-ordinal ")
    if len(chunks) != expected_count + 1 or rows(chunks[-1], "packet-input "):
        raise ValueError("packet records have no matching original fault ordinal")
    for chunk, fault in zip(chunks, faults):
        status = unsigned(fault["actual-status" if kind == "compiler" else "status"])
        reference = rows(output.split(PREFIX + "normal=", 1)[0], "packet-input ")
        packet_contract(chunk, fault, packet_statuses(fault["failure-stage"], status), reference)
    if len(releases(output)) != expected_count + 1:
        raise ValueError("each original normal/fault did not release its own owners")
    if kind == "compiler":
        cleanups = rows(output, "compiler-cleanup ")
        if len(cleanups) != expected_count:
            raise ValueError("missing unique original compiler cleanup")
        pairs = (("sites-before", "sites-after"), ("runtime-sites-before", "runtime-sites-after"),
                 ("count-before", "count-owner-before-release"), ("allocated-before", "allocated-owner-before-release"),
                 ("peak-before", "peak-owner-before-release"), ("work-before", "work-owner-before-release"))
        for cleanup in cleanups:
            if cleanup.get("cumulative-uncharged") != "1" or any(
                    unsigned(cleanup[a]) != unsigned(cleanup[b]) for a, b in pairs):
                raise ValueError("compiler cleanup charged a cumulative axis")
            if unsigned(cleanup["live-owner-before-release"]) > unsigned(cleanup["live-before"]):
                raise ValueError("compiler cleanup grew live stock")
    else:
        blocks = runtime_blocks(output)
        if len(blocks) != expected_count or [fault for fault, trace in blocks] != faults:
            raise ValueError("runtime raw blocks are not bound to the exact fault ordinals")
        for fault, trace in blocks:
            runtime_trace_contract(trace, fault)
    return covered


def load_helpers(input_root: Path):
    path = input_root / "scripts/source_product_consumer_faults.py"
    spec = importlib.util.spec_from_file_location("source0_original_fault_helpers", path)
    if spec is None or spec.loader is None:
        raise ValueError("original helper module is unavailable")
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    if module.ROOT.resolve() != input_root:
        raise ValueError("original helper ROOT differs from the actual source root")
    return module


class CaptureGitError(ValueError):
    def __init__(self, message: str, evidence: dict):
        super().__init__(message)
        self.evidence = evidence


def capture_inputs(args, helpers) -> dict:
    root = args.input_root
    source = args.source_file.read_text(encoding="utf-8")
    expected = re.search(r"XR_XIR_CHECKED_SCHEMA == ([0-9]+)u && XR_XIR_CHECKED_CONTRACT == ([0-9]+)u", source)
    checked = (root / "src/xir/xxir_checked.h").read_text(encoding="utf-8")
    identity_matches = [re.search(r"#define XR_XIR_CHECKED_" + key + r" ([0-9]+)u", checked)
                        for key in ("SCHEMA", "CONTRACT")]
    if any(match is None for match in identity_matches):
        raise ValueError("actual producer Checked identity is unavailable")
    identity = tuple(match.group(1) for match in identity_matches)
    if expected is None or identity != expected.groups():
        raise ValueError("actual producer schema/contract differs from the compiled consumer source")
    command = ["git", "--no-optional-locks", "ls-files", "-z"]
    try:
        git = subprocess.run(command, cwd=root, capture_output=True, timeout=10)
    except (OSError, subprocess.SubprocessError) as error:
        stdout, stderr = getattr(error, "stdout", None) or b"", getattr(error, "stderr", None) or b""
        if isinstance(stdout, str): stdout = stdout.encode("utf-8")
        if isinstance(stderr, str): stderr = stderr.encode("utf-8")
        evidence = {"argv": command, "cwd": str(root), "returncode": getattr(error, "returncode", None),
                    "timed_out": isinstance(error, subprocess.TimeoutExpired), "timeout_seconds": 10,
                    "output_encoding": "hex", "stdout_hex": stdout.hex(), "stderr_hex": stderr.hex()}
        raise CaptureGitError("actual source root git ls-files failed or timed out: " + str(error), evidence) from error
    if git.returncode:
        evidence = {"argv": command, "cwd": str(root), "returncode": git.returncode,
                    "timed_out": False, "timeout_seconds": 10, "output_encoding": "hex",
                    "stdout_hex": git.stdout.hex(), "stderr_hex": git.stderr.hex()}
        raise CaptureGitError("actual source root git ls-files failed", evidence)
    tracked_count = len([p for p in git.stdout.split(b"\0") if p])
    # Use the same real tracked list for the original input families without another Git child.
    paths = git.stdout.decode("utf-8").split("\0")
    paths += [p.relative_to(root).as_posix() for p in (root / "tests/unit/xir/product_consumers").rglob("*")
              if p.is_file()]
    paths += [p.relative_to(root).as_posix() for p in (root / "scripts").glob("source_product_consumer_*.py")]
    paths += ["tests/unit/program/source_product_consumers.cmake"]
    files = {name: helpers.digest(root / name) for name in sorted(set(paths)) if name and (root / name).is_file()}
    files[str(args.binary)] = helpers.digest(args.binary)
    families = {}
    for name in SOURCE_FAMILIES:
        directory = root / name; count = 0
        if directory.is_dir():
            for path in sorted(directory.rglob("*")):
                if path.is_file():
                    files[path.relative_to(root).as_posix()] = helpers.digest(path); count += 1
        families[name] = {"exists": directory.is_dir(), "files": count}
    if not families["src"]["files"] or not (root / "CMakeLists.txt").is_file():
        raise ValueError("actual source root lacks producer source/CMake inputs")
    for name in ("CMakeLists.txt", "CMakePresets.json"):
        path = root / name
        if path.is_file():
            files[name] = helpers.digest(path)
    dynamic = [p for p in sorted(args.root.rglob("*.xr")) if p.is_file()]
    required = [args.binary, args.file, args.source_file, args.registration_file,
                Path(__file__).resolve(), root / "scripts/source_product_consumer_faults.py"]
    for path in dynamic + required:
        files[str(path.resolve(strict=True))] = helpers.digest(path)
    metadata = {}
    for name in files:
        stat = (root / name).stat()
        metadata[name] = {"bytes": stat.st_size, "mtime_ns": stat.st_mtime_ns}
    return {"source_root": str(root), "source_identity": {"checked_schema": identity[0], "checked_contract": identity[1]},
            "file_metadata": metadata,
            "capture_source": "git-tracked-plus-bounded-sources" if tracked_count else "git-empty-bounded-SourceMirror",
            "original_helper": str(root / "scripts/source_product_consumer_faults.py"),
            "git_ls_files": {"argv": command, "returncode": git.returncode, "tracked_count": tracked_count,
                             "timeout_seconds": 10,
                             "stdout_sha256": hashlib.sha256(git.stdout).hexdigest(),
                             "stderr_sha256": hashlib.sha256(git.stderr).hexdigest()},
            "bounded_families": families, "dynamic_sources": [str(p) for p in dynamic], "files": files,
            "qualification_boundary": "Input capture only; actual Ninja dependencies/public libraries/sanitizer support and Main qualification require independent evidence."}


def stop_process(process, deadline: float) -> tuple[int | None, list[str]]:
    errors = []
    try:
        process.kill()
    except (OSError, subprocess.SubprocessError) as error:
        errors.append("kill: " + str(error))
    try:
        code = process.wait(timeout=max(0.0, deadline - time.monotonic()))
    except (OSError, subprocess.SubprocessError) as error:
        code = None; errors.append("bounded reap: " + str(error))
    return code, errors


def process_run(command: list[str], label: str, run: Path, deadline: float, helpers, cwd: Path) -> dict:
    stdout = run / (label + ".stdout.raw"); stderr = run / (label + ".stderr.raw")
    started = time.monotonic()
    result = {"label": label, "argv": command, "returncode": None, "timed_out": False,
              "pid": None, "entered": False, "launch_error": None, "execution_error": None,
              "termination_errors": [], "termination_observed": False,
              "started_at": datetime.now(timezone.utc).isoformat(),
              "stdout": str(stdout), "stderr": str(stderr),
              "budget_clock_origin": "process_run entry",
              "wait_budget_seconds": max(0.0, deadline - started),
              "reap_grace_seconds": 8,
              "reap_deadline_seconds": max(0.0, deadline + 8 - started)}
    with stdout.open("wb") as out, stderr.open("wb") as err:
        helpers.write(run / (label + ".json"), result)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            result["timed_out"] = True
        else:
            process = None
            try:
                process = subprocess.Popen(command, cwd=cwd, stdout=out, stderr=err)
                result.update(pid=process.pid, entered=True)
                # Keep the actual PID receipt even if waiting or reaping fails.
                helpers.write(run / (label + ".json"), result)
                try:
                    result["returncode"] = process.wait(timeout=max(0.0, deadline - time.monotonic()))
                except subprocess.TimeoutExpired as error:
                    result["timed_out"] = True; result["execution_error"] = str(error)
                except (OSError, subprocess.SubprocessError) as error:
                    result["execution_error"] = str(error)
            except (OSError, subprocess.SubprocessError) as error:
                key = "execution_error" if result["entered"] else "launch_error"
                result[key] = str(error)
            finally:
                if process is not None and result["returncode"] is None:
                    result["returncode"], result["termination_errors"] = stop_process(process, deadline + 8)
                result["termination_observed"] = result["entered"] and result["returncode"] is not None
    result.update(elapsed_seconds=time.monotonic() - started,
                  stdout_sha256=helpers.digest(stdout), stderr_sha256=helpers.digest(stderr))
    helpers.write(run / (label + ".json"), result)
    return result


def process_contract(process: dict) -> None:
    if (process["returncode"] != 0 or process["timed_out"] or not process["termination_observed"]
            or process["launch_error"] or process["execution_error"] or process["termination_errors"]):
        raise ValueError("process failed, timed out or has unobserved termination; raw first failure retained")


def process_failure(command: list[str], label: str, run: Path, error: Exception, helpers) -> dict:
    receipt = run / (label + ".json")
    result = {"label": label, "argv": command, "entered": None, "returncode": None,
              "pid": None, "termination_observed": False}
    try:
        if receipt.is_file():
            raw = receipt.read_bytes()
            (run / (label + ".exception-receipt.raw")).write_bytes(raw)
            known = json.loads(raw)
            if isinstance(known, dict):
                result.update(known)
    except Exception as receipt_error:
        result["receipt_read_error"] = str(receipt_error)
    result["parse_issue"] = "runner exception: " + str(error)
    result["runner_exception"] = {"type": type(error).__name__, "message": str(error),
                                  "traceback": traceback.format_exc()}
    try:
        helpers.write(run / (label + ".runner-error.json"), result)
    except Exception as write_error:
        result["receipt_write_error"] = str(write_error)
    return result


def finish_report(report: dict, run: Path, helpers) -> None:
    try:
        helpers.write(run / "result.json", report)
    except Exception as error:
        report["status"] = "FAIL"
        report["issues"].append("final report write failed: " + str(error))
    print(json.dumps(report, ensure_ascii=False))
    if report["status"] != "PASS":
        raise SystemExit(1)


def self_test() -> None:
    packets = "".join(PREFIX + f"packet-input role={role} bytes=10 status=0 preserved=1\n"
                      for role in PACKET_ROLES)
    def sample(index: int, jobs: int) -> str:
        normal = packets + "".join(PREFIX + f"runtime-call record={i} status=0 hit=0\n" for i in range(43))
        release = PREFIX + "release compiler=0/0 runtime=0/0 table=0 result=PASS\n"
        normal += release + PREFIX + ("normal=PASS compiler-sites=2 runtime-sites=3 allocated=10 peak=10 work=10 "
                                     "input-packet-bytes=PRESERVED packet-reads=3 full-eight-source-FI=NOT_RUN native=NOT_RUN\n")
        covered = 0
        for site in range(index, 2, jobs):
            normal += PREFIX + ("compiler-cleanup had-owner=0 valid-before=0 sites-before=1 sites-after=1 "
                "runtime-sites-before=0 runtime-sites-after=0 count-before=0 count-owner-before-release=0 "
                "allocated-before=0 allocated-owner-before-release=0 live-before=0 live-owner-before-release=0 "
                "peak-before=0 peak-owner-before-release=0 work-before=0 work-owner-before-release=0 cumulative-uncharged=1\n")
            normal += release + PREFIX + (f"compiler-fault=PASS frozen-N=2 fault-site={site} hit-transitions=1 "
                "failure-stage=owner-new actual-status=3 expected-oom=3 output-preserved=1 diagnostic-match=1 "
                "cleanup-uncharged=1 compiler-sites=1 runtime-sites=0 allocated=0 peak=0 work=0 "
                "compiler-physical=0/0 runtime-physical=0/0 table=0 input-packet-bytes=NOT_REACHED packet-reads=0 "
                "full-eight-source-FI=NOT_RUN runtime-FI=NOT_RUN axes=NOT_RUN native=NOT_RUN\n")
            normal += PREFIX + (f"fault-ordinal kind=compiler ordinal={site} frozen-N=2 frozen-R=3 "
                f"shard={index} shards={jobs} compiler-physical=0/0 runtime-physical=0/0 table=0\n")
            covered += 1
        return normal + PREFIX + (f"fault-summary kind=compiler frozen-N=2 frozen-R=3 normal-records=43 "
            f"shard={index} shards={jobs} ordinal-start={index} ordinal-limit=2 ordinal-step={jobs} "
            f"covered={covered} scope={'FOCUSED' if jobs == 2 else 'SHARD'} "
            "compiler-physical=0/0 runtime-physical=0/0 table=0\n")

    good = sample(0, 1); census = (2, 3, 43)
    assert shard_contract(good, "compiler", 0, 1, census) == [0, 1]
    assert shard_contract(sample(0, 2), "compiler", 0, 2, census) == [0]
    assert shard_contract(sample(3, 4), "compiler", 3, 4, census) == []
    ordinal = next(line for line in good.splitlines(True) if "fault-ordinal" in line)
    summary = next(line for line in good.splitlines(True) if "fault-summary" in line)
    bad_outputs = [good + summary, good.replace(ordinal, "", 1), good.replace("covered=2", "covered=1"),
                   good.replace("frozen-R=3", "frozen-R=4"), good.replace("compiler-physical=0/0", "compiler-physical=1/0", 1),
                   good.replace("table=0", "table=1", 1), good.replace("compiler-fault=PASS", "compiler-fault=FAIL", 1),
                   good.replace("hit-transitions=1", "hit-transitions=2", 1), good.replace("actual-status=3", "actual-status=7", 1),
                   good.replace("preserved=1", "preserved=0", 1), good.replace("packet-reads=3", "packet-reads=2", 1),
                   good.replace("role=closed", "role=source", 1), good.replace("bytes=10", "bytes=65537", 1),
                   good.replace("input-packet-bytes=PRESERVED", "input-packet-bytes=UNSNAPSHOTTED_OPEN", 1),
                   good.replace(packets, packets + packets, 1), good + packets]
    runtime = sample(3, 4).split(PREFIX + "fault-summary ")[0]
    for site in (0, 2):
        runtime += packets
        runtime += PREFIX + (f"runtime-call record=0 stage=poll repeat=0 site-begin=0 site-end={site + 1} "
                             "status=6 expected-oom=6 hit=1\n")
        runtime += PREFIX + "release compiler=0/0 runtime=0/0 table=0 result=PASS\n"
        runtime += PREFIX + (f"runtime-fault=PASS frozen-R=3 fault-site={site} hit-windows=1 failure-stage=poll "
            "record=0 downstream=NOT_ENTERED status=6 expected-oom=6 output-preserved=1 borrowed-oom-shape=1 "
            "cleanup-uncharged=1 compiler-sites=2 runtime-sites=1 compiler-physical=0/0 runtime-physical=0/0 "
            "table=0 input-packet-bytes=PRESERVED packet-reads=3 full-eight-source-FI=NOT_RUN axes=NOT_RUN native=NOT_RUN\n")
        runtime += PREFIX + (f"fault-ordinal kind=runtime ordinal={site} frozen-N=2 frozen-R=3 "
            "shard=0 shards=2 compiler-physical=0/0 runtime-physical=0/0 table=0\n")
    runtime += PREFIX + ("fault-summary kind=runtime frozen-N=2 frozen-R=3 normal-records=43 shard=0 shards=2 "
        "ordinal-start=0 ordinal-limit=3 ordinal-step=2 covered=2 scope=SHARD compiler-physical=0/0 runtime-physical=0/0 table=0\n")
    assert shard_contract(runtime, "runtime", 0, 2, census) == [0, 2]
    runtime_bad = [runtime.replace("hit-windows=1", "hit-windows=2", 1),
                   runtime.replace("status=6 expected-oom=6", "status=7 expected-oom=6", 1),
                   runtime.replace("borrowed-oom-shape=1", "borrowed-oom-shape=0", 1),
                   runtime.replace("runtime-call record=0 stage=poll", "runtime-call record=999 stage=poll", 1),
                   runtime.replace("status=6 expected-oom=6 hit=1", "status=1 expected-oom=6 hit=1", 1)]
    diagnostic = ("source-allocation0 failure operation=original-checkAnswer owner-status=0 status=0 "
                  "source-stage=0 source-status=0 module=0 line=0 column=0 xir-status=0 function=0 "
                  "block=0 instruction=0 reason=0 call-status=6 message=\n").encode("utf-8")
    stderr_contract(b"", "normal", sample(3, 4))
    stderr_contract(b"", "compiler", good)
    stderr_contract(diagnostic * 2, "runtime", runtime)
    stderr_bad = [(b"runtime error: synthetic UBSan report\n", "normal", sample(3, 4)),
                  (b"runtime error: synthetic UBSan report\n", "compiler", good),
                  (diagnostic * 2 + b"runtime error: synthetic UBSan report\n", "runtime", runtime),
                  (diagnostic.replace(b"call-status=6", b"call-status=1", 1) + diagnostic, "runtime", runtime),
                  (diagnostic.replace(b"operation=original-checkAnswer", b"operation=canonical-entry", 1)
                   + diagnostic, "runtime", runtime)]
    for stderr, kind, output in stderr_bad:
        try:
            stderr_contract(stderr, kind, output)
        except (ValueError, KeyError):
            continue
        raise AssertionError("damaged stderr evidence was accepted")
    for output in runtime_bad:
        try:
            shard_contract(output, "runtime", 0, 2, census)
        except (ValueError, KeyError):
            continue
        raise AssertionError("damaged runtime evidence was accepted")
    for output in bad_outputs:
        try:
            shard_contract(output, "compiler", 0, 1, census)
        except (ValueError, KeyError):
            continue
        raise AssertionError("damaged fault evidence was accepted")
    print(json.dumps({"status": "PASS", "scope": "PARSER_SELF_TEST_ONLY", "accepted": 7, "rejected": len(bad_outputs) + len(runtime_bad) + len(stderr_bad),
                      "product_execution": "NOT_RUN", "full_FI": "NOT_RUN"}))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    for name in ("binary", "root", "file", "input-root", "source-file", "registration-file", "evidence"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--kind", choices=("compiler", "runtime"))
    parser.add_argument("--jobs", type=int, choices=range(1, 9), default=8)
    args = parser.parse_args()
    if args.self_test:
        self_test(); return
    for name in ("binary", "root", "file", "input_root", "source_file", "registration_file", "evidence", "kind"):
        if getattr(args, name) is None:
            parser.error("--" + name.replace("_", "-") + " is required")
    for name in ("binary", "root", "file", "input_root", "source_file", "registration_file"):
        setattr(args, name, getattr(args, name).resolve(strict=True))
    if (not args.root.is_dir() or not args.input_root.is_dir() or not args.file.is_relative_to(args.root)
            or any(not getattr(args, name).is_file() for name in ("binary", "file", "source_file", "registration_file"))):
        parser.error("actual fixture, executable, source and registration paths are invalid")
    helpers = load_helpers(args.input_root)
    run = args.evidence / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:8])
    run.mkdir(parents=True, exist_ok=False)
    started = time.monotonic(); deadline = started + 570
    issues, normal_results, results, covered, census = [], [], [], [], None
    capture_errors = []
    try:
        before = capture_inputs(args, helpers)
        helpers.write(run / "inputs-before.json", before)
    except Exception as error:
        finish_report({"status": "FAIL", "stage": "INPUT_CAPTURE", "issues": [str(error)],
                       "source_root": str(args.input_root), "product_execution": "NOT_RUN",
                       "git_capture_error": getattr(error, "evidence", None)}, run, helpers)
        raise SystemExit(1) from error
    base = [str(args.binary), str(args.root), str(args.file)]
    for index in range(2):
        label = f"normal-{index}"
        try:
            result = process_run(base, label, run, deadline, helpers, args.input_root)
            process_contract(result)
            output = Path(result["stdout"]).read_text(encoding="utf-8", errors="replace")
            stderr_contract(Path(result["stderr"]).read_bytes(), "normal", output)
            actual = normal_contract(output)
            result["normal_census"] = actual
            if census is None:
                census = actual
            elif actual != census:
                raise ValueError("two fresh original normals disagree on N/R/records")
            helpers.write(run / (label + ".json"), result)
            normal_results.append(result)
        except Exception as error:
            normal_results.append(process_failure(base, label, run, error, helpers))
            issues.append(f"normal {index}: {error}")
            break

    def shard(index: int) -> dict:
        command = base + ["--" + args.kind + "-shard", str(index), str(args.jobs)]
        label = f"shard-{index}"
        try:
            result = process_run(command, label, run, deadline, helpers, args.input_root)
            result["shard"] = index
            process_contract(result)
            output = Path(result["stdout"]).read_text(encoding="utf-8", errors="replace")
            ordinals = shard_contract(output, args.kind, index, args.jobs, census)
            stderr_contract(Path(result["stderr"]).read_bytes(), args.kind, output)
            result["ordinals"] = ordinals
            result["normal_census"] = normal_contract(output)
            helpers.write(run / (label + ".json"), result)
        except Exception as error:
            result = process_failure(command, label, run, error, helpers)
            result["shard"] = index
        return result

    if not issues and census is not None:
        try:
            with ThreadPoolExecutor(max_workers=args.jobs) as pool:
                futures = []
                for index in range(args.jobs):
                    try:
                        futures.append((index, pool.submit(shard, index)))
                    except Exception as error:
                        command = base + ["--" + args.kind + "-shard", str(index), str(args.jobs)]
                        failed = process_failure(command, f"shard-{index}", run, error, helpers)
                        failed["shard"] = index; results.append(failed)
                        issues.append(f"shard {index} submit: {error}")
                        break
                # Observe every submitted future, retaining partial results on any failure.
                for index, future in futures:
                    try:
                        results.append(future.result())
                    except Exception as error:
                        command = base + ["--" + args.kind + "-shard", str(index), str(args.jobs)]
                        failed = process_failure(command, f"shard-{index}", run, error, helpers)
                        failed["shard"] = index; results.append(failed)
        except Exception as error:
            issues.append("shard executor failed: " + str(error))
        results.sort(key=lambda value: value["shard"])
        for result in results:
            if result.get("parse_issue"):
                issues.append(f"shard {result['shard']}: {result['parse_issue']}")
            covered += result.get("ordinals", [])
        total = census[0] if args.kind == "compiler" else census[1]
        if len(results) != args.jobs or sum("normal_census" in r and not r.get("parse_issue") for r in results) != args.jobs:
            issues.append("all original shard normals were not verified")
        if len(covered) != total or any(site != index for index, site in enumerate(sorted(covered))):
            issues.append("complete original ordinal domain was not covered exactly once")
    else:
        total = None
    try:
        after = capture_inputs(args, helpers); helpers.write(run / "inputs-after.json", after)
        changed = sorted(name for name in before["files"].keys() | after["files"].keys()
                         if before["files"].get(name) != after["files"].get(name))
        if changed or {k: v for k, v in before.items() if k != "files"} != {k: v for k, v in after.items() if k != "files"}:
            issues.append("actual captured inputs or capture origin changed: " + repr(changed))
    except Exception as error:
        changed = ["INPUT_CAPTURE_FAILED"]; issues.append("after input capture failed: " + str(error))
        capture_errors.append(getattr(error, "evidence", {"error": str(error)}))
    report = {"status": "FAIL" if issues else "PASS", "scope": "COMPLETE_SOURCE0_ORDINAL_COVERAGE", "kind": args.kind,
              "jobs": args.jobs, "normal_processes_planned": 2,
              "normal_processes_entered": sum(r.get("entered") is True for r in normal_results),
              "normal_census": census,
              "shard_normal_censuses_verified": sum("normal_census" in r and not r.get("parse_issue") for r in results),
              "normal_runs_verified": sum("normal_census" in r and not r.get("parse_issue") for r in normal_results + results),
              "sites": total, "covered": len(covered), "issues": issues, "changed": changed,
              "inputs": len(before["files"]), "capture_source": before["capture_source"], "source_root": str(args.input_root),
              "process_budget_seconds": 570, "elapsed_seconds": time.monotonic() - started,
              "input_capture_errors": capture_errors, "parent_pid": os.getpid(),
              "binary": str(args.binary), "binary_sha256": before["files"][str(args.binary)], "evidence": str(run),
              "finished_at": datetime.now(timezone.utc).isoformat(),
              "qualification_boundary": "Only this actual Source0 executable/input scope; full-eight-source-FI, axes/native and Main qualification remain OPEN."}
    finish_report(report, run, helpers)


if __name__ == "__main__":
    main()
