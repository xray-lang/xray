"""Measure the cost of per-element checks and publication with matched growth."""

import argparse
import importlib.util
from itertools import permutations
from pathlib import Path
from statistics import median


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    directory = root / "bench/managed-value-layout"
    spec = importlib.util.spec_from_file_location("layout_runner", directory / "run.py")
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    work = args.work.resolve()
    work.mkdir()
    evidence = runner.evidence
    env = evidence.environment(root)
    cc = args.cc.resolve(strict=True)
    result = {"state": "RUNNING", "source": runner.source_identity(), "builds": {}, "samples": []}

    def save():
        evidence.save(work / "result.json", result)

    try:
        version = evidence.execute([cc, "--version"], work, "compiler", env)
        assert version["ok"], version
        output = Path(version["streams"]["stdout"]["path"]).read_bytes()
        flags = runner.flags(cc, output)
        programs = {}
        for key, unique, length in (
                ("segment", 0, 0), ("length", 0, 1), ("unique", 1, 0), ("both", 1, 1)):
            product = work / key
            command = [cc, *flags, f"-DARRAY_COST_CHECK_UNIQUE={unique}", f"-DARRAY_COST_PUBLISH_LENGTH={length}",
                       directory / "array_append_cost.c", directory / "layout.c", "-o", product]
            check = evidence.execute(command, work, "build-" + key, env)
            assert check["ok"], check
            programs[key] = product
            result["builds"][key] = {"command": check, "product": evidence.fingerprint(product)}
            save()
        orders = list(permutations(programs))
        for repeat, order in enumerate(orders):
            for key in order:
                row = evidence.execute([programs[key]], work, f"run-{repeat}-{key}", env, b"12499997500000\n")
                row.update(variant=key, repeat=repeat)
                result["samples"].append(row)
                save()
                assert row["ok"], row
        result["medians"] = {
            key: median(row["processWallSeconds"] for row in result["samples"] if row["variant"] == key)
            for key in programs
        }
        for key, binary in programs.items():
            assert result["builds"][key]["product"] == evidence.fingerprint(binary)
        assert result["source"] == runner.source_identity()
        result["state"] = "OBSERVED"
        print(result["medians"])
    except BaseException as failure:
        result.update(state="FAILED_OBSERVATION", error=repr(failure))
        raise
    finally:
        save()


if __name__ == "__main__":
    main()
