"""Exercise report rejection and real parent-death ownership with the native allocation target."""
from __future__ import annotations

import time
STARTED = time.monotonic()

import argparse
import ctypes
from ctypes import wintypes
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

sys.dont_write_bytecode = True
driver_path = Path(__file__).with_name("run_allocation_case_faults.py")
spec = importlib.util.spec_from_file_location("allocation_case_driver", driver_path)
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--reports-root", type=Path)
    args = parser.parse_args()
    if os.name != "nt":
        print("Native Windows parent-death control is NOT_RUN on this host; parent POSIX lifeline is separate.")
        return 77
    job = driver.establish_job()
    deadline = STARTED + 120
    source, build, exe = (path.resolve(strict=True) for path in
                          (args.source_root, args.build_root, args.executable))
    paths, _, cache = driver.input_paths(source, build, exe, deadline)
    before = driver.fingerprint(paths, deadline)
    configuration = driver.fingerprint(driver.config_paths(build), deadline)
    identity = {"nonce": uuid.uuid4().hex, "input_sha256": driver.mapping_sha(before),
                "binary_sha256": driver.sha(exe, deadline), "config_sha256": driver.mapping_sha(configuration),
                "asan": driver.cmake_flag(cache.get("ENABLE_ASAN")), "ubsan": driver.cmake_flag(cache.get("ENABLE_UBSAN"))}
    environment = os.environ.copy()
    for key in ("input", "binary", "config"):
        environment[f"XRAY_ALLOCATION_{key.upper()}_SHA256"] = identity[f"{key}_sha256"]

    def native(arguments):
        result = subprocess.run([str(exe), *arguments, identity["nonce"]], cwd=build,
                                env=environment, stdin=subprocess.DEVNULL, capture_output=True,
                                timeout=driver.remaining(deadline), close_fds=True,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        driver.checked(result.returncode == 0 and not result.stderr, "Actual native control fixture failed")
        return result.stdout

    census_raw = native(["--allocation-census"])
    census, _, _ = driver.parse_report(census_raw, identity, "census", 0, 1)
    sample = native(["--allocation-shard", "0", "67", "normal"])
    driver.parse_report(sample, identity, "normal", 0, 67, census)
    rejected = []

    def reject(name, raw, **changes):
        arguments = {"identity": identity, "mode": "normal", "index": 0, "workers": 67, "census": census}
        arguments.update(changes)
        try:
            driver.parse_report(raw, **arguments)
        except (RuntimeError, ValueError, UnicodeError):
            rejected.append(name)
        else:
            raise RuntimeError("Invalid actual report accepted: " + name)

    lines = sample.decode().splitlines()
    def altered(prefix, key, value):
        changed = list(lines)
        for index, line in enumerate(changed):
            if line.startswith(prefix):
                row = json.loads(line[len(prefix):]);row[key] = value
                changed[index] = prefix + json.dumps(row)
                return ("\n".join(changed) + "\n").encode()
        raise RuntimeError("Missing actual mutation target")

    for key, value in [("nonce", "f" * 32), ("input_sha256", "f" * 64),
                       ("binary_sha256", "f" * 64), ("config_sha256", "f" * 64),
                       ("index", 1), ("workers", 66), ("mode", "producer"),
                       ("asan", bool(identity["asan"])), ("schema", "foreign")]:
        reject("identity-" + key, altered("ALLOCATION_BEGIN ", key, value))
    for key, value in [("cases", 0), ("whole", 0), ("compiler", 1), ("runtime", 1),
                       ("bridge", 1), ("axes", 11), ("axis_groups", 1),
                       ("remaining_resources", 1), ("physical_blocks", 1), ("physical_bytes", 1),
                       ("physical_blocks", False)]:
        reject("completion-" + key + "-" + type(value).__name__, altered("ALLOCATION_END ", key, value))
    case_line = next(line for line in lines if line.startswith("FI map "))
    reject("duplicate-case", sample.replace(case_line.encode(), (case_line + "\n" + case_line).encode()))
    reject("missing-case", ("\n".join(line for line in lines if line != case_line) + "\n").encode())
    reject("wrong-case", sample.replace(b"Function scalar0 mode0", b"Function scalar0 mode1"))
    reject("bad-partition", sample.replace(b"compiler=[0,", b"compiler=[1,"))
    reject("duplicate-key", sample.replace(b'"cases":', b'"cases":1,"cases":'))
    reject("truncated", "\n".join(lines[:-1]).encode())
    reject("extra-envelope", sample.replace(b"ALLOCATION_END ", b"ALLOCATION_BEGIN {}\nALLOCATION_END "))
    reject("wrong-consumer", sample, workers=66)
    reject("zero-workers", sample, workers=0)
    reject("oversized", b"x" * (2 * 1024 * 1024 + 1))
    match = driver.CASE.fullmatch(case_line)
    family = match[1]
    scalar, case_mode, whole, prefix, _, _, runtime = map(int, match.groups()[1:])
    forged_line = (f"FI map {family} scalar{scalar} mode{case_mode}: whole={whole+1} "
                   f"compiler=[0,{prefix}) runtime=[{prefix},{whole+1}) local=[0,{runtime+1})")
    forged = altered("ALLOCATION_END ", "whole", whole + 1).replace(case_line.encode(), forged_line.encode())
    reject("self-consistent-foreign-census", forged)
    try:
        driver.remaining(time.monotonic() - 1)
    except RuntimeError as error:
        driver.checked("300-second deadline" in str(error), "Wrong deadline rejection")
        rejected.append("expired-global-deadline")
    else:
        raise RuntimeError("Expired original global deadline accepted")

    reports_root = args.reports_root.resolve() if args.reports_root is not None else None
    base_command = [sys.executable, "-B", str(driver_path), "--executable", str(exe),
                    "--source-root", str(source), "--build-root", str(build)]
    for root in (source, build):
        result = subprocess.run([*base_command, "--mode", "normal", "--reports-root", str(root)],
                                capture_output=True, timeout=driver.remaining(deadline),
                                creationflags=subprocess.CREATE_NO_WINDOW)
        driver.checked(result.returncode == 1 and b"Reports must be outside Source and Build" in result.stderr,
                       "Source/Build report ownership rejection failed")
        rejected.append("reports-inside-" + ("source" if root == source else "build"))

    if reports_root is None:
        import tempfile
        reports_root = Path(tempfile.gettempdir()) / "xray-allocation-case-driver-control"
    reports_root = reports_root.resolve() / ("parent-death-" + uuid.uuid4().hex)
    driver.checked(not reports_root.is_relative_to(source) and not reports_root.is_relative_to(build),
                   "Control outputs must be outside Source and Build")
    reports_root.mkdir(parents=True, exist_ok=False)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.CloseHandle.argtypes = [wintypes.HANDLE];kernel.CloseHandle.restype = wintypes.BOOL
    handles = []
    subject = None
    try:
        with (reports_root / "parent.stdout.log").open("xb") as stdout, (reports_root / "parent.stderr.log").open("xb") as stderr:
            subject = subprocess.Popen([*base_command, "--mode", "producer", "--reports-root", str(reports_root)],
                                       stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, close_fds=True,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
        receipt = None
        while receipt is None:
            driver.remaining(deadline)
            driver.checked(subject.poll() is None, "Owned parent exited before its real child-death probe")
            candidates = list(reports_root.glob("*/qualification.json"))
            if candidates:
                driver.checked(len(candidates) == 1, "Ambiguous control receipt")
                try:
                    observed = json.loads(candidates[0].read_text(encoding="utf-8"))
                except (json.JSONDecodeError, FileNotFoundError):
                    observed = None
                if observed and observed.get("stage") == "partitions_running":
                    receipt = observed
            if receipt is None:
                time.sleep(.005)
        rows = [row for row in receipt["commands"] if row["name"].startswith("shard-")]
        driver.checked(len(rows) == driver.WORKERS and not receipt["qualified"], "Incorrect native control population")
        for row in rows:
            handle = kernel.OpenProcess(0x00100000, False, row["pid"])
            driver.checked(handle, "Cannot hold actual native child process")
            handles.append(handle)
            driver.checked(kernel.WaitForSingleObject(handle, 0) == 258, "Native child was not alive before parent death")
        subject.kill()
        parent_status = subject.wait(timeout=driver.remaining(deadline))
        driver.checked(parent_status != 0, "Hard-killed parent unexpectedly succeeded")
        for handle in handles:
            driver.checked(kernel.WaitForSingleObject(handle, min(5000, int(driver.remaining(deadline) * 1000))) == 0,
                           "Native child survived the death of its sole Job owner")
        control = {"parent_pid": subject.pid, "parent_exit_code": parent_status,
                   "held_native_children": [row["pid"] for row in rows], "live_before_parent_kill": len(rows),
                   "all_held_process_handles_signaled": True, "direct_child_kill_used": False,
                   "subject_job_handle_held_by_observer": False, "reports_directory": str(reports_root)}
    finally:
        if subject is not None and subject.poll() is None:
            subject.kill();subject.wait(timeout=5)
        for handle in handles:
            driver.checked(kernel.CloseHandle(handle), "Cannot release native observation handle")
    driver.checked(driver.fingerprint(paths, deadline) == before and
                   driver.fingerprint(driver.config_paths(build), deadline) == configuration,
                   "Actual inputs changed during driver controls")
    driver.checked(driver.job_processes(job) == 1, "Native control descendants did not drain")
    result = {"result": "PASS", "report_rejections": rejected, "real_report_mutations": True,
              "parent_death": control, "source_and_configuration_unchanged": True,
              "native_drain_active_processes": 1, "seconds": time.monotonic() - STARTED,
              "full_fault_qualification": False}
    (reports_root / "control-result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    driver.remaining(deadline)
    print(json.dumps(result), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
