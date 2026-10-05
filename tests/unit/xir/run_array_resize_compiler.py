"""Complete Array resize compiler faults in owned, bounded Windows processes."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "lib"))
from xraytest.windows_owned_job import establish_job

WORKERS = 8
TIMEOUT = 300
MAX_REPORT = 16 * 1024 * 1024
PHYSICAL = ("compiler_blocks", "compiler_bytes", "runtime_blocks", "runtime_bytes")


def checked(condition, message):
    if not condition:
        raise RuntimeError(message)


def records(path):
    checked(path.is_file() and path.stat().st_size <= MAX_REPORT, f"Missing/oversize report: {path}")
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def zero_physical(record):
    checked(all(type(record.get(key)) is int and record[key] == 0 for key in PHYSICAL),
            f"Residual physical owners: {record}")


def baseline(record, mode):
    checked(record.get("kind") == "baseline" and record.get("mode") == mode, "Wrong fresh baseline")
    sites = record.get("sites")
    checked(type(sites) is int and 0 < sites <= 67108864, "Nonfinite whole-graph site denominator")
    checked(record.get("oom_status") == 7 and record.get("functions", 0) > 0 and
            0 < record.get("c_bytes", 0) <= 1048576, "Complete Program/C-output contract changed")
    checked((record.get("allocated_cap"), record.get("live_cap"), record.get("work_cap")) ==
            (67108864, 8388608, 128000000), "Whole-graph resource caps changed")
    for value, cap in (("allocated", "allocated_cap"), ("peak", "live_cap"), ("work", "work_cap")):
        checked(type(record.get(value)) is int and 1 < record[value] <= record[cap], "Baseline budget changed")
    zero_physical(record)
    return sites


def complete(record, mode, index, workers, sites):
    expected = {"kind": "complete", "mode": mode, "index": index, "workers": workers, "sites": sites,
                **{key: 0 for key in PHYSICAL}}
    checked(record == expected, "Missing exact completion/physical-zero sentinel")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--reports-root", type=Path, required=True)
    parser.add_argument("--mode", type=int, choices=range(4), required=True)
    parser.add_argument("--workers", type=int, required=True)
    parser.add_argument("--component", choices=("transaction", "matrix1", "matrix2", "matrix3", "matrix4", "matrix5", "matrix6", "matrix7"), default="transaction")
    parser.add_argument("--shared-prefix-root", type=Path)
    parser.add_argument("--prefix-only", action="store_true")
    options = parser.parse_args()
    checked(not options.prefix_only or (options.mode == 0 and options.shared_prefix_root), "Shared prefix requires VM mode and owned report root")
    checked(os.name == "nt", "Parallel compiler gate is registered only on Windows")
    checked(1 <= options.workers <= WORKERS, "Worker count must be in 1..8")
    start = time.monotonic()
    deadline = start + TIMEOUT
    executable = options.executable.resolve(strict=True)
    root = options.reports_root / (time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex)
    root.mkdir(parents=True, exist_ok=False)
    manifest = {"qualified": False, "workers": options.workers, "mode": options.mode,
                "timeout": TIMEOUT, "executable": str(executable),
                "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                "component": options.component, "commands": [], "completed_ordinals": [], "inherited_ordinals": []}
    fixture = Path(__file__).resolve().parents[2] / "fixtures/xir_array_resize" / options.component
    inputs = [fixture / "root.xr", fixture / "producer.xr", Path(__file__).resolve()]
    manifest["inputs"] = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    active = []

    def publish():
        (root / "qualification.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    def launch(name, arguments):
        checked(time.monotonic() < deadline, "Original overall 300-second deadline exhausted")
        command = [str(executable), *arguments, str(root / (name + ".jsonl"))]
        log = (root / (name + ".log")).open("xb")
        try:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                                       stderr=subprocess.STDOUT, close_fds=True)
        finally:
            log.close()
        active.append((name, process))
        manifest["commands"].append({"name": name, "argv": command, "pid": process.pid})
        publish()

    def wait_phase():
        while active:
            checked(time.monotonic() < deadline, "Original overall 300-second deadline exhausted")
            for name, process in list(active):
                status = process.poll()
                if status is not None:
                    active.remove((name, process))
                    manifest["commands"][next(i for i, row in enumerate(manifest["commands"])
                                               if row["name"] == name)]["exit_code"] = status
                    checked(status == 0, f"{name} exited {status}; see {root / (name + '.log')}")
            if active:
                time.sleep(0.01)

    try:
        # Assign this parent before the first child. Kernel termination closes
        # its sole non-inherited Job handle, including when CTest kills us.
        manifest["windows_job"] = establish_job()
        publish()
        mode = str(options.mode)
        launch("baseline", ["--compiler-baseline", mode])
        wait_phase()
        rows = records(root / "baseline.jsonl")
        checked(len(rows) == 3, "Incomplete normal baseline")
        prefix = rows[0]
        checked(prefix.get("kind") == "prefix_identity" and type(prefix.get("prefix_sites")) is int,
                "Missing actual pre-seal prefix identity")
        checked(all(isinstance(prefix.get(key), str) and len(prefix[key]) == 64 and
                    all(c in "0123456789abcdef" for c in prefix[key]) for key in ("c_sha256", "proof_sha256")), "Invalid typed artifacts")
        sites = baseline(rows[1], options.mode)
        first = rows[1]
        cut = prefix["prefix_sites"]
        checked(0 < cut < sites, "Missing actual prefix/tail partition")
        complete(rows[2], options.mode, 0, 1, sites)
        manifest["prefix"] = prefix
        begin, end = 0, cut if options.prefix_only else sites
        shared = None
        certificate = options.shared_prefix_root / (options.component + ".json") if options.shared_prefix_root else None
        if certificate and not options.prefix_only and certificate.is_file():
            shared = json.loads(certificate.read_text(encoding="utf-8"))
            checked(shared.get("qualified") is True and shared.get("prefix_only") is True and
                    shared.get("prefix") == prefix and shared.get("inputs") == manifest["inputs"] and
                    shared.get("executable_sha256") == manifest["executable_sha256"], "Stale/different shared prefix proof")
            checked(shared.get("completed_ordinals") == list(range(cut)), "Incomplete shared prefix responsibility")
            inherited = set()
            shared_workers = shared.get("workers")
            checked(type(shared_workers) is int and 1 <= shared_workers <= WORKERS and
                    len(shared.get("shard_reports", {})) == shared_workers and shared.get("mode") == 0 and
                    shared.get("executable") == str(executable), "Wrong shared execution owner/partition")
            for index, (name, digest) in enumerate(shared["shard_reports"].items()):
                report = Path(name)
                checked(hashlib.sha256(report.read_bytes()).hexdigest() == digest, "Shared point report changed")
                data = records(report)
                checked(len(data) >= 4 and data[0] == prefix and data[1] == shared["baseline"], "Shared worker prefix/baseline changed")
                shared_sites = baseline(data[1], 0)
                complete(data[-1], 0, index, shared_workers, shared_sites)
                checked(data[-2] == {"kind": "range", "begin": 0, "end": cut}, "Shared worker range changed")
                checked([row.get("ordinal") for row in data[2:-2]] == list(range(index, cut, shared_workers)), "Shared worker partition incomplete")
                for row in data[2:-2]:
                    ordinal = row.get("ordinal")
                    checked(type(ordinal) is int and 0 <= ordinal < cut and ordinal not in inherited and
                            row.get("kind") == "point" and row.get("mode") == 0 and row.get("status") == 7 and
                            row.get("injected") is True and row.get("empty_output") is True and
                            type(row.get("attempts")) is int and row["attempts"] > ordinal and
                            type(row.get("stage")) is int and 0 <= row["stage"] <= 6, "Invalid inherited actual prefix point")
                    zero_physical(row); inherited.add(ordinal)
            checked(inherited == set(range(cut)), "Shared actual ordinal union is incomplete")
            manifest["inherited_ordinals"] = sorted(inherited)
            manifest["shared_prefix_certificate"] = {"path": str(certificate), "sha256": hashlib.sha256(certificate.read_bytes()).hexdigest(),
                                                       "actual_mode": 0, "logical_mode": options.mode}
            shared_seconds = shared.get("seconds")
            checked(type(shared_seconds) in (int, float) and math.isfinite(shared_seconds) and 0 < shared_seconds < TIMEOUT,
                    "Shared prefix did not finish within original parent deadline")
            deadline = start + TIMEOUT - shared_seconds
            checked(time.monotonic() < deadline, "Original logical parent shared-prefix plus suffix deadline exhausted")
            manifest["shared_prefix_seconds"] = shared_seconds
            begin = cut
        manifest["prefix_only"] = options.prefix_only
        manifest["range"] = [begin, end]
        manifest["baseline"] = first
        manifest["sites"] = sites
        for index in range(options.workers):
            arguments = ["--compiler-range-shard" if (options.prefix_only or shared) else "--compiler-shard",
                         mode, str(index), str(options.workers), str(sites)]
            if options.prefix_only or shared: arguments.extend((str(begin), str(end)))
            launch(f"shard-{index}", arguments)
        wait_phase()
        union = set()
        manifest["shard_reports"] = {}
        for index in range(options.workers):
            rows = records(root / f"shard-{index}.jsonl")
            checked(len(rows) >= 3 and rows[0] == prefix and rows[1] == first, "Worker fresh complete baseline differs")
            baseline(rows[1], options.mode)
            ranged = options.prefix_only or shared is not None
            if ranged: checked(rows[-2] == {"kind": "range", "begin": begin, "end": end}, "Wrong actual worker range")
            manifest["shard_reports"][str(root / f"shard-{index}.jsonl")] = hashlib.sha256((root / f"shard-{index}.jsonl").read_bytes()).hexdigest()
            complete(rows[-1], options.mode, index, options.workers, sites)
            observed = []
            for row in rows[2:-2 if ranged else -1]:
                ordinal = row.get("ordinal")
                checked(row.get("kind") == "point" and row.get("mode") == options.mode and
                        type(ordinal) is int and begin <= ordinal < end, "Invalid actual failure ordinal")
                checked(ordinal % options.workers == index and ordinal not in union, "Duplicate/wrong partition")
                checked(row.get("status") == 7 and row.get("injected") is True and row.get("empty_output") is True,
                        "Failure status/output contract changed")
                checked(type(row.get("attempts")) is int and row["attempts"] > ordinal and
                        type(row.get("stage")) is int and 0 <= row["stage"] <= 7, "Fault was not reached")
                checked(not ranged or (row["stage"] <= 6 if options.prefix_only else row["stage"] == 7), "Fault crossed prefix/tail boundary")
                zero_physical(row)
                union.add(ordinal)
                observed.append(ordinal)
            expected = [n for n in range(begin, end) if n % options.workers == index]
            checked(observed == expected, "Missing/reordered worker ordinals")
        checked(union == set(range(begin, end)), "Actual fault range is incomplete")
        if not options.prefix_only:
            checked(union | set(manifest["inherited_ordinals"]) == set(range(sites)) and
                    not union.intersection(manifest["inherited_ordinals"]), "Original logical fault responsibility is incomplete/duplicated")
        manifest["completed_ordinals"] = sorted(union)
        publish()
        if not options.prefix_only:
            launch("bounds", ["--compiler-bounds", mode])
            wait_phase()
            rows = records(root / "bounds.jsonl")
            checked(len(rows) == 9 and rows[0] == prefix and rows[1] == first, "Bounds normal baseline changed")
            complete(rows[-1], options.mode, 0, 1, sites)
            for axis in range(3):
                for shortfall in range(2):
                    row = rows[2 + 2 * axis + shortfall]
                    checked(row.get("kind") == "axis" and row.get("mode") == options.mode and
                            row.get("axis") == axis and row.get("shortfall") == shortfall and
                            row.get("status") == (6 if shortfall else 0), "Exact/minus1 resource responsibility missing")
                    zero_physical(row)
        checked(not active and time.monotonic() < deadline, "Incomplete process drain/deadline")
        checked(hashlib.sha256(executable.read_bytes()).hexdigest() == manifest["executable_sha256"],
                "Executable changed during complete fault census")
        checked(all(hashlib.sha256(Path(path).read_bytes()).hexdigest() == digest
                    for path, digest in manifest["inputs"].items()), "Source/driver changed during fault census")
        manifest["qualified"] = True
        manifest["seconds"] = time.monotonic() - start
        manifest["logical_parent_seconds"] = manifest["seconds"] + manifest.get("shared_prefix_seconds", 0)
        checked(manifest["logical_parent_seconds"] < TIMEOUT, "Original logical 300-second whole-parent deadline exhausted")
        publish()
        if options.prefix_only:
            certificate.parent.mkdir(parents=True, exist_ok=True)
            temporary = certificate.with_suffix("." + uuid.uuid4().hex + ".tmp")
            temporary.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
            os.replace(temporary, certificate)
        print(f"Array resize mode{options.mode}: logical {sites}, actual {len(union)}, inherited {len(manifest['inherited_ordinals'])}; physical0; {root}")
        return 0
    except Exception as error:
        manifest["failure"] = str(error)
        manifest["seconds"] = time.monotonic() - start
        publish()
        print(f"Array resize FAIL: {error}; {root}", file=sys.stderr)
        return 1
    finally:
        for _, process in active:
            if process.poll() is None:
                process.kill()
        for _, process in active:
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass
        # Never CloseHandle the Job containing this live parent; process exit
        # closes the sole handle and kills any descendants not yet drained.


if __name__ == "__main__":
    raise SystemExit(main())
