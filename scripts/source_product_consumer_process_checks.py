#!/usr/bin/env python3
"""Exercise allocation runner process handling with real Windows child processes."""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import importlib.util
import json
import os
from pathlib import Path
import sys
import time
import uuid

sys.dont_write_bytecode = True
OUT = b"runner-child stdout\x00\xff\n"
ERR = b"runner-child stderr\x00\xfe\n"


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise ValueError(reason)


def load(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    require(spec is not None and spec.loader is not None, "runner module is unavailable")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def terminated(pid: int) -> str:
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.OpenProcess(0x00100000, False, pid)
    if not handle:
        error = ctypes.get_last_error()
        require(error == 87, f"cannot independently inspect child {pid}: Windows error {error}")
        return "OpenProcess: ERROR_INVALID_PARAMETER (PID no longer present)"
    try:
        result = kernel.WaitForSingleObject(handle, 0)
        require(result == 0, f"child {pid} is not terminated: wait result {result}")
        return "WaitForSingleObject: WAIT_OBJECT_0"
    finally:
        require(bool(kernel.CloseHandle(handle)), "failed to close observation handle")


def check(module, helpers, root: Path, run: Path, case: str) -> dict:
    child_case = case if case in ("normal", "nonzero", "timeout") else "normal"
    command = [sys.executable, "-B", str(Path(__file__).resolve()), "--child", child_case]
    if case == "missing":
        command = [str(run / "absent-child.exe")]
    deadline = time.monotonic() + (2 if case == "timeout" else 10)
    if case == "expired":
        deadline = time.monotonic() - 1
    receipt = module.process_run(command, case, run, deadline, helpers, root)
    accepted = True
    try:
        module.process_contract(receipt)
    except ValueError:
        accepted = False
    require(accepted == (case == "normal"), "process contract accepted the wrong outcome")
    out, err = Path(receipt["stdout"]).read_bytes(), Path(receipt["stderr"]).read_bytes()
    require(helpers.digest(Path(receipt["stdout"])) == receipt["stdout_sha256"], "stdout hash mismatch")
    require(helpers.digest(Path(receipt["stderr"])) == receipt["stderr_sha256"], "stderr hash mismatch")
    require(json.loads((run / (case + ".json")).read_bytes()) == receipt, "saved receipt differs")
    if case in ("normal", "nonzero", "timeout"):
        require(receipt["entered"] and receipt["pid"] > 0 and receipt["termination_observed"],
                "actual PID or observed termination missing")
        require(out == str(receipt["pid"]).encode() + b"\n" + OUT and err == ERR,
                "binary output or child-reported PID was not preserved")
        require(not receipt["launch_error"] and not receipt["termination_errors"], "launch or reap failed")
        observation = terminated(receipt["pid"])
        if case == "timeout":
            require(receipt["timed_out"] and receipt["execution_error"] and receipt["returncode"] != 0,
                    "real timeout was not killed, reaped and rejected")
        else:
            require(not receipt["timed_out"] and not receipt["execution_error"], "unexpected execution failure")
            require(receipt["returncode"] == (0 if case == "normal" else 17), "exit status changed")
    else:
        require(not receipt["entered"] and receipt["pid"] is None and receipt["returncode"] is None,
                "unlaunched child received a fabricated process identity")
        require(not receipt["termination_observed"] and not out and not err, "unlaunched process produced output")
        require(receipt["timed_out"] == (case == "expired"), "deadline status changed")
        require(bool(receipt["launch_error"]) == (case == "missing"), "launch failure status changed")
        observation = "NOT_LAUNCHED"
    return {"case": case, "receipt": receipt, "contract_accepted": accepted, "OS_observation": observation}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--evidence", type=Path)
    parser.add_argument("--child", choices=("normal", "nonzero", "timeout"))
    args = parser.parse_args()
    if args.child:
        os.write(1, str(os.getpid()).encode() + b"\n" + OUT)
        os.write(2, ERR)
        if args.child == "timeout":
            time.sleep(30)
        raise SystemExit(17 if args.child == "nonzero" else 0)
    require(os.name == "nt", "this check requires actual Windows process semantics")
    require(args.evidence is not None, "--evidence is required")
    root = args.input_root.resolve(strict=True)
    run = args.evidence.resolve() / uuid.uuid4().hex
    run.mkdir(parents=True, exist_ok=False)
    helpers = load(root / "scripts/source_product_consumer_faults.py", "process_check_helpers")
    paths = [root / ("scripts/source_product_consumer_allocation_" + name + ".py")
             for name in ("faults", "axes")]
    paths += [Path(__file__).resolve(), Path(helpers.__file__).resolve(), Path(sys.executable)]
    before = {str(path): helpers.digest(path) for path in paths}
    helpers.write(run / "inputs-before.json", before)
    rows, issues = [], []
    for name, path in zip(("faults", "axes"), paths[:2]):
        module = load(path, "process_check_" + name)
        folder = run / name
        folder.mkdir()
        for case in ("normal", "nonzero", "timeout", "missing", "expired"):
            try:
                rows.append({"runner": name, **check(module, helpers, root, folder, case)})
            except Exception as error:
                issues.append({"runner": name, "case": case, "error": repr(error)})
                break
        if issues:
            break
    after = {str(path): helpers.digest(path) for path in paths}
    helpers.write(run / "inputs-after.json", after)
    if before != after:
        issues.append({"error": "runner, helper, interpreter or test input changed"})
    report = {"status": "FAIL" if issues else "PASS", "scope": "WINDOWS_RUNNER_PROCESS_CHECK_ONLY",
              "cases": rows, "issues": issues, "evidence": str(run),
              "product_execution": "NOT_RUN", "full_FI": "NOT_RUN", "resource_axes": "NOT_RUN",
              "reap_error_path": "NOT_RUN", "qualification": "Python child processes only; no product qualification."}
    helpers.write(run / "result.json", report)
    print(json.dumps({k: v for k, v in report.items() if k != "cases"}))
    if issues:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
