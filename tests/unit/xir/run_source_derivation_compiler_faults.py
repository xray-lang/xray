"""Cover every automatic derivation allocation within one 600-second deadline."""
from __future__ import annotations

import time
STARTED = time.monotonic()

import argparse
import concurrent.futures
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import uuid

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "lib"))
from xraytest.windows_owned_job import establish_job
from xraytest.proc import _terminate_tree

WORKERS = 8
DEADLINE = STARTED + 600
CAPS = [67108864, 8388608, 128000000]
ACTIVE = set()
ACTIVE_LOCK = threading.Lock()
CANCELED = threading.Event()


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def remaining():
    seconds = DEADLINE - time.monotonic()
    check(seconds > 0, "Original global 600-second deadline exhausted")
    return seconds


def digest(path):
    remaining()
    return hashlib.sha256(path.read_bytes()).hexdigest()


def invoke(exe, folder, name, arguments):
    remaining()
    with (folder / (name + ".log")).open("wb") as log:
        with ACTIVE_LOCK:
            check(not CANCELED.is_set(), "Qualification already failed")
            process = subprocess.Popen([str(exe), *map(str, arguments)], stdout=log, stderr=subprocess.STDOUT)
            ACTIVE.add(process)
        try:
            code = process.wait(timeout=remaining())
        except BaseException:
            _terminate_tree(process)
            raise
        finally:
            with ACTIVE_LOCK:
                ACTIVE.discard(process)
    check(code == 0, f"{name} exited {code}; see {folder / (name + '.log')}")
    remaining()


def rows(path):
    remaining()
    check(path.is_file() and path.stat().st_size <= 8 * 1024 * 1024, f"Missing or oversized report: {path}")
    result = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
    check(len(result) >= 2 and result[0]["event"] == "input" and result[-1]["event"] == "complete",
          f"Incomplete operation: {path}")
    check(result[0]["caps"] == CAPS and result[-1]["compiler_blocks"] == 0 and
          result[-1]["compiler_bytes"] == 0, f"Caps or physical ownership changed: {path}")
    return result


def main():
    check(os.name in {"nt", "posix"}, "Native process ownership is required")
    if os.name == "nt":
        establish_job()
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    exe = args.exe.resolve()
    folder = args.output.resolve() / uuid.uuid4().hex
    folder.mkdir(parents=True)
    binary = digest(exe)
    runner = digest(Path(__file__))
    packet = folder / "generator.chk"
    invoke(exe, folder, "prepare", ["--emit-generator", packet])
    packet_sha = digest(packet)
    census_path = folder / "census.jsonl"
    invoke(exe, folder, "census", ["--compiler-fi", packet, 0, 0, 0, census_path])
    census = rows(census_path)
    check(len(census) == 3 and census[1]["event"] == "census" and
          census[0]["packet_sha256"] == packet_sha, "Invalid real operation census")
    sites = census[1]["sites"]
    check(isinstance(sites, int) and sites > WORKERS, "Empty or invalid allocation census")
    print(f"Automatic Source derivation: {sites} actual compiler sites, {WORKERS} partitions, caps={CAPS}", flush=True)
    invoke(exe, folder, "controls", ["--compiler-controls"])

    def partition(index):
        first, end = sites * index // WORKERS, sites * (index + 1) // WORKERS
        report = folder / f"partition-{index}.jsonl"
        invoke(exe, folder, f"partition-{index}", ["--compiler-fi", packet, first, end, sites, report])
        result = rows(report)
        check(result[0] == {"event": "input", "first": first, "end": end, "sites": sites,
                           "packet_sha256": packet_sha, "caps": CAPS}, f"Partition {index} input changed")
        observed = result[1:-1]
        check(len(observed) == end - first and result[-1]["points"] == end - first,
              f"Partition {index} has missing points")
        for ordinal, row in zip(range(first, end), observed):
            check(row["event"] == "fault" and row["ordinal"] == ordinal and row["attempts"] == ordinal + 1,
                  f"Partition {index} changed original fault ordinal {ordinal}")
            check(row["owner"] == (0 if ordinal == 0 else 1), f"Unexpected ledger bootstrap at {ordinal}")
        return {"index": index, "first": first, "end": end, "points": len(observed),
                "report_sha256": digest(report)}

    with concurrent.futures.ThreadPoolExecutor(max_workers=WORKERS) as pool:
        pending = [pool.submit(partition, index) for index in range(WORKERS)]
        try:
            partitions = [future.result() for future in concurrent.futures.as_completed(pending, timeout=remaining())]
        except BaseException:
            with ACTIVE_LOCK:
                CANCELED.set()
                for process in ACTIVE:
                    _terminate_tree(process)
            raise
    partitions.sort(key=lambda row: row["index"])
    check(sum(row["points"] for row in partitions) == sites, "Full census coverage changed")
    check(digest(exe) == binary and digest(packet) == packet_sha and digest(Path(__file__)) == runner,
          "Executable, authentic packet or runner changed during qualification")
    summary = {"status": "PASS", "sites": sites, "workers": WORKERS, "caps": CAPS,
               "deadline_seconds": 600, "elapsed_seconds": time.monotonic() - STARTED,
               "binary_sha256": binary, "packet_sha256": packet_sha, "runner_sha256": runner,
               "census": census[1], "partitions": partitions,
               "scope": "Actual catalog, generator execution, both Source constructions and Checked receiving; "
                        "all faults retain same-ledger retry and physical baseline; whole P12 not inferred"}
    remaining()
    (folder / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary), flush=True)


def posix_parent():
    read_fd, write_fd = os.pipe()
    os.set_inheritable(write_fd, False)
    child = None
    try:
        command = [sys.executable, "-B", str(Path(__file__).resolve()), "--posix-owner",
                   str(read_fd), repr(STARTED), *sys.argv[1:]]
        child = subprocess.Popen(command, pass_fds=(read_fd,), close_fds=True, start_new_session=True)
        os.close(read_fd)
        read_fd = None
        return child.wait(timeout=remaining())
    except BaseException:
        if child is not None:
            _terminate_tree(child)
        raise
    finally:
        if read_fd is not None:
            os.close(read_fd)
        os.close(write_fd)


def entry():
    global STARTED, DEADLINE
    if os.name != "posix":
        main()
        return 0
    if len(sys.argv) < 2 or sys.argv[1] != "--posix-owner":
        return posix_parent()
    check(len(sys.argv) >= 5 and os.getpgrp() == os.getpid(), "Invalid POSIX process owner")
    read_fd = int(sys.argv[2])
    parent_started = float(sys.argv[3])
    check(read_fd >= 3 and math.isfinite(parent_started) and 0 < parent_started <= STARTED,
          "Invalid inherited global deadline or parent lifeline")
    STARTED = parent_started
    DEADLINE = STARTED + 600
    os.set_inheritable(read_fd, False)
    sys.argv = [sys.argv[0], *sys.argv[4:]]

    def orphan_watch():
        # The outer parent alone owns the non-inherited pipe writer. Its death
        # closes the pipe even when CTest terminates it without running finally.
        try:
            while os.read(read_fd, 1):
                pass
        finally:
            os.killpg(os.getpgrp(), signal.SIGKILL)

    threading.Thread(target=orphan_watch, name="derive-owner-lifeline", daemon=True).start()
    try:
        main()
        return 0
    except BaseException:
        os.killpg(os.getpgrp(), signal.SIGKILL)
        raise


if __name__ == "__main__":
    sys.exit(entry())
