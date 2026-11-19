"""Compare two normally built standalone Array variants on a single host."""

import argparse
import importlib.util
import json
from pathlib import Path, PurePosixPath
from statistics import median


def effective_provider(prepared):
    provider = prepared["provider"].copy()
    source_include = str(PurePosixPath(prepared["root"]) / "src")
    assert provider["flags"].count(source_include) == 1, provider["flags"]
    provider["flags"] = ["<source>/src" if flag == source_include else flag for flag in provider["flags"]]
    return provider


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--parent", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    cases = ("push-direct", "push", "push-noalias")
    parser.add_argument("--case", choices=cases, action="append")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    spec = importlib.util.spec_from_file_location("layout", root / "bench/managed-value-layout/run.py")
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    parent, candidate = (json.loads(p.read_bytes()) for p in (args.parent, args.candidate))
    assert parent["source"]["head"] != candidate["source"]["head"]
    assert effective_provider(parent) == effective_provider(candidate)
    for key in ("platform", "expected", "rawCases", "ratioCases"):
        assert parent[key] == candidate[key], key
    for variant in (parent, candidate):
        assert variant["state"] == "PREPARED_NOT_MEASURED"
        assert variant["timeoutSeconds"] == 30
        runner.evidence.verify_fingerprints(variant["fingerprints"])
    args.work.mkdir()
    receipt = args.work / "result.json"
    result = {
        "state": "RUNNING",
        "prototypeOnly": True,
        "parent": parent["source"],
        "candidate": candidate["source"],
        "provider": candidate["provider"],
        "timeoutSeconds": candidate["timeoutSeconds"],
        "prepared": [runner.evidence.fingerprint(path) for path in (args.parent, args.candidate)],
        "cases": {},
    }

    def save():
        runner.evidence.save(receipt, result)

    programs = {
        "parent": parent["products"]["managed"],
        "candidate": candidate["products"]["managed"],
        "raw": parent["products"]["raw"],
    }
    env = runner.evidence.environment(root)
    names = tuple(dict.fromkeys(args.case or cases))
    try:
        for label in names:
            result["cases"][label] = {"samples": []}
        for turn in range(12):
            for name in names if turn % 2 == 0 else reversed(names):
                order = list(programs)
                offset = (turn // 2) % len(order)
                order = order[offset:] + order[:offset]
                expected = bytes.fromhex(candidate["expected"][name])
                samples = result["cases"][name]["samples"]
                for engine in order if turn % 2 == 0 else reversed(order):
                    mode = candidate["rawCases"].get(name, name) if engine == "raw" else name
                    row = runner.evidence.execute(
                        [programs[engine], mode], args.work, f"{name}-{turn}-{engine}", env, expected,
                        timeout=result["timeoutSeconds"])
                    row.update(engine=engine, round=turn)
                    samples.append(row)
                    save()
                    if not row["ok"] or Path(row["streams"]["stderr"]["path"]).stat().st_size:
                        raise RuntimeError(f"{name}: {engine} exited or produced different output")
        for name, case in result["cases"].items():
            medians = {
                engine: median(row["processWallSeconds"] for row in case["samples"] if row["engine"] == engine)
                for engine in programs
            }
            case["medians"] = medians
            case["candidateToRaw"] = medians["candidate"] / medians["raw"]
            case["candidateToParent"] = medians["candidate"] / medians["parent"]
            case["layoutGate"] = {"limit": 1.3, "pass": case["candidateToRaw"] <= 1.3}
            print(name, f"before {medians['parent']:.6f}s after {medians['candidate']:.6f}s raw {medians['raw']:.6f}s")
        for variant in (parent, candidate):
            runner.evidence.verify_fingerprints(variant["fingerprints"])
        result["state"] = "OBSERVED"
    except BaseException as error:
        result.update(state="FAILED_OBSERVATION", error=repr(error))
        raise
    finally:
        save()


if __name__ == "__main__":
    main()
