"""Replay every root-parameter compiler fault with two owned processes in 600 seconds."""
from __future__ import annotations

import time
STARTED = time.monotonic()

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "lib"))
from xraytest.windows_owned_job import establish_job

WORKERS, TIMEOUT, MEMORY_LIMIT = 2, 600, 8388608
FIXTURES = ("apply.xr", "capture-pure.xr")
INPUTS = ("apply.xr", "fixed.xr", "root.xr", "forward.xr",
          "capture-pure.xr", "capture-root.xr", "capture-fixed.xr")


def checked(condition, message):
    if not condition:
        raise RuntimeError(message)


def remaining(deadline):
    seconds = deadline - time.monotonic()
    checked(seconds > 0, "Original overall 600-second deadline exhausted")
    return seconds


def sha(path, deadline):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(65536):
            remaining(deadline)
            digest.update(chunk)
    remaining(deadline)
    return digest.hexdigest()


def input_snapshot(fixtures, deadline):
    result = {}
    for name in INPUTS:
        path = fixtures / name
        if path.exists():
            stat = path.stat()
            result[name] = {"sha256": sha(path, deadline), "bytes": stat.st_size, "mtime_ns": stat.st_mtime_ns}
        else:
            result[name] = None
    return result


def exact(row, expected):
    checked(row == expected and all(type(row[key]) is type(value) for key, value in expected.items()),
            "Unexpected, incomplete or untyped report row")


def rows(path, deadline):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            checked(key not in result, "Duplicate report field")
            result[key] = value
        return result
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            remaining(deadline)
            checked(line.endswith("\n"), "Truncated report record")
            row = json.loads(line, object_pairs_hook=unique)
            checked(type(row) is dict, "Report record is not an object")
            yield row


def census(row, fixture):
    keys = {"kind", "fixture", "file", "sites", "allocated", "peak", "allocations",
            "baseline", "held", "blocks", "bytes"}
    checked(set(row) == keys and row["kind"] == "census" and row["file"] == FIXTURES[fixture],
            "Wrong original fixture census")
    checked(all(type(row[key]) is int and row[key] >= 0 for key in keys - {"kind", "file"}),
            "Invalid census statistics")
    checked(row["fixture"] == fixture and row["sites"] > 0 and 0 < row["peak"] <= MEMORY_LIMIT and
            row["held"] == row["baseline"] and row["blocks"] == row["bytes"] == 0,
            "Invalid finite census or physical cleanup")
    return row


def verify(path, mode, index, nonce, deadline, baseline=None, union=None):
    records = iter(rows(path, deadline))
    exact(next(records), {"kind": "identity", "mode": mode, "index": index, "workers": WORKERS,
                          "nonce": nonce, "memory_limit": MEMORY_LIMIT})
    actual, total = [], 0
    for fixture in range(len(FIXTURES)):
        measured = census(next(records), fixture)
        if baseline is not None:
            checked(measured == baseline[fixture], "Worker census, input or finite owner differs")
        actual.append(measured)
        sites, points = measured["sites"], 0
        if mode == "shard":
            for ordinal in range(index, sites, WORKERS):
                point = next(records)
                keys = {"kind", "fixture", "ordinal", "attempts", "status", "injected",
                        "baseline", "held", "blocks", "bytes"}
                checked(set(point) == keys and all(type(point[key]) is int and point[key] >= 0
                        for key in keys - {"kind", "injected"}), "Invalid fault record")
                checked(point["kind"] == "point" and point["fixture"] == fixture and point["ordinal"] == ordinal and
                        point["status"] == 7 and point["injected"] is True and point["attempts"] > ordinal and
                        point["held"] == point["baseline"] == measured["baseline"] and
                        point["blocks"] == point["bytes"] == 0, "Fault ordinal, typed OOM or cleanup differs")
                checked(union is not None and ordinal not in union[fixture], "Duplicate fault ordinal")
                union[fixture].add(ordinal)
                points += 1
        if mode == "bounds":
            for boundary in range(3):
                exact(next(records), {"kind": "boundary", "fixture": fixture, "boundary": boundary,
                                      "limit": measured["peak"] - 1 + boundary, "status": 12 if boundary == 0 else 0,
                                      "blocks": 0, "bytes": 0})
        exact(next(records), {"kind": "complete", "fixture": fixture, "points": points,
                              "boundaries": 3 if mode == "bounds" else 0, "blocks": 0, "bytes": 0})
        total += points
    exact(next(records), {"kind": "finished", "points": total, "boundaries": 6 if mode == "bounds" else 0,
                          "blocks": 0, "bytes": 0})
    checked(next(records, None) is None, "Extra report material")
    return actual


class WindowsJob:
    def __init__(self):
        self.handle = establish_job()
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        signatures = {
            "QueryInformationJobObject": ([wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                           wintypes.DWORD, ctypes.c_void_p], wintypes.BOOL),
            "OpenProcess": ([wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE),
            "IsProcessInJob": ([wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)], wintypes.BOOL),
            "TerminateProcess": ([wintypes.HANDLE, wintypes.UINT], wintypes.BOOL),
            "WaitForSingleObject": ([wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD),
            "CloseHandle": ([wintypes.HANDLE], wintypes.BOOL),
            "GetProcessTimes": ([wintypes.HANDLE, *[ctypes.POINTER(wintypes.FILETIME)] * 4], wintypes.BOOL),
        }
        for name, (arguments, result) in signatures.items():
            call = getattr(self.kernel, name)
            call.argtypes, call.restype = arguments, result

    def members(self, deadline):
        capacity = 4
        while True:
            remaining(deadline)
            data = ctypes.create_string_buffer(8 + capacity * ctypes.sizeof(ctypes.c_size_t))
            if self.kernel.QueryInformationJobObject(self.handle, 3, data, len(data), None):
                assigned = ctypes.c_uint32.from_buffer(data, 0).value
                count = ctypes.c_uint32.from_buffer(data, 4).value
                checked(count <= capacity, "Invalid actual Job process inventory")
                if count < assigned:
                    capacity = max(capacity * 2, assigned)
                    continue
                return {ctypes.c_size_t.from_buffer(data, 8 + n * ctypes.sizeof(ctypes.c_size_t)).value
                        for n in range(count)}
            checked(ctypes.get_last_error() == 234, "Cannot query owned Job process inventory")
            capacity *= 2

    def active(self):
        class Accounting(ctypes.Structure):
            _fields_ = [("user", ctypes.c_longlong), ("kernel", ctypes.c_longlong),
                        ("period_user", ctypes.c_longlong), ("period_kernel", ctypes.c_longlong),
                        ("faults", ctypes.c_uint32), ("total", ctypes.c_uint32),
                        ("active", ctypes.c_uint32), ("terminated", ctypes.c_uint32)]
        data = Accounting()
        checked(self.kernel.QueryInformationJobObject(self.handle, 1, ctypes.byref(data), ctypes.sizeof(data), None),
                "Cannot query actual Job active processes")
        return data.active

    def times(self, process):
        values = [wintypes.FILETIME() for _ in range(4)]
        checked(self.kernel.GetProcessTimes(wintypes.HANDLE(int(process._handle)),
                *[ctypes.byref(value) for value in values]), "Cannot read actual child creation/exit identity")
        return tuple((value.dwHighDateTime << 32) | value.dwLowDateTime for value in values[:2])

    def terminate_member(self, pid, deadline):
        handle = self.kernel.OpenProcess(0x00101001, False, pid)
        if not handle:
            checked(ctypes.get_last_error() == 87, "Cannot open remaining Job descendant")
            return
        try:
            member = wintypes.BOOL()
            checked(self.kernel.IsProcessInJob(handle, self.handle, ctypes.byref(member)), "Cannot bind descendant to Job")
            if member.value:
                if self.kernel.WaitForSingleObject(handle, 0) == 258:
                    terminated = self.kernel.TerminateProcess(handle, 1)
                    checked(terminated or self.kernel.WaitForSingleObject(handle, 0) == 0,
                            "Cannot terminate owned descendant")
                checked(self.kernel.WaitForSingleObject(handle, max(1, int(remaining(deadline) * 1000))) == 0,
                        "Owned descendant did not terminate within original deadline")
        finally:
            checked(self.kernel.CloseHandle(handle), "Cannot close descendant process handle")

    def drained(self, deadline, terminate=False):
        while True:
            members = self.members(deadline)
            checked(os.getpid() in members, "Parent lost owned Job membership")
            others = members - {os.getpid()}
            if not others and self.active() == 1:
                return
            checked(terminate or not others, "Unexpected owned descendants after completed workers")
            for pid in others:
                self.terminate_member(pid, deadline)
            time.sleep(min(.001, remaining(deadline)))


class Run:
    def __init__(self, executable, deadline):
        self.executable, self.deadline = executable, deadline
        self.root = Path(tempfile.mkdtemp(prefix="xray-root-parameter-compiler-"))
        self.nonce, self.active = uuid.uuid4().hex, []
        self.manifest = {"qualified": False, "workers": WORKERS, "timeout": TIMEOUT,
                         "commands": [], "reports": {}, "scope": "Original two fixtures, all fault ordinals, three held boundaries"}
        self.job = WindowsJob()

    def publish(self):
        (self.root / "result.json").write_text(json.dumps(self.manifest, indent=2) + "\n", encoding="utf-8")

    def launch(self, name, arguments, expected=0):
        remaining(self.deadline)
        checked(len(self.active) < WORKERS, "Original two-worker process budget exceeded")
        self.manifest.pop("native_drain", None)
        command = [str(self.executable), *arguments]
        with (self.root / (name + ".stdout")).open("xb") as stdout, (self.root / (name + ".stderr")).open("xb") as stderr:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                       close_fds=True, creationflags=subprocess.CREATE_NO_WINDOW)
        row = {"name": name, "argv": command, "pid": process.pid, "expected_exit": expected}
        self.active.append((process, row))
        self.manifest["commands"].append(row)
        row["creation_filetime"] = self.job.times(process)[0]
        checked(row["creation_filetime"] > 0, "Missing actual child creation time")
        self.publish()

    def wait(self, terminate=False):
        failures = []
        if terminate:
            for process, _ in self.active:
                if process.poll() is None:
                    try:
                        process.kill()
                    except OSError:
                        if process.poll() is None:
                            self.job.terminate_member(process.pid, self.deadline)
        while self.active:
            remaining(self.deadline)
            for process, row in list(self.active):
                status = process.poll()
                if status is None:
                    continue
                process.wait(timeout=remaining(self.deadline))
                try:
                    creation, exited = self.job.times(process)
                    checked(creation == row.get("creation_filetime", creation) and exited >= creation,
                            "Changed child identity or missing terminal time")
                    row.update(creation_filetime=creation, exit_filetime=exited, exit_code=status)
                except BaseException as error:
                    if not terminate:
                        raise
                    failures.append(str(error))
                    row.update(exit_code=status, terminal_identity_failure=str(error))
                process._handle.Close()
                self.active.remove((process, row))
                self.publish()
                checked(terminate or status == row["expected_exit"], f"{row['name']} exited {status}; see actual logs")
            if self.active:
                time.sleep(min(.01, remaining(self.deadline)))
        self.job.drained(self.deadline, terminate)
        self.manifest["native_drain"] = {"active_processes": 1, "parent_pid": os.getpid()}
        checked(not failures, "Missing child terminal identity after cleanup: " + "; ".join(failures))

    def stage(self, name, mode, index=None):
        checked(mode == "shard" or not self.active, "Census or bounds overlap fault workers")
        arguments = ["--compiler-" + mode]
        if index is not None:
            arguments.append(str(index))
        self.launch(name, [*arguments, self.nonce, str(self.root / (name + ".jsonl"))])

    def report(self, name, mode, index=0, baseline=None, union=None):
        path = self.root / (name + ".jsonl")
        result = verify(path, mode, index, self.nonce, self.deadline, baseline, union)
        self.manifest["reports"][path.name] = sha(path, self.deadline)
        return result


def execute(run, fixtures):
    before = input_snapshot(fixtures, run.deadline)
    negatives = [("arity", ["--compiler-shard"]), ("unknown", ["--compiler-unknown"])]
    for text in ("2", "-1", "00"):
        negatives.append(("index-" + text, ["--compiler-shard", text, run.nonce, str(run.root / ("reject-" + text))]))
    negatives.append(("nonce", ["--compiler-census", "x" * 32, str(run.root / "reject-nonce")]))
    for name, arguments in negatives:
        run.launch("reject-" + name, arguments, expected=1)
        run.wait()
    checked(not any(path.name.startswith("reject-") and path.suffix not in {".stdout", ".stderr"}
                    for path in run.root.iterdir()), "Rejected parameters wrote a report")
    checked(input_snapshot(fixtures, run.deadline) == before, "Rejected parameters wrote Source inputs")
    run.stage("census", "census")
    run.wait()
    baseline = run.report("census", "census")
    inputs = input_snapshot(fixtures, run.deadline)
    checked(all(value is not None for value in inputs.values()), "Census omitted an original Source input")
    run.manifest.update(census=baseline, inputs=inputs)
    for index in range(WORKERS):
        run.stage(f"shard-{index}", "shard", index)
    run.wait()
    union = [set() for _ in FIXTURES]
    for index in range(WORKERS):
        run.report(f"shard-{index}", "shard", index, baseline, union)
    checked(all(actual == set(range(row["sites"])) for actual, row in zip(union, baseline)),
            "Original full fault ordinal union has omissions")
    run.manifest["completed_ordinals"] = [sorted(actual) for actual in union]
    run.stage("bounds", "bounds")
    run.wait()
    run.report("bounds", "bounds", baseline=baseline)
    checked(inputs == input_snapshot(fixtures, run.deadline), "A worker wrote original Source inputs")
    return baseline


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--fixtures-root", type=Path, required=True)
    args = parser.parse_args()
    checked(os.name == "nt", "Root compiler partitions require Windows Job ownership")
    deadline = STARTED + TIMEOUT
    remaining(deadline)
    executable = args.executable.resolve(strict=True)
    fixtures = args.fixtures_root.resolve(strict=True)
    run = Run(executable, deadline)
    complete, baseline, summary = False, [], ""
    try:
        identity = sha(executable, deadline)
        run.manifest.update(executable=str(executable), binary_sha256=identity, fixtures_root=str(fixtures))
        run.publish()
        baseline = execute(run, fixtures)
        checked(sha(executable, deadline) == identity, "Consumed executable changed")
        complete = True
    except BaseException as error:
        run.manifest["failure"] = f"{type(error).__name__}: {error}"
    finally:
        try:
            run.wait(terminate=not complete)
            run.manifest["native_drain"] = {"active_processes": 1, "parent_pid": os.getpid()}
            remaining(deadline)
            run.manifest["logs"] = {path.name: sha(path, deadline) for path in run.root.iterdir()
                                     if path.suffix in {".stdout", ".stderr"}}
        except BaseException as error:
            complete = False
            run.manifest["cleanup_failure"] = f"{type(error).__name__}: {error}"
        try:
            remaining(deadline)
            if complete:
                summaries = []
                for name, row in zip(("apply", "capture"), baseline):
                    summaries.append(f"H1 Source {name} full same-ledger pipeline compilerOOM={row['sites']} sites={row['sites']} "
                                     f"mode=all observed allocated={row['allocated']} peak={row['peak']} allocations={row['allocations']} "
                                     "held-minus/exact/plus/physicalbaseline PASS\n")
                summary = "".join(summaries)
            print(f"Root compiler evidence: {run.root}", flush=True)
            run.manifest.update(qualified=complete, seconds=time.monotonic() - STARTED)
            run.publish()
            remaining(deadline)
        except BaseException as error:
            complete = False
            run.manifest.update(qualified=False, failure=f"{type(error).__name__}: {error}")
            run.publish()
    # Successful summaries are the final output. Earlier evidence, deadline or
    # drain failures must never leave the original all-ordinal PASS text behind.
    if complete:
        sys.stderr.write(summary)
    return 0 if complete else 1


if __name__ == "__main__":
    raise SystemExit(main())
