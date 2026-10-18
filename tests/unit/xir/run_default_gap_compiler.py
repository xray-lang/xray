"""Complete default-gap compiler ordinals in owned, bounded Windows processes."""
from __future__ import annotations
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "lib"))
from xraytest.windows_owned_job import establish_job

WORKERS = 8
# The original registrations have no TIMEOUT property; this is CTest's effective default.
TIMEOUT = 10000000
CAPS = (33554432, 8388608, 64000000)
PHYSICAL = ("compiler_blocks", "compiler_bytes", "runtime_blocks", "runtime_bytes", "runtime_owned")


def checked(condition, message):
    if not condition:
        raise RuntimeError(message)


def records(path):
    checked(path.is_file() and path.stat().st_size <= 16 * 1024 * 1024, f"Missing/oversize report: {path}")
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def zero(record):
    checked(all(type(record.get(k)) is int and record[k] == 0 for k in PHYSICAL), "Residual physical ownership")


def common(record, first):
    zero(record)
    checked(record.get("mode") == first["mode"] and record.get("sites") == first["sites"], "Wrong mode/denominator")
    checked(tuple(record.get(k) for k in ("allocated_cap", "live_cap", "work_cap")) == CAPS, "Original caps changed")
    checked(all(record.get(k) == first[k] for k in ("allocated", "peak", "work")), "Whole-graph baseline changed")


def baseline(record, mode):
    checked(record.get("kind") == "baseline" and record.get("mode") == mode, "Missing complete graph baseline")
    checked(type(record.get("sites")) is int and 0 < record["sites"] < 20000, "Original site bound changed")
    common(record, record)
    checked(record.get("attempts") == record["sites"] and record.get("status") == 0 and
            record.get("injected") is False and record.get("empty_output") is False and
            record.get("ledger_oom") is True, "Baseline/new-ledger OOM contract changed")
    checked(all(type(record.get(k)) is int and 1 < record[k] <= cap
                for k, cap in zip(("allocated", "peak", "work"), CAPS)), "Invalid baseline resources")
    return record["sites"]


def complete(record, mode, index, workers, sites, points, axes):
    checked(record == dict(kind="complete", mode=mode, index=index, workers=workers,
                           sites=sites, points=points, axes=axes, **{k: 0 for k in PHYSICAL}),
            "Missing exact completion sentinel")


def verify_shard(rows, first, index, workers):
    checked(len(rows) >= 2 and rows[0] == first, "Worker complete baseline differs")
    sites = baseline(rows[0], first["mode"])
    expected = list(range(index, sites, workers))
    complete(rows[-1], first["mode"], index, workers, sites, len(expected), 0)
    observed = []
    for row in rows[1:-1]:
        common(row, first)
        ordinal = row.get("ordinal")
        checked(row.get("kind") == "point" and type(ordinal) is int and 0 <= ordinal < sites,
                "Invalid fault ordinal")
        checked(row.get("status") == 7 and row.get("injected") is True and
                row.get("empty_output") is True and row.get("ledger_oom") is True and
                type(row.get("attempts")) is int and row["attempts"] > ordinal,
                "Fault status, actual hit or empty output changed")
        observed.append(ordinal)
    checked(observed == expected, "Missing/duplicate/reordered partition ordinal")
    return observed


def verify_union(partitions, sites):
    seen = set()
    for values in partitions:
        checked(not seen.intersection(values), "Duplicate cross-process ordinal")
        seen.update(values)
    checked(seen == set(range(sites)), "Incomplete original denominator")
    return sorted(seen)


def verify_bounds(rows, first):
    checked(len(rows) == 6 and rows[0] == first, "Bounds baseline differs")
    complete(rows[-1], first["mode"], 0, 1, first["sites"], 0, 4)
    for axis, row in enumerate(rows[1:-1]):
        common(row, first)
        checked(row.get("kind") == "axis" and row.get("axis") == axis and
                row.get("status") == (6 if axis else 0) and
                row.get("empty_output") is bool(axis), "Exact/three-minus-one responsibility changed")


def sanitizer_report(data):
    return any(marker in data for marker in (b"ERROR: AddressSanitizer", b"AddressSanitizer:DEADLYSIGNAL",
                                             b"WARNING: ThreadSanitizer", b"runtime error:"))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cpu_seconds(process):
    api = ctypes.WinDLL("kernel32", use_last_error=True).GetProcessTimes
    api.argtypes = [wintypes.HANDLE, *([ctypes.POINTER(wintypes.FILETIME)] * 4)]
    api.restype = wintypes.BOOL
    created, exited, kernel, user = (wintypes.FILETIME() for _ in range(4))
    checked(api(int(process._handle), ctypes.byref(created), ctypes.byref(exited),
                ctypes.byref(kernel), ctypes.byref(user)), "Cannot record owned child CPU time")
    return sum((x.dwHighDateTime << 32) + x.dwLowDateTime for x in (kernel, user)) / 10000000


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--reports-root", type=Path, required=True)
    parser.add_argument("--mode", type=int, choices=range(4), required=True)
    parser.add_argument("--packet", type=Path, required=True)
    parser.add_argument("--generated-c", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=WORKERS)
    args = parser.parse_args()
    checked(os.name == "nt" and 1 <= args.workers <= WORKERS, "Windows worker count must be in 1..8")
    started = time.monotonic()
    deadline = started + TIMEOUT
    exe = args.executable.resolve(strict=True)
    root = args.reports_root / (time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex)
    root.mkdir(parents=True, exist_ok=False)
    repo = Path(__file__).resolve().parents[3]
    inputs = [exe, Path(__file__).resolve(), repo / "tests/lib/xraytest/windows_owned_job.py",
              *sorted((repo / "tests/fixtures/xir_default_gaps").rglob("*")),
              *(repo / "tests/unit/xir" / p for p in ("test_xir_default_gap_execution.c",
                "xir_default_execution_owner.h", "xir_default_gap_runtime.h", "xir_default_gap_owned.h",
                "xir_default_gap_partitions.h"))]
    if args.mode:
        inputs.append(args.packet.resolve(strict=True))
    if args.mode >= 2:
        inputs.append(args.generated_c.resolve(strict=True))
    inputs = [p for p in inputs if p.is_file()]
    hashes = {str(p): sha(p) for p in inputs}
    manifest = dict(qualified=False, mode=args.mode, workers=args.workers, timeout=TIMEOUT,
                    executable=str(exe), inputs=hashes, commands=[], completed_ordinals=[],
                    logical_cpus=os.cpu_count(), maximum_active=0)
    active = []
    artifacts = {}

    def publish():
        (root / "qualification.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    def launch(name, option, extra=()):
        checked(time.monotonic() < deadline, "Original overall deadline exhausted")
        folder = root / name
        folder.mkdir()
        packet = args.packet.resolve() if args.mode == 1 else folder / "actual-input.xrc"
        source = folder / "generated.c"
        report = folder / "report.jsonl"
        if option == "--normal-only":
            arguments = [option, str(packet), str(source)] if args.mode <= 1 else [option]
        else:
            arguments = [option, str(packet), str(source), *map(str, extra), str(report)]
        command = [str(exe), *arguments]
        log = folder / "process.log"
        with log.open("xb") as output:
            child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=output,
                                     stderr=subprocess.STDOUT, close_fds=True)
        row = dict(name=name, argv=command, pid=child.pid)
        active.append((child, row, log))
        artifacts[name] = dict(packet=packet, source=source, report=report)
        manifest["commands"].append(row)
        manifest["maximum_active"] = max(manifest["maximum_active"], len(active))
        publish()

    def wait_phase():
        while active:
            checked(time.monotonic() < deadline, "Original overall deadline exhausted")
            for child, row, log in list(active):
                status = child.poll()
                if status is not None:
                    row.update(exit_code=status, cpu_seconds=cpu_seconds(child))
                    active.remove((child, row, log))
                    publish()
                    checked(not sanitizer_report(log.read_bytes()), f"Sanitizer report in {log}")
                    checked(status == 0, f"{row['name']} exited {status}; see {log}")
            if active:
                time.sleep(.01)

    def artifact(name):
        paths = artifacts[name]
        selected = paths["source"] if args.mode == 1 else paths["packet"]
        checked(selected.is_file(), f"Missing actual compiler input/output: {selected}")
        value = sha(selected)
        manifest.setdefault("actual_artifacts", {})[name] = dict(path=str(selected), sha256=value)
        return value

    try:
        manifest["windows_job"] = establish_job()
        publish()
        launch("normal", "--normal-only")
        wait_phase()
        normal = (root / "normal/process.log").read_text(encoding="utf-8")
        checked("runtime OOM=48 physical baseline restored" in normal and
                "normal runtime and lifetime outputs retained physicalzero" in normal,
                "Original runtime48/two-Instance/retained/code-lease control missing")
        normal_hash = artifact("normal") if args.mode <= 1 else None
        launch("baseline", "--compiler-baseline")
        wait_phase()
        rows = records(artifacts["baseline"]["report"])
        checked(len(rows) == 2, "Incomplete baseline report")
        first = rows[0]
        sites = baseline(first, args.mode)
        complete(rows[1], args.mode, 0, 1, sites, 0, 0)
        reference = artifact("baseline")
        checked(normal_hash is None or reference == normal_hash, "Normal and compiler baseline artifacts differ")
        manifest.update(baseline=first, sites=sites)
        for index in range(args.workers):
            launch(f"shard-{index}", "--compiler-shard", (index, args.workers, sites))
        wait_phase()
        partitions = []
        for index in range(args.workers):
            name = f"shard-{index}"
            partitions.append(verify_shard(records(artifacts[name]["report"]), first, index, args.workers))
            checked(artifact(name) == reference, "Worker actual input/C artifact differs")
        manifest["completed_ordinals"] = verify_union(partitions, sites)
        launch("bounds", "--compiler-bounds", (sites,))
        wait_phase()
        verify_bounds(records(artifacts["bounds"]["report"]), first)
        checked(artifact("bounds") == reference, "Bounds actual input/C artifact differs")
        checked(not active and time.monotonic() < deadline, "Incomplete process drain/deadline")
        checked(all(sha(Path(p)) == h for p, h in hashes.items()), "Source, fixture, executable or input changed")
        manifest.update(qualified=True, seconds=time.monotonic() - started,
                        cpu_seconds=sum(p["cpu_seconds"] for p in manifest["commands"]))
        publish()
        print(f"Default gap mode{args.mode}: all {sites} compiler OOM ordinals, runtime48, exact/three-minus-one, physical0; {root}")
        return 0
    except (Exception, KeyboardInterrupt) as error:
        manifest.update(failure=str(error), seconds=time.monotonic() - started)
        publish()
        print(f"Default gap FAIL: {error}; {root}", file=sys.stderr)
        return 1
    finally:
        cleanup = []
        for child, row, _ in active:
            if child.poll() is None:
                child.kill()
            try:
                cleanup.append(dict(name=row["name"], exit_code=child.wait(timeout=2)))
            except subprocess.TimeoutExpired:
                cleanup.append(dict(name=row["name"], failure="Owned child did not drain; Job closes on parent exit"))
        if cleanup:
            manifest["cleanup"] = cleanup
            publish()
        # The sole non-inherited Job handle closes with this parent, including hard CTest cancellation.


if __name__ == "__main__":
    raise SystemExit(main())
