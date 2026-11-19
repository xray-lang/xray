#!/usr/bin/env python3
"""Replay archived Array products under one serial macOS runner."""

from itertools import permutations
from pathlib import Path
import os
import platform
import statistics
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from array_access_benchmark import digest, environment, execute, save


ARCHIVES = {
    "snapshot": (44484191076, "a2d0a2db97d81dedf167567d8ec6354e63e7adb6",
                 "4279b36511bd7b0d9ee06c9660baf1f9e6f7157a7bad97af1f26800a4361117a"),
    "display": (44485868575, "0a7645d9fb9aa563c7c6966abc2dae340f986d21",
                "26f1c2014bf8daa3cd1019f248f938883444eaf659930d1b266665fef1910885"),
}
C_DIGEST = "572367b646720264bae97b7068f37418e500dc53ea9620ed1ca248865a2f506b"
EXPECTED = b"-742713184288245760\n"


def replay(inputs: Path, output: Path) -> None:
    output.mkdir()
    result_path = output / "result.json"
    report = {"state": "RUNNING", "host": platform.uname()._asdict(),
              "workflowRun": os.environ.get("GITHUB_RUN_ID"), "inputs": ARCHIVES,
              "referenceSha256": C_DIGEST, "samples": [], "warmup": [],
              "scope": "Exact binary replay only. No new product build or qualification."}
    try:
        if (platform.system(), platform.machine()) != ("Darwin", "arm64"):
            raise RuntimeError("The archived native products require macOS arm64")
        prepare = Path("build/array-performance-prepare")
        binaries = {name: inputs / name / prepare / "inplace-native" for name in ARCHIVES}
        references = [inputs / name / prepare / "inplace-C" for name in ARCHIVES]
        fingerprints = {binaries[name]: row[2] for name, row in ARCHIVES.items()}
        fingerprints.update({path: C_DIGEST for path in references})
        for path, sha in fingerprints.items():
            if digest(path) != sha:
                raise RuntimeError(f"Unexpected archived binary hash for {path}")
            path.chmod(path.stat().st_mode | 0o100)
        binaries["C"] = references[0]
        env = environment(Path.cwd())
        for name, binary in binaries.items():
            report["warmup"].append(execute([binary], output, f"warmup-{name}", env, EXPECTED))
        if not all(row["ok"] for row in report["warmup"]):
            raise RuntimeError("Warmup exit status or output differs from the original benchmark")
        for index, order in enumerate(list(permutations(binaries)) * 3):
            row = {"order": order, "engines": {}}
            report["samples"].append(row)
            for name in order:
                run = execute([binaries[name]], output, f"{index:02d}-{name}", env, EXPECTED)
                row["engines"][name] = run
                if not run["ok"]:
                    raise RuntimeError(f"Invalid observation in round {index} for {name}")
        for path, sha in fingerprints.items():
            if digest(path) != sha:
                raise RuntimeError(f"Archived binary changed during replay at {path}")
        medians = {name: statistics.median(row["engines"][name]["processWallSeconds"]
                                           for row in report["samples"]) for name in binaries}
        report.update(state="OBSERVED", medianSeconds=medians,
                      snapshotOverC=medians["snapshot"] / medians["C"],
                      displayOverC=medians["display"] / medians["C"],
                      displayOverSnapshot=medians["display"] / medians["snapshot"])
        print(f"display/snapshot {report['displayOverSnapshot']:.4f}")
    except BaseException as error:
        report.update(state="FAILED_OBSERVATION", error=str(error))
        raise
    finally:
        save(result_path, report)


if __name__ == "__main__":
    replay(Path(sys.argv[1]).resolve(strict=True), Path(sys.argv[2]).resolve())
