#!/usr/bin/env python3
"""Cover every allocation of independent products and detached Checked owners."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import time
import uuid

from source_product_consumer_faults import ROOT, digest, inputs, write

SUMMARY = re.compile(r"detachment-summary case=(\w+) sites=(\d+) shard=(\d+) shards=(\d+) covered=(\d+) physical=0/0")
ORDINAL = re.compile(r"^detachment ordinal=(\d+) physical=0/0$", re.M)
FIXTURES = ROOT / "tests/unit/xir/product_consumers/fixtures"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--case", choices=("generics", "callables", "single_module", "text_program"), required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--jobs", type=int, choices=range(1, 9), default=8)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    scratch = args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    run = args.evidence / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:8])
    run.mkdir(parents=True, exist_ok=False)
    sources = sorted((FIXTURES / args.case).glob("*.xr"))
    if len(sources) != 1 or sources[0].name != "root.xr" or sources[0].is_symlink():
        raise ValueError("the admitted detachment fixture must own one regular root source")
    source_bytes = sources[0].read_bytes()
    private_roots = []
    for index in range(args.jobs):
        owned = scratch / (args.case + "-fault-" + uuid.uuid4().hex)
        owned.mkdir(exist_ok=False)
        if owned.resolve().parent != scratch:
            raise ValueError("private replay directory escaped its owner")
        (owned / "root.xr").write_bytes(source_bytes)
        if digest(owned / "root.xr") != digest(sources[0]):
            raise ValueError("private replay bytes differ from the exact fixture")
        private_roots.append(owned)
    before = inputs(binary)
    write(run / "inputs-before.json", before)
    write(run / "private-inputs.json", {"source":str(sources[0]), "sha256":digest(sources[0]),
                                      "private_roots":[str(path) for path in private_roots],
                                      "preparation":"Finite replay ledger released before each observed operation"})
    started = time.monotonic()

    def shard(index: int) -> dict:
        command = [str(binary), args.case, str(private_roots[index]), "--delete-source",
                   "--compiler-shard", str(index), str(args.jobs)]
        log = run / f"shard-{index}.log"
        shard_started = datetime.now(timezone.utc).isoformat()
        shard_clock = time.monotonic()
        with log.open("wb") as stream:
            process = subprocess.Popen(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
            try:
                code = process.wait(timeout=max(1, 570 - (time.monotonic() - started)))
                timed_out = False
            except subprocess.TimeoutExpired:
                process.kill()
                code = process.wait()
                timed_out = True
        output = log.read_text(encoding="utf-8", errors="replace")
        result = {"shard":index, "argv":command, "returncode":code, "timed_out":timed_out,
                  "pid":process.pid, "started_at":shard_started, "elapsed_seconds":time.monotonic()-shard_clock,
                  "log_sha256":digest(log), "ordinals":[int(value) for value in ORDINAL.findall(output)],
                  "summaries":SUMMARY.findall(output),
                  "source_remaining":(private_roots[index]/"root.xr").exists()}
        write(run / f"shard-{index}.json", result)
        return result

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(shard, range(args.jobs)))
    after = inputs(binary)
    write(run / "inputs-after.json", after)
    changed = sorted(name for name in before.keys() | after.keys() if before.get(name) != after.get(name))
    issues = [f"inputs changed: {changed}"] if changed else []
    total = None
    covered = []
    for result in results:
        index = result["shard"]
        if result["returncode"] or result["timed_out"] or result["source_remaining"] or len(result["summaries"]) != 1:
            issues.append(f"shard {index} failed or has no unique complete summary")
            continue
        case, count, actual, jobs, size = result["summaries"][0]
        count, actual, jobs, size = map(int, (count, actual, jobs, size))
        if total is None:
            total = count
        expected = list(range(index, count, args.jobs))
        if (case != args.case or count != total or actual != index or jobs != args.jobs or
                size != len(expected) or result["ordinals"] != expected):
            issues.append(f"shard {index} census differs from its exact ordinal range")
        covered += result["ordinals"]
    if total is None or sorted(covered) != list(range(total)):
        issues.append("complete detachment allocation denominator was not covered exactly once")
    report = {"status":"FAIL" if issues else "PASS", "case":args.case, "jobs":args.jobs,
              "sites":total, "covered":len(covered), "issues":issues, "inputs":len(before),
              "changed":changed, "elapsed_seconds":time.monotonic()-started,
              "binary":str(binary), "binary_sha256":before[str(binary)], "evidence":str(run),
              "parent_pid":os.getpid(), "finished_at":datetime.now(timezone.utc).isoformat()}
    write(run / "result.json", report)
    print(json.dumps(report, ensure_ascii=False))
    if issues:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
