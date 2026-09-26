"""Survey CGen cases in isolated processes without replacing CTest."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def diagnostic_signature(data):
    """Group first diagnostics, not presumed root causes or expected negatives."""
    lines = data.decode("utf-8", errors="replace").splitlines()
    patterns = (r"XR_[A-Z_]+_[0-9]+:.*", r"ANALYZE FAILED.*",
                r".*(?:fixture error|transition failed|ERROR:).*", r"FAIL:.*")
    for pattern in patterns:
        for line in lines:
            match = re.search(pattern, line)
            if match:
                message = match.group().strip()
                message = re.sub(r"ANALYZE FAILED \([^)]*\):", "ANALYZE FAILED:", message)
                # Row identities vary between fixtures; retain opcode/kind distinctions.
                message = re.sub(r"\b(operation|function|target)=\d+", r"\1=*", message)
                message = re.split(r" result=| receiver=", message)[0]
                return message
    return "No recognized diagnostic; inspect log"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--baseline", type=Path,
                        help="Previous summary.json for case status comparison")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32 or args.timeout <= 0:
        parser.error("jobs must be 1..32 and timeout must be positive")
    binary = args.binary.resolve(strict=True)
    baseline = json.loads(args.baseline.read_text(encoding="utf-8")) if args.baseline else None
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    binary_hash = digest(binary)
    environment = {key: value for key, value in os.environ.items()
                   if key != "XRAY_TEST_FILTER"}
    listing = subprocess.run([str(binary), "--list-cases"], capture_output=True,
                             env=environment, check=True, timeout=30)
    (output / "listing.log").write_bytes(listing.stdout + listing.stderr)
    cases = re.findall(rb"^CASE ([a-zA-Z0-9_]+)\r?$", listing.stdout, re.MULTILINE)
    names = [case.decode("ascii") for case in cases]
    if not names or len(names) != len(set(names)):
        raise RuntimeError("CGen case enumeration is empty or contains duplicates")

    def run_case(name):
        started = time.perf_counter()
        try:
            result = subprocess.run([str(binary), "--case", name], capture_output=True,
                                    env=environment, timeout=args.timeout)
            data = result.stdout + result.stderr
            exact = re.search(rb"=== 1/1 Xi CGen tests passed ===", result.stdout) is not None
            status = "PASS" if result.returncode == 0 and exact else "FAIL"
            code = result.returncode
        except subprocess.TimeoutExpired as error:
            data = (error.stdout or b"") + (error.stderr or b"")
            status, code = "TIMEOUT", None
        log = output / (name + ".log")
        log.write_bytes(data)
        return {"case": name, "status": status, "exit_code": code,
                "seconds": time.perf_counter() - started, "log": log.name,
                "diagnostic": diagnostic_signature(data) if status != "PASS" else None,
                "log_sha256": hashlib.sha256(data).hexdigest()}

    started = time.perf_counter()
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(run_case, names))
    unchanged = digest(binary) == binary_hash
    passed = sum(result["status"] == "PASS" for result in results)
    groups = {}
    for result in results:
        if result["status"] != "PASS":
            groups.setdefault(result["diagnostic"], []).append(result["case"])
    grouped = [{"diagnostic": key, "count": len(value), "cases": value}
               for key, value in sorted(groups.items(), key=lambda item: (-len(item[1]), item[0]))]
    report = {
        "scope": "Isolated CGen cases; does not qualify shared-process CTest or build profile",
        "binary": str(binary), "binary_sha256": binary_hash,
        "binary_unchanged": unchanged, "runner_sha256": digest(Path(__file__)),
        "jobs": args.jobs, "case_timeout_seconds": args.timeout,
        "wall_seconds": time.perf_counter() - started,
        "passed": passed, "failed": len(results) - passed, "cases": results,
        "diagnostic_groups": grouped,
    }
    if baseline is not None:
        previous = {row["case"]: row["status"] for row in baseline["cases"]}
        current = {row["case"]: row["status"] for row in results}
        report["comparison"] = {
            "baseline_sha256": digest(args.baseline),
            "changes": [{"case": name, "before": previous.get(name), "after": current.get(name)}
                        for name in sorted(previous.keys() | current.keys())
                        if previous.get(name) != current.get(name)],
        }
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for group in grouped:
        print(f'{group["count"]} failures: {group["diagnostic"]}', flush=True)
    print(f'{passed}/{len(results)} passed in {report["wall_seconds"]:.3f}s; {output}', flush=True)
    return 0 if passed == len(results) and unchanged else 1


if __name__ == "__main__":
    raise SystemExit(main())
