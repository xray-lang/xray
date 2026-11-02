"""Accept reviewed complete concurrency traces and compare both real engines."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path


SOURCES = frozenset("demos/05-concurrency/" + name + ".xr" for name in (
    "goroutines", "shared_data", "select_and_scope"))


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
        if case.get("stdout_traces") is not None:
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
