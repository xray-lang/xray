#!/usr/bin/env python3
"""Run public source detachment against owned, disposable exact fixture copies."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests/unit/xir/product_consumers/fixtures"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--case", choices=("generics", "callables", "single_module", "text_program",
        "cross_module_coroutine", "cross_module_static_coroutine"), required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--check", choices=("normal", "axes", "baseline", "site"), default="normal")
    parser.add_argument("--ordinal", type=int)
    args = parser.parse_args()
    if (args.check == "site") != (args.ordinal is not None) or (args.ordinal is not None and args.ordinal < 0):
        parser.error("only a site check takes a nonnegative ordinal")
    binary = args.binary.resolve(strict=True)
    scratch = args.scratch.resolve()
    owned = scratch / (args.case + "-" + uuid.uuid4().hex)
    owned.mkdir(parents=True, exist_ok=False)
    if owned.resolve().parent != scratch:
        raise ValueError("private fixture directory escaped its owner")
    before = {}
    for source in sorted((FIXTURES / args.case).glob("*.xr")):
        if source.is_symlink():
            raise ValueError("fixture links are not owned source inputs")
        target = owned / source.name
        data = source.read_bytes()
        target.write_bytes(data)
        before[str(source)] = digest(source)
        if digest(target) != before[str(source)]:
            raise ValueError("private source copy differs from its exact fixture")
    if not (owned / "root.xr").is_file():
        raise ValueError("private fixture has no root source")
    argv = [str(binary), args.case, str(owned), "--delete-source"]
    if args.check != "normal":
        argv += [{"axes":"--axes", "baseline":"--compiler-baseline", "site":"--compiler-site"}[args.check]]
    if args.ordinal is not None:
        argv.append(str(args.ordinal))
    record = {"argv": argv, "binary_sha256": digest(binary), "owner_pid": os.getpid(),
              "check": args.check, "ordinal": args.ordinal,
              "fixture_preparation": "Exact private copies; finite replay ledger released before each measured product operation",
              "started_at": datetime.now(timezone.utc).isoformat(), "originals": before,
              "private_source_paths": [str(owned / Path(path).name) for path in before]}
    clock = time.monotonic()
    with (owned / "consumer.log").open("wb") as log:
        process = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)
        record["pid"] = process.pid
        (owned / "live.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        record["returncode"] = process.wait()
    record["remaining_private_sources"] = [path for path in record["private_source_paths"] if Path(path).exists()]
    record["changed_originals"] = [path for path, expected in before.items() if digest(Path(path)) != expected]
    record["binary_changed"] = digest(binary) != record["binary_sha256"]
    record.update(finished_at=datetime.now(timezone.utc).isoformat(), elapsed_seconds=time.monotonic()-clock)
    passed = not record["returncode"] and not record["remaining_private_sources"] and not record["changed_originals"] and not record["binary_changed"]
    record["status"] = "PASS" if passed else "FAIL"
    (owned / "result.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print((owned / "consumer.log").read_text(encoding="utf-8", errors="replace"), end="")
    print(json.dumps({"status": record["status"], "case": args.case, "check": args.check,
                      "sources_deleted": len(before)-len(record["remaining_private_sources"]), "evidence": str(owned)}))
    if not passed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
