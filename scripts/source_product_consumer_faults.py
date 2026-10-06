#!/usr/bin/env python3
"""Run complete compiler fault ordinals without per-ordinal child processes."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
SUMMARY = re.compile(r"compiler-summary case=(\w+) mode=(\d+) sites=(\d+) shard=(\d+) shards=(\d+) covered=(\d+) physical=0/0")
ORDINAL = re.compile(r"^compiler ordinal=(\d+) physical=0/0$", re.M)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inputs(binary: Path) -> dict[str, str]:
    paths = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT).decode().split("\0")
    paths += [p.relative_to(ROOT).as_posix() for p in (ROOT / "tests/unit/xir/product_consumers").rglob("*") if p.is_file()]
    paths += [p.relative_to(ROOT).as_posix() for p in (ROOT / "scripts").glob("source_product_consumer_*.py")]
    paths += ["tests/unit/program/source_product_consumers.cmake"]
    result = {name: digest(ROOT / name) for name in sorted(set(paths)) if name and (ROOT / name).is_file()}
    result[str(binary)] = digest(binary)
    return result


def write(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--mode", type=int, choices=range(4), required=True)
    parser.add_argument("--jobs", type=int, choices=range(1, 9), default=8)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    run = args.evidence / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:8])
    run.mkdir(parents=True, exist_ok=False)
    before = inputs(binary)
    write(run / "inputs-before.json", before)
    started = time.monotonic()

    def shard(index: int) -> dict:
        command = [str(binary), str(args.mode), "--compiler-shard", str(index), str(args.jobs)]
        log = run / f"shard-{index}.log"
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
        summaries = SUMMARY.findall(output)
        ordinals = [int(value) for value in ORDINAL.findall(output)]
        result = {"shard": index, "argv": command, "returncode": code, "timed_out": timed_out,
                  "log_sha256": digest(log), "ordinals": ordinals, "summaries": summaries}
        write(run / f"shard-{index}.json", result)
        return result

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(shard, range(args.jobs)))
    after = inputs(binary)
    write(run / "inputs-after.json", after)
    changed = sorted(name for name in before.keys() | after.keys() if before.get(name) != after.get(name))
    issues = [f"inputs changed: {changed}"] if changed else []
    total = None
    case = None
    covered = []
    for result in results:
        index = result["shard"]
        if result["returncode"] or result["timed_out"] or len(result["summaries"]) != 1:
            issues.append(f"shard {index} failed or has no unique summary")
            continue
        name, mode, count, actual, jobs, size = result["summaries"][0]
        count, actual, jobs, size = map(int, (count, actual, jobs, size))
        if total is None:
            total, case = count, name
        expected = list(range(index, count, args.jobs))
        if (name != case or int(mode) != args.mode or count != total or actual != index or
                jobs != args.jobs or size != len(expected) or result["ordinals"] != expected):
            issues.append(f"shard {index} census differs from its exact ordinal range")
        covered += result["ordinals"]
    if total is None or sorted(covered) != list(range(total)):
        issues.append("complete allocation denominator was not covered exactly once")
    report = {"status": "FAIL" if issues else "PASS", "case": case, "mode": args.mode,
              "jobs": args.jobs, "sites": total, "covered": len(covered), "issues": issues,
              "inputs": len(before), "changed": changed, "elapsed_seconds": time.monotonic() - started,
              "binary": str(binary), "binary_sha256": before[str(binary)], "evidence": str(run)}
    write(run / "result.json", report)
    print(json.dumps(report, ensure_ascii=False))
    if issues:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
