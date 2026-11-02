"""Accept reviewed complete concurrency traces and compare both real engines."""
from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path


SOURCES = frozenset("demos/05-concurrency/" + name + ".xr" for name in (
    "goroutines", "shared_data", "select_and_scope"))
CHANNELS = "demos/05-concurrency/channels.xr"


@dataclass(frozen=True)
class ChannelTrace:
    prefix: tuple[bytes, ...]
    suffix: tuple[bytes, ...]

    def __contains__(self, output: bytes) -> bool:
        if not output.endswith(b"\n") or b"\r" in output:
            return False
        lines = output.split(b"\n")[:-1]
        if len(lines) != 20 or tuple(lines[:9]) != self.prefix or tuple(lines[-2:]) != self.suffix:
            return False
        seen, last, printed = set(), {1: -1, 2: -1, 3: -1}, []
        for line in lines[9:18]:
            match = re.fullmatch(rb"worker-([123]): processed job ([0-8])", line)
            if match is None:
                return False
            worker, job = int(match[1]), int(match[2])
            if job in seen or job <= last[worker]:
                return False
            seen.add(job)
            last[worker] = job
            printed.append((worker, job))
        if seen != set(range(9)):
            return False
        owners = {job: worker for worker, job in printed}
        receiving, active = 0, {}
        for worker, job in printed:
            # FIFO receives may precede prints, with one pending value per worker.
            while receiving <= job:
                target = owners[receiving]
                if target in active:
                    return False
                active[target] = receiving
                receiving += 1
            if active.get(worker) != job:
                return False
            del active[worker]
        return receiving == 9 and not active


def expected_channels(case: dict, root: Path) -> ChannelTrace | None:
    relative = case.get("stdout_channels")
    if relative is None:
        return None
    if (case["path"] != CHANNELS or case["expect"] != "pass"
            or case["mode"] not in ("run", "build-run") or not isinstance(relative, str)):
        raise ValueError("channel fanout oracle is restricted to its passing original demo")
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()) or not path.is_file():
        raise ValueError("channel oracle is missing or outside the source root")
    data = json.loads(path.read_text(encoding="utf-8"))
    keys = {"schema", "source", "sourceCanonicalSha256", "prefix", "suffix", "workers"}
    if (not isinstance(data, dict) or set(data) != keys
            or type(data["schema"]) is not int or data["schema"] != 1 or data["source"] != CHANNELS):
        raise ValueError("channel oracle requires its exact source-bound schema 1")
    source = (root / CHANNELS).read_bytes().replace(b"\r\n", b"\n")
    if hashlib.sha256(source).hexdigest() != data["sourceCanonicalSha256"]:
        raise ValueError("channel oracle source identity differs")
    workers = data["workers"]
    expected = {"lines": 9, "workerIds": [1, 2, 3], "eachJobExactlyOnce": list(range(9)),
                "perWorkerIncreasing": True, "fifoReachable": True}
    if (workers != expected or type(workers.get("lines")) is not int
            or any(type(value) is not int for key in ("workerIds", "eachJobExactlyOnce")
                   for value in workers[key]) or workers["perWorkerIncreasing"] is not True
            or workers["fifoReachable"] is not True):
        raise ValueError("channel oracle must cover all nine jobs and each worker's FIFO order")
    for key, count in (("prefix", 9), ("suffix", 2)):
        lines = data[key]
        if (not isinstance(lines, list) or len(lines) != count
                or any(not isinstance(line, str) or not line or "\r" in line or "\n" in line for line in lines)):
            raise ValueError("channel oracle requires the complete prefix and suffix")
    return ChannelTrace(tuple(line.encode("utf-8") for line in data["prefix"]),
                        tuple(line.encode("utf-8") for line in data["suffix"]))


def expected_traces(case: dict, root: Path) -> tuple[bytes, ...] | None:
    relative = case.get("stdout_traces")
    if relative is None:
        return None
    if (case["path"] not in SOURCES or case["expect"] != "pass"
            or case["mode"] not in ("run", "build-run") or not isinstance(relative, str)):
        raise ValueError("trace oracles are restricted to passing original concurrency demos")
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()) or not path.is_file():
        raise ValueError("trace oracle is missing or outside the source root")
    data = json.loads(path.read_text(encoding="utf-8"))
    keys = {"schema", "source", "sourceCanonicalSha256", "traces"}
    if (not isinstance(data, dict) or set(data) != keys
            or type(data["schema"]) is not int or data["schema"] != 1
            or data["source"] != case["path"]):
        raise ValueError("trace oracle requires its exact source-bound schema 1")
    source = (root / case["path"]).read_bytes().replace(b"\r\n", b"\n")
    if hashlib.sha256(source).hexdigest() != data["sourceCanonicalSha256"]:
        raise ValueError("trace oracle source identity differs")
    traces = data["traces"]
    if (not isinstance(traces, list) or not traces
            or any(not isinstance(trace, str) or not trace.endswith("\n") or "\r" in trace
                   for trace in traces) or len(set(traces)) != len(traces)):
        raise ValueError("trace oracle requires distinct complete LF output traces")
    return tuple(trace.encode("utf-8") for trace in traces)


def verify_pairs(cases: list[dict], rows: list[dict]) -> list[dict]:
    """A legal but different trace is still a VM/native differential failure."""
    groups: dict[str, list[dict]] = {}
    by_id = {row["id"]: row for row in rows}
    changed = []
    for case in cases:
        if case.get("stdout_traces") is not None or case.get("stdout_channels") is not None:
            groups.setdefault(case["path"], []).append(case)
    for path, pair in groups.items():
        actual = [by_id[case["id"]] for case in pair]
        reason = None
        if len(pair) != 2 or {case["mode"] for case in pair} != {"run", "build-run"}:
            reason = "trace output requires both VM and native results"
        elif any(row["status"] != "pass" for row in actual):
            reason = "trace companion did not pass its complete output oracle"
        else:
            try:
                outputs = [(Path(row["log_directory"]) / "run.stdout").read_bytes() for row in actual]
                if outputs[0] != outputs[1]:
                    reason = "VM/native complete trace bytes differ"
                else:
                    for row in actual:
                        row["trace_pair"] = {"source": path, "raw_sha256": hashlib.sha256(outputs[0]).hexdigest()}
            except OSError as error:
                reason = f"trace comparison could not read original stdout: {error}"
        if reason:
            for row in actual:
                if row["status"] == "pass":
                    row.update(status="fail", reason=reason)
                    changed.append(row)
    return changed
