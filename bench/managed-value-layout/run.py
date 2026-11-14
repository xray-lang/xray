#!/usr/bin/env python3
"""Prepare and measure the standalone native layout experiment, never the VM."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import array_access_benchmark as evidence
import xray_process

CASES = {"push": "12499997500000", "record": "9000000000000", "map": "19999900000",
         "push-prepared": "12499997500000", "push-direct": "12499997500000",
         "map-take": "19999900000", "map-typed-drop": "19999900000"}
RAW_CASES = {"push-prepared": "push", "push-direct": "push", "map-take": "map", "map-typed-drop": "map"}
RATIO_CASES = ("push", "map", "push-direct", "map-take", "map-typed-drop")
ROUNDS = 5


def source_identity() -> dict:
    def git(*args):
        return subprocess.check_output(["git", "-C", ROOT, *args], text=True).strip()
    identity = {"head": git("rev-parse", "HEAD"), "status": git("status", "--porcelain")}
    if identity["status"]:
        raise ValueError("The prototype source must be committed and clean")
    return identity


def flags(compiler: Path, version: bytes) -> list[str]:
    if os.name == "nt":
        name = compiler.name.lower()
        if name not in ("cl.exe", "clang-cl.exe"):
            raise ValueError("Windows requires an explicit MSVC or clang-cl executable")
        common = ["/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/O2", "/DNDEBUG",
                  "/D_CRT_SECURE_NO_WARNINGS", "/MT", "/I" + str(ROOT / "src")]
        return common + (["/fp:precise", "/wd4200", "/experimental:c11atomics"] if name == "cl.exe" else
                         ["/clang:-ffp-contract=off", "/clang:-falign-functions=16"])
    if sys.platform not in ("linux", "darwin"):
        raise ValueError("Unsupported prototype host: " + sys.platform)
    text = version.decode("utf-8", errors="replace").lower()
    if not any(family in text for family in ("clang", "gcc", "free software foundation")):
        raise ValueError("The Unix provider must identify as Clang or GCC")
    feature = "-D_DARWIN_C_SOURCE" if sys.platform == "darwin" else "-D_POSIX_C_SOURCE=200809L"
    return ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-O2", "-DNDEBUG",
            "-ffp-contract=off", feature, "-I", str(ROOT / "src")]


def prepare(args) -> None:
    initial = source_identity()
    args.work.mkdir(parents=True)
    result = {"prototypeSchema": 2, "state": "PREPARING", "source": initial, "root": str(ROOT), "platform": sys.platform,
              "commands": [], "prototypeOnly": True, "productQualification": "NOT_RUN"}
    env = evidence.environment(ROOT)

    def run(name, argv, expected=None):
        row = evidence.execute(argv, args.work, name, env, expected)
        result["commands"].append(row)
        evidence.save(args.work / "preparation.json", result)
        if row["state"] == "NEEDS_RECOVERY":
            raise xray_process.RecoveryRequired(row["error"])
        if not row["ok"]:
            raise RuntimeError("Prototype preparation failed: " + name)
        return b"".join(Path(row["streams"][stream]["path"]).read_bytes()
                        for stream in ("stdout", "stderr"))

    try:
        compiler = args.cc.resolve(strict=True)
        version = run("compiler-version", [compiler, "/?"] if compiler.name.lower() == "cl.exe"
                      else [compiler, "--version"])
        options = flags(compiler, version)
        result["provider"] = {"compiler": evidence.fingerprint(compiler), "flags": options,
                              "lto": False, "layout": "one declared build per provider"}
        directory = ROOT / "bench/managed-value-layout"
        products = {}
        for name, sources in (("managed", ["main.c", "layout.c", "verify.c", "escape.c"]),
                              ("raw", ["raw.c", "raw_keys.c"])):
            output = args.work / (name + (".exe" if os.name == "nt" else ""))
            argv = [compiler, *options, *(directory / source for source in sources)]
            argv += ["/Fe" + str(output)] if os.name == "nt" else ["-o", output]
            run("compile-" + name, argv)
            run("layout-" + name, [output, "layout"])
            products[name] = str(output)
        newline = "\r\n" if os.name == "nt" else "\n"
        run("verify", [products["managed"], "verify"], ("verified" + newline).encode())
        # This checks actual escaped ownership; it does not replace the original
        # record input or establish a timing ratio against an unmatched control.
        run("record-escape-control", [products["managed"], "record-escape"],
            (CASES["record"] + newline).encode())
        dependencies = list(directory.glob("*.c")) + list(directory.glob("*.h"))
        dependencies += [Path(__file__), Path(evidence.__file__), Path(xray_process.__file__), compiler]
        dependencies += [ROOT / "src/base" / name for name in
                         ("xhash.h", "xmalloc.h", "xdefs.h", "xplatform.h", "xposix_compat.h",
                          "xsigned_rc.h")]
        dependencies += [path for path in args.work.rglob("*") if path.is_file()
                         and path.name != "preparation.json"]
        if source_identity() != initial:
            raise ValueError("Source changed during preparation")
        result.update(state="PREPARED_NOT_MEASURED", rounds=ROUNDS, timeoutSeconds=30,
                      timingMethod=evidence.TIMING_METHOD,
                      rawCases=RAW_CASES, ratioCases=list(RATIO_CASES),
                      products=products, expected={name: (value + newline).encode().hex()
                                                   for name, value in CASES.items()},
                      fingerprints=[evidence.fingerprint(path) for path in sorted(set(dependencies))])
        evidence.save(args.work / "preparation.json", result)
        evidence.save(args.work / "prepared.json", result)
    except BaseException as error:
        result.update(state="NEEDS_RECOVERY" if isinstance(error, xray_process.RecoveryRequired)
                      else "FAIL", error=repr(error))
        evidence.save(args.work / "preparation.json", result)
        raise


def measure(args) -> int:
    prepared = json.loads(args.prepared.read_text(encoding="utf-8"))
    newline = "\r\n" if os.name == "nt" else "\n"
    expected = {name: (value + newline).encode().hex() for name, value in CASES.items()}
    if (prepared.get("prototypeSchema") != 2 or prepared["state"] != "PREPARED_NOT_MEASURED"
            or prepared["source"] != source_identity()
            or prepared["platform"] != sys.platform or prepared["rounds"] != ROUNDS
            or prepared["timeoutSeconds"] != 30 or set(prepared["products"]) != {"managed", "raw"}
            or prepared.get("timingMethod") != evidence.TIMING_METHOD
            or prepared["expected"] != expected or prepared.get("rawCases") != RAW_CASES
            or prepared.get("ratioCases") != list(RATIO_CASES)):
        raise ValueError("The prepared prototype identity or fixed workload schema differs")
    evidence.verify_fingerprints(prepared["fingerprints"])
    frozen = {str(Path(row["path"]).resolve()) for row in prepared["fingerprints"]}
    if any(str(Path(path).resolve()) not in frozen for path in prepared["products"].values()):
        raise ValueError("A measured executable is absent from the frozen inputs")
    args.work.mkdir(parents=True)
    result = {"state": "RUNNING", "prepared": evidence.fingerprint(args.prepared), "samples": [],
              "timingMethod": evidence.TIMING_METHOD,
              "medians": {}, "ratios": {}, "prototypeOnly": True, "productQualification": "NOT_RUN",
              "boundary": "Native local RC layout only. No VM, shared RC, Root/unwind, Source or full "
                          "sanitizer qualification. Record may fold and has no heap-allocation ratio gate. "
                          "One build per provider is not formal multi-layout qualification."}
    try:
        for case in CASES:
            for round_index in range(ROUNDS):
                order = ("managed", "raw") if round_index % 2 == 0 else ("raw", "managed")
                for engine in order:
                    mode = RAW_CASES.get(case, case) if engine == "raw" else case
                    row = evidence.execute([prepared["products"][engine], mode], args.work,
                                           f"{case}-{round_index}-{engine}", evidence.environment(ROOT),
                                           bytes.fromhex(prepared["expected"][case]))
                    row.update(case=case, engine=engine, round=round_index)
                    result["samples"].append(row)
                    evidence.save(args.work / "result.json", result)
                    if row["state"] == "NEEDS_RECOVERY":
                        raise xray_process.RecoveryRequired(row["error"])
        complete = len(result["samples"]) == len(CASES) * ROUNDS * 2
        complete &= all(row["ok"] for row in result["samples"])
        evidence.verify_fingerprints(prepared["fingerprints"])
        complete &= prepared["source"] == source_identity()
        for case in CASES:
            result["medians"][case] = {}
            for engine in ("managed", "raw"):
                rows = [row for row in result["samples"] if (row["case"], row["engine"]) == (case, engine)]
                if len(rows) == ROUNDS and all(row["ok"] for row in rows):
                    values = [row["processWallSeconds"] for row in rows]
                    result["medians"][case][engine] = {"median": statistics.median(values),
                                                       "min": min(values), "max": max(values), "all": values}
            if set(result["medians"][case]) == {"managed", "raw"}:
                result["ratios"][case] = (result["medians"][case]["managed"]["median"] /
                                          result["medians"][case]["raw"]["median"])
        result["state"] = "PASS_EXECUTION_ONLY" if complete else "FAIL"
        result["layoutGates"] = {case: {"ratio": result["ratios"].get(case), "limit": 1.3,
                                       "pass": complete and result["ratios"].get(case, float("inf")) <= 1.3}
                                 for case in RATIO_CASES}
        if args.gate and complete and not all(gate["pass"] for gate in result["layoutGates"].values()):
            result["state"] = "FAIL_LAYOUT_RATIO"
        return 0 if result["state"] == "PASS_EXECUTION_ONLY" else 1
    except BaseException as error:
        result.update(state="NEEDS_RECOVERY" if isinstance(error, xray_process.RecoveryRequired)
                      else "FAIL", error=repr(error))
        raise
    finally:
        evidence.save(args.work / "result.json", result)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare_parser = commands.add_parser("prepare")
    prepare_parser.add_argument("--cc", type=Path, required=True)
    prepare_parser.add_argument("--work", type=Path, required=True)
    measure_parser = commands.add_parser("measure")
    measure_parser.add_argument("--prepared", type=Path, required=True)
    measure_parser.add_argument("--work", type=Path, required=True)
    measure_parser.add_argument("--gate", action="store_true")
    args = parser.parse_args()
    for name, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, name, value.resolve())
    if args.command == "prepare":
        prepare(args)
        return 0
    return measure(args)


if __name__ == "__main__":
    raise SystemExit(main())
