#!/usr/bin/env python3
"""Measure and verify one Source0 compiler resource boundary with identical argv."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import time
import uuid

PREFIX = "source-allocation0 "
LIMITS = {"allocated": 67108864, "live": 8388608, "work": 128000000}
KINDS = {"allocated": 0, "live": 1, "work": 2}
FIELDS = {"allocated": "allocated", "live": "peak", "work": "work"}
DOMAIN_BUDGET = {0: 2, 1: 6, 2: 2, 3: 6}
STAGE_DOMAINS = {"owner-new": 0, "original-input": 1, "session-new": 2,
                 "instance-new": 4, "canonical-start": 5, "original-test-start": 5,
                 "poll": 6, "take-result": 7, "value-drop": 8,
                 "instance-config": 8, "private-entry-authority": 8}
STAGE_DOMAINS.update({name: 3 for name in (
    "source-product", "source-Checked-reader", "closed-Checked-reader", "closed-Checked-retained",
    "detached-source-verify", "detached-closed-verify", "retained-Checked-reread",
    "retained-Checked-verify", "Lowered", "Lowered-verify", "VM-Program-take")})
PAIRS = (("count-before", "count-after"), ("allocated-before", "allocated-after"),
         ("peak-before", "peak-after"), ("work-before", "work-after"),
         ("compiler-site-begin", "compiler-site-end"))


def load_fault_helpers(input_root: Path):
    path = input_root / "scripts/source_product_consumer_allocation_faults.py"
    spec = importlib.util.spec_from_file_location("source0_allocation_fault_helpers", path)
    if spec is None or spec.loader is None:
        raise ValueError("actual Source0 fault helper is unavailable")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def normal_values(output: str, helpers) -> dict:
    n, r, records = helpers.normal_contract(output)
    row = helpers.rows(output, "normal=")[0]
    values = {axis: helpers.unsigned(row[field]) for axis, field in FIELDS.items()}
    if any(not value or value > LIMITS[axis] for axis, value in values.items()):
        raise ValueError("fresh normal resource measurements exceed the original finite limits")
    return {"compiler_sites": n, "runtime_sites": r, "records": records, "totals": values}


def signed(value: str) -> int:
    if not re.fullmatch(r"-?[0-9]+", value):
        raise ValueError("invalid signed domain status")
    return int(value)


def axis_contract(output: str, axis: str, cut: str, normal_output: str, helpers) -> dict:
    normal = normal_values(normal_output, helpers)
    totals = normal["totals"]; total = totals[axis]; limit = total - (cut == "minus1")
    caps = LIMITS | {axis: limit}
    rows = helpers.rows(output, "compiler-axis=")
    if len(rows) != 1:
        raise ValueError("axis result is not unique")
    row = rows[0]; helpers.zero_physical(row)
    expected_fields = {"compiler-axis": "PASS", "axis": str(KINDS[axis]), "cut": cut,
                       "normal-total": str(total), "limit": str(limit), "cleanup-uncharged": "1",
                       "input-packet-bytes": "UNSNAPSHOTTED_OPEN", "full-eight-source-FI": "NOT_RUN",
                       "native": "NOT_RUN"}
    if any(row.get(k) != v for k, v in expected_fields.items()):
        raise ValueError("axis result is not bound to its fresh normal, cut and OPEN boundary")
    actual = helpers.unsigned(row["actual"])
    if actual != helpers.unsigned(row[FIELDS[axis]]) or actual > limit:
        raise ValueError("axis actual does not match the selected finite ledger axis")
    if len(helpers.releases(output)) != 1:
        raise ValueError("axis owner was not released exactly once with five physical zeros")
    calls = helpers.rows(output, "compiler-axis-call ")
    reference = helpers.rows(normal_output, "runtime-call ")
    count = helpers.unsigned(row["records"])
    if not calls or len(calls) != count or count > normal["records"]:
        raise ValueError("axis public-call prefix length differs from its result")
    rejected = None; previous = None
    for i, call in enumerate(calls):
        if helpers.unsigned(call["record"]) != i or call["stage"] != reference[i]["stage"]:
            raise ValueError("axis records are not the continuous original public-call prefix")
        domain = helpers.unsigned(call["domain"])
        if domain != STAGE_DOMAINS.get(call["stage"]) or signed(call["expected-budget"]) != DOMAIN_BUDGET.get(domain, -1):
            raise ValueError("axis expected budget does not belong to the exact public API domain")
        for before, after in PAIRS:
            if helpers.unsigned(call[after]) < helpers.unsigned(call[before]):
                raise ValueError("axis cumulative charge or compiler ordinal was refunded")
            if previous is not None and helpers.unsigned(call[before]) != helpers.unsigned(previous[after]):
                raise ValueError("axis charged outside the original public-call windows")
        for suffix in ("before", "after"):
            if (helpers.unsigned(call["allocated-" + suffix]) > caps["allocated"]
                    or helpers.unsigned(call["live-" + suffix]) > helpers.unsigned(call["peak-" + suffix])
                    or helpers.unsigned(call["peak-" + suffix]) > caps["live"]
                    or helpers.unsigned(call["work-" + suffix]) > caps["work"]):
                raise ValueError("axis crossed an unchanged finite resource limit")
        status = helpers.unsigned(call["status"])
        budget = DOMAIN_BUDGET.get(domain)
        if budget is not None and status == budget:
            if cut != "minus1" or rejected is not None or i != count - 1 or call.get("output-preserved") != "1":
                raise ValueError("axis did not stop at its unique first budget refusal")
            rejected = call
        elif status != helpers.unsigned(reference[i]["status"]):
            raise ValueError("axis returned OOM, LIMIT or a different original public status")
        if domain >= 4 and (any(call[a] != call[b] for a, b in PAIRS)
                or call["live-before"] != call["live-after"] or call["valid-before"] != call["valid-after"]):
            raise ValueError("runtime Call changed the compiler ledger")
        previous = call
    for kind, field in FIELDS.items():
        value = helpers.unsigned(row[field])
        if value != helpers.unsigned(previous[field + "-after"]) or value > caps[kind]:
            raise ValueError("axis final cumulative ledger was refunded or exceeded its original limit")
    if (row["compiler-sites"] != previous["compiler-site-end"]
            or row["runtime-sites"] != previous["runtime-site-end"]):
        raise ValueError("axis cleanup changed actual allocation ordinals")
    diagnostics = helpers.rows(output, "compiler-axis-diagnostic ")
    rollbacks = helpers.rows(output, "compiler-axis-rollback ")
    downstream = helpers.rows(output, "compiler-axis-downstream=")
    runtime = helpers.rows(output, "runtime-call ")
    expected_runtime = count if cut == "exact" else count - 1
    if (len(runtime) != expected_runtime or any(helpers.unsigned(call["record"]) != i
            or call["stage"] != reference[i]["stage"] or call["status"] != reference[i]["status"]
            or call.get("hit") != "0" for i, call in enumerate(runtime))):
        raise ValueError("axis runtime prefix changed the original unfaulted calls")
    if cut == "exact":
        if (rejected is not None or diagnostics or rollbacks or downstream or count != normal["records"]
                or row.get("failure-applicable") != "0" or row.get("first-rejection") != "NOT_APPLICABLE"
                or actual != total or helpers.unsigned(row["compiler-sites"]) != normal["compiler_sites"]
                or helpers.unsigned(row["runtime-sites"]) != normal["runtime_sites"]
                or any(helpers.unsigned(row[field]) != totals[kind] for kind, field in FIELDS.items())):
            raise ValueError("exact axis did not complete the unchanged full normal program")
    else:
        if rejected is None or len(diagnostics) != 1 or len(rollbacks) != 1 or len(downstream) != 1:
            raise ValueError("minus1 lacks the unique real budget diagnostic, rollback or stop")
        if (any(row.get(k) != "1" for k in ("failure-applicable", "output-preserved", "rollback", "diagnostic-valid"))
                or row.get("first-rejection") != rejected["stage"] or row.get("record") != rejected["record"]
                or row.get("status") != rejected["status"] or downstream[0].get("compiler-axis-downstream") != "NOT_ENTERED"):
            raise ValueError("minus1 summary is not bound to its real first refusal")
        diagnostic, rollback = diagnostics[0], rollbacks[0]
        if diagnostic.get("stage") != rejected["stage"] or diagnostic.get("valid") != "1":
            raise ValueError("minus1 diagnostic belongs to a different API or was invalid")
        applicable = rejected["domain"] == "3" and rejected["stage"] != "VM-Program-take"
        if diagnostic.get("applicable") != str(int(applicable)):
            raise ValueError("minus1 diagnostic applicability differs from the called API")
        if applicable and rejected["stage"] == "source-product":
            stage = helpers.unsigned(diagnostic["product-stage"])
            if diagnostic.get("product-status") != "6" or not 1 <= stage <= 6:
                raise ValueError("SourceProduct budget diagnostic has a wrong domain or stage")
            if stage == 1 and diagnostic.get("source-status") != "6":
                raise ValueError("Source check budget diagnostic has a wrong nested status")
            if stage not in (1, 6) and (diagnostic.get("xir-status") != "6" or diagnostic.get("reason") != "0"):
                raise ValueError("Source projection budget diagnostic has a wrong nested status")
        elif applicable and (diagnostic.get("status") != "6" or diagnostic.get("reason") != "0"):
            raise ValueError("XIR budget diagnostic has a wrong status or reason")
        if (rollback.get("stage") != rejected["stage"] or rollback.get("diagnostic-free-uncharged") != "1"
                or rollback.get("stock-preserved") != "1" or rollback.get("compiler-before") != rollback.get("compiler-after")
                or rollback.get("runtime-before") != rollback.get("runtime-after")
                or rollback.get("table-before") != rollback.get("table-after")):
            raise ValueError("minus1 diagnostic free charged resources or failed physical rollback")
    return {"normal": normal, "axis": axis, "cut": cut, "total": total, "limit": limit,
            "actual": actual, "records": count, "first_refusal": None if rejected is None else rejected["stage"]}


def axis_stderr_contract(stderr: bytes, output: str, cut: str, helpers) -> None:
    if cut == "exact":
        if stderr:
            raise ValueError("unexpected exact-axis stderr; raw diagnostics retained")
        return
    rows = helpers.rows(output, "compiler-axis=")
    calls = helpers.rows(output, "compiler-axis-call ")
    if len(rows) != 1 or not calls:
        raise ValueError("minus1 stderr has no bound original refusal")
    lines = stderr.decode("utf-8").splitlines()
    if len(lines) != 1:
        raise ValueError("minus1 stderr is not one original budget diagnostic")
    match = helpers.RUNTIME_DIAGNOSTIC.fullmatch(lines[0])
    if match is None or "Sanitizer" in lines[0] or "runtime error:" in lines[0]:
        raise ValueError("minus1 stderr contains unknown or sanitizer information")
    call = calls[-1]; domain = helpers.unsigned(call["domain"])
    operation = helpers.RUNTIME_OPERATION.get(call["stage"])
    if operation is None or match["operation"] != operation:
        raise ValueError("minus1 diagnostic operation differs from its original API")
    if domain == 0 and match["owner"] != "2":
        raise ValueError("minus1 diagnostic does not report owner budget")
    if domain == 3 and match["status"] != "6":
        raise ValueError("minus1 diagnostic does not report XIR budget")
    if domain not in DOMAIN_BUDGET or rows[0].get("status") != str(DOMAIN_BUDGET[domain]):
        raise ValueError("minus1 used a runtime failure as a compiler budget refusal")


def capture(args, faults, original) -> dict:
    result = faults.capture_inputs(args, original)
    result["files"][str(Path(__file__).resolve())] = original.digest(Path(__file__).resolve())
    result["argv_namespace"] = [str(args.binary), str(args.root), str(args.file)]
    result["axis"] = args.axis; result["cut"] = args.cut
    return result


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
              "stdout": str(stdout), "stderr": str(stderr), "reap_deadline_seconds": 118}
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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "root", "file", "input-root", "source-file", "registration-file", "evidence"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--axis", choices=LIMITS, required=True)
    parser.add_argument("--cut", choices=("exact", "minus1"), required=True)
    args = parser.parse_args()
    for name in ("binary", "root", "file", "input_root", "source_file", "registration_file"):
        setattr(args, name, getattr(args, name).resolve(strict=True))
    if not args.root.is_dir() or not args.input_root.is_dir() or not args.file.is_relative_to(args.root):
        parser.error("actual fixture or producer directory is invalid")
    if any(not getattr(args, name).is_file() for name in ("binary", "file", "source_file", "registration_file")):
        parser.error("actual executable, fixture, source and registration are required files")
    faults = load_fault_helpers(args.input_root); original = faults.load_helpers(args.input_root)
    run = args.evidence / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:8])
    run.mkdir(parents=True, exist_ok=False)
    start = time.monotonic(); deadline = start + 110
    issues, normals, result, normal_output, values = [], [], None, None, None
    capture_errors = []
    try:
        before = capture(args, faults, original); original.write(run / "inputs-before.json", before)
    except Exception as error:
        faults.finish_report({"status": "FAIL", "stage": "INPUT_CAPTURE", "issues": [str(error)],
                              "product_execution": "NOT_RUN", "git_capture_error": getattr(error, "evidence", None)}, run, original)
        raise SystemExit(1) from error
    base = before["argv_namespace"]
    for index in range(2):
        label = "normal-" + str(index)
        try:
            process = process_run(base, label, run, deadline, original, args.input_root)
            process_contract(process)
            output = Path(process["stdout"]).read_text(encoding="utf-8")
            faults.stderr_contract(Path(process["stderr"]).read_bytes(), "normal", output)
            actual = normal_values(output, faults)
            if values is not None and actual != values:
                raise ValueError("two same-argv fresh normals disagree on compiler totals or denominators")
            normal_output, values = output, actual; process["measurements"] = actual
            original.write(run / (label + ".json"), process)
            normals.append(process)
        except Exception as error:
            normals.append(faults.process_failure(base, label, run, error, original))
            issues.append("normal " + str(index) + ": " + str(error))
            break
    if not issues and values is not None:
        command = base + ["--compiler-axis", args.axis, args.cut, str(values["totals"][args.axis])]
        try:
            process = process_run(command, "axis", run, deadline, original, args.input_root)
            process_contract(process)
            output = Path(process["stdout"]).read_text(encoding="utf-8")
            result = axis_contract(output, args.axis, args.cut, normal_output, faults)
            axis_stderr_contract(Path(process["stderr"]).read_bytes(), output, args.cut, faults)
            process["axis_result"] = result
            original.write(run / "axis.json", process)
        except Exception as error:
            faults.process_failure(command, "axis", run, error, original)
            issues.append(str(error))
    try:
        after = capture(args, faults, original); original.write(run / "inputs-after.json", after)
        if before != after:
            issues.append("actual captured inputs, argv namespace or producer changed")
    except Exception as error:
        issues.append("after input capture failed: " + str(error))
        capture_errors.append(getattr(error, "evidence", {"error": str(error)}))
    report = {"status": "FAIL" if issues else "PASS", "scope": "SOURCE0_ONE_COMPILER_RESOURCE_BOUNDARY",
              "axis": args.axis, "cut": args.cut, "argv_namespace": base, "issues": issues,
              "measurements": values, "axis_result": result, "evidence": str(run),
              "input_capture_errors": capture_errors,
              "normal_processes_planned": 2, "normal_processes_verified": sum("measurements" in r for r in normals),
              "process_deadline_seconds": 110, "elapsed_seconds": time.monotonic() - start,
              "qualification_boundary": "Only this measured Source0 axis/cut; complete FI, other sources, native, full safety and Main qualification remain OPEN."}
    faults.finish_report(report, run, original)


if __name__ == "__main__":
    main()
