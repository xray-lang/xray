#!/usr/bin/env python3
"""Measure equivalent-spelling performance families against hand-written C.

check --root .
run --root . --build build --cc /absolute/compiler --work NEW [--family NAME] [--blocking]

A family is a set of member programs that do the same work with different
spellings. Every member expands at a native and a VM scale and must print the
family's declared output on both engines. Native members, the family's
hand-written C reference and the VM members are timed in five forward/reverse
rounds through array_access_benchmark's process observer and product timeout.

The report judges, per family: slowest / fastest native member and slowest /
fastest VM member against the spread limit; the fastest native member against
the family's own C limit; the slowest native member against the generic C limit.
A threshold miss is recorded as FAIL in result.json and summary.md. It changes
the exit status only with --blocking; a failed or invalid sample always does.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import sys

import array_access_benchmark as bench
import xray_process

FAMILIES = Path("bench/perf-families")
EXE = ".exe" if os.name == "nt" else ""
RULES = {"seed", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8"}
ENGINES = ("native", "vm")
PLACEHOLDER = re.compile(r"@[A-Z]+@")


def expand(text: str, scale: dict) -> str:
    for key, value in scale.items():
        text = text.replace(key, value)
    return text


def validate(root: Path, manifest: dict) -> list:
    """Return every structural problem instead of stopping at the first one."""
    problems = []
    limits = manifest.get("limits", {})
    for key in ("familySpread", "genericOverC"):
        if not isinstance(limits.get(key), (int, float)) or limits[key] < 1:
            problems.append(f"limits.{key} must be a number >= 1")
    families = manifest.get("families")
    if not isinstance(families, dict) or not families:
        return problems + ["at least one family is required"]
    for name, family in families.items():
        folder = root / FAMILIES / name
        members = family.get("members", {})
        scales = family.get("scales", {})
        stdout = family.get("stdout", {})
        if members.get("seed") != "seed":
            problems.append(f"{name}: the member 'seed' with rule 'seed' is required")
        if not isinstance(family.get("nativeOverC"), (int, float)) or family["nativeOverC"] < 1:
            problems.append(f"{name}: nativeOverC must be a number >= 1")
        if not family.get("limitSource"):
            problems.append(f"{name}: limitSource is required")
        for engine in ENGINES:
            if not isinstance(scales.get(engine), dict) or not scales[engine]:
                problems.append(f"{name}: scales.{engine} is required")
            if not str(stdout.get(engine, "")).endswith("\n"):
                problems.append(f"{name}: stdout.{engine} must be a complete line")
        reference = folder / family.get("c", "")
        if not family.get("c") or not reference.is_file():
            problems.append(f"{name}: missing hand-written C reference {family.get('c')}")
        declared = set(members)
        on_disk = {path.stem for path in folder.glob("*.xr")}
        for extra in sorted(on_disk - declared):
            problems.append(f"{name}: {extra}.xr is not declared as a member")
        for member, rule in members.items():
            if rule not in RULES:
                problems.append(f"{name}.{member}: unknown rule {rule}")
            source = folder / (member + ".xr")
            if not source.is_file():
                problems.append(f"{name}.{member}: missing {source.name}")
                continue
            text = source.read_text(encoding="utf-8")
            if not PLACEHOLDER.search(text):
                problems.append(f"{name}.{member}: no scale placeholder; native and VM would run the same size")
            for engine in ENGINES:
                left = PLACEHOLDER.findall(expand(text, scales.get(engine, {})))
                if left:
                    problems.append(f"{name}.{member}: {engine} scale leaves {sorted(set(left))}")
        origin = family.get("seedOrigin")
        if origin and (folder / "seed.xr").is_file():
            seed = expand((folder / "seed.xr").read_text(encoding="utf-8"), scales.get("native", {}))
            target = folder / origin
            if not target.is_file() or target.read_text(encoding="utf-8") != seed:
                problems.append(f"{name}: native seed differs from {origin}")
    return problems


def load(root: Path) -> dict:
    manifest = json.loads((root / FAMILIES / "manifest.json").read_text(encoding="utf-8"))
    problems = validate(root, manifest)
    if problems:
        raise ValueError("Invalid performance family manifest:\n  " + "\n  ".join(problems))
    return manifest


def check(args) -> int:
    manifest = load(args.root.resolve())
    for name, family in manifest["families"].items():
        print(f"{name}: {len(family['members'])} members, C limit {family['nativeOverC']}")
    return 0


def judge(name: str, family: dict, limits: dict, rows: dict, samples: list) -> dict:
    medians = {}
    for key in rows:
        values = [row["processWallSeconds"] for row in samples
                  if row["family"] == name and row["entry"] == key and row["ok"]]
        medians[key] = statistics.median(values) if len(values) == bench.ROUNDS else None
    native = {key: medians[key] for key, row in rows.items() if row["engine"] == "native"}
    vm = {row["member"]: medians[key] for key, row in rows.items() if row["engine"] == "VM"}
    report = {"nativeOverC": family["nativeOverC"], "limitSource": family["limitSource"],
              "cSeconds": medians["C"], "members": {}}
    if medians["C"] is None or None in native.values() or None in vm.values():
        report.update(state="FAIL", reason="At least one entry has no valid five-sample median")
        return report
    fastest, slowest = min(native.values()), max(native.values())
    vm_fastest, vm_slowest = min(vm.values()), max(vm.values())
    checks = {
        "nativeSpread": {"value": slowest / fastest, "limit": limits["familySpread"]},
        "vmSpread": {"value": vm_slowest / vm_fastest, "limit": limits["familySpread"]},
        "fastestOverC": {"value": fastest / medians["C"], "limit": family["nativeOverC"]},
        "slowestOverC": {"value": slowest / medians["C"], "limit": limits["genericOverC"]},
    }
    for item in checks.values():
        item["pass"] = item["value"] <= item["limit"]
    for member in native:
        report["members"][member] = {
            "rule": family["members"][member], "nativeSeconds": native[member],
            "nativeOverFastest": native[member] / fastest, "nativeOverC": native[member] / medians["C"],
            "vmSeconds": vm[member], "vmOverFastest": vm[member] / vm_fastest}
    report.update(checks=checks, state="PASS" if all(item["pass"] for item in checks.values()) else "FAIL")
    return report


def summarize(result: dict) -> str:
    lines = [f"## Performance families: {result['state']}"
             + ("" if result.get("blocking") else " (report only)"), ""]
    provider = result.get("provider", {})
    lines.append(f"Platform `{result['platform']}`, C provider {provider.get('family', '?')}, "
                 f"commit `{result.get('identity', {}).get('commit', '?')[:12]}`.")
    for name, family in result.get("families", {}).items():
        lines += ["", f"### {name}: {family['state']}", ""]
        if "checks" not in family:
            lines.append(family.get("reason", "No judgement."))
            continue
        lines.append(" | ".join(f"{key} {item['value']:.2f} (limit {item['limit']})"
                                + ("" if item["pass"] else " FAIL") for key, item in family["checks"].items()))
        lines += ["", "| member | rule | native ms | x fastest | x C | VM ms | x fastest |", "|---|---|---|---|---|---|---|"]
        for member, row in family["members"].items():
            lines.append(f"| {member} | {row['rule']} | {row['nativeSeconds'] * 1000:.1f} | {row['nativeOverFastest']:.2f} | "
                         f"{row['nativeOverC']:.2f} | {row['vmSeconds'] * 1000:.1f} | {row['vmOverFastest']:.2f} |")
        lines.append(f"| C reference | | {family['cSeconds'] * 1000:.1f} | | 1.00 | | |")
    return "\n".join(lines) + "\n"


def run(args) -> int:
    root, build = args.root.resolve(), args.build.resolve()
    manifest = load(root)
    selected = args.family or list(manifest["families"])
    unknown = sorted(set(selected) - set(manifest["families"]))
    if unknown or len(selected) != len(set(selected)):
        raise ValueError("Unknown or repeated family: " + ", ".join(unknown or selected))
    args.work.mkdir(parents=True)
    work = args.work.resolve()
    env = bench.environment(root)
    binary = build / ("xray" + EXE)
    compiler = args.cc.resolve(strict=True)
    result = {"schema": 1, "state": "PREPARING", "platform": sys.platform, "blocking": args.blocking,
              "driver": bench.fingerprint(Path(__file__)), "limits": manifest["limits"],
              "rounds": bench.ROUNDS, "timeoutSeconds": bench.PRODUCT_TIMEOUT,
              "timingMethod": bench.TIMING_METHOD, "commands": [], "samples": [], "families": {}}

    def prepare(name: str, argv: list) -> bytes:
        row = bench.execute(argv, work, name, env)
        row["name"] = name
        result["commands"].append(row)
        bench.save(work / "result.json", result)
        if not row["ok"]:
            raise RuntimeError("Preparation failed: " + name)
        return b"".join(Path(row["streams"][key]["path"]).read_bytes() for key in ("stdout", "stderr"))

    complete = False
    try:
        head = bench.clean_head(root)
        identity = json.loads(prepare("identity", [binary, "--version", "--json"]))
        if identity["commit"] != head or identity["dirty"] or identity["buildProfile"] != "Release":
            raise ValueError("CLI must identify the exact clean Release source commit")
        result["identity"] = identity
        frozen = work / binary.name
        shutil.copy2(binary, frozen)
        header = (build / "generated/xray_native_link_paths.h").read_text(encoding="utf-8")
        if "#define XRT_NATIVE_ASAN 0" not in header or "#define XRT_NATIVE_UBSAN 0" not in header:
            raise ValueError("Performance products must use uninstrumented runtime libraries")
        version = prepare("compiler-version",
                          [compiler, "/?"] if compiler.name.lower() == "cl.exe" else [compiler, "--version"])
        flags, provider = bench.compiler_flags(compiler, header, version)
        result["provider"] = {"compiler": bench.fingerprint(compiler), "family": provider, "flags": flags}
        entries = {}
        for name in selected:
            family = manifest["families"][name]
            folder, out = root / FAMILIES / name, work / name
            out.mkdir()
            native_text = family["stdout"]["native"].encode()
            c_text = native_text.replace(b"\n", b"\r\n") if os.name == "nt" else native_text
            reference, c_exe = (folder / family["c"]).resolve(), out / ("C" + EXE)
            if os.name == "nt":
                obj = out / "C.obj"
                prepare(f"{name}-C-compile", [compiler, *flags, "/c", reference, "/Fo" + str(obj)])
                prepare(f"{name}-C-link", [compiler, "/nologo", flags[6], obj, "/Fe" + str(c_exe)])
            else:
                obj = out / "C.o"
                prepare(f"{name}-C-compile", [compiler, *flags, "-c", reference, "-o", obj])
                prepare(f"{name}-C-link", [compiler, obj, "-o", c_exe])
            rows = {"C": {"engine": "C", "argv": [str(c_exe)], "expectedHex": c_text.hex()}}
            for member, rule in family["members"].items():
                text = (folder / (member + ".xr")).read_text(encoding="utf-8")
                for engine in ENGINES:
                    (out / f"{member}.{engine}.xr").write_bytes(expand(text, family["scales"][engine]).encode())
                executable = out / (member + EXE)
                verbose = prepare(f"{name}-{member}-build",
                                  [binary, "build", "-v", "--cc", compiler, out / f"{member}.native.xr", "-o", executable])
                choices = [line[len("C compiler "):].strip() for line in verbose.decode(errors="replace").splitlines()
                           if line.startswith("C compiler ")]
                if len(choices) != 1 or not os.path.samefile(choices[0], compiler):
                    raise ValueError("CLI did not report the requested native compiler for " + member)
                rows[member] = {"engine": "native", "member": member, "rule": rule,
                                "argv": [str(executable)], "expectedHex": native_text.hex()}
                rows[member + "@vm"] = {"engine": "VM", "member": member, "rule": rule,
                                        "argv": [str(frozen), "run", str(out / f"{member}.vm.xr")],
                                        "expectedHex": family["stdout"]["vm"].encode().hex()}
            entries[name] = rows
        result["entries"] = entries
        result["state"] = "MEASURING"
        for name, rows in entries.items():
            keys = list(rows)
            for round_index in range(bench.ROUNDS):
                for key in keys if round_index % 2 == 0 else list(reversed(keys)):
                    row = bench.execute(rows[key]["argv"], work, f"{name}-{key.replace('@', '-')}-{round_index}",
                                        env, bytes.fromhex(rows[key]["expectedHex"]))
                    row.update(family=name, entry=key, round=round_index)
                    result["samples"].append(row)
                    bench.save(work / "result.json", result)
                    if row["state"] == "NEEDS_RECOVERY":
                        raise xray_process.RecoveryRequired(row.get("error", "sample needs recovery"))
        if bench.clean_head(root) != head:
            raise ValueError("Source commit changed during measurement")
        complete = all(row["ok"] for row in result["samples"])
        for name, rows in entries.items():
            result["families"][name] = judge(name, manifest["families"][name], manifest["limits"],
                                             rows, result["samples"])
        passed = complete and all(family["state"] == "PASS" for family in result["families"].values())
        result["state"] = "PASS" if passed else "FAIL"
    except BaseException as error:
        result.update(state="FAIL", error=repr(error))
        raise
    finally:
        bench.save(work / "result.json", result)
        summary = summarize(result)
        (work / "summary.md").write_text(summary, encoding="utf-8")
        if os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as output:
                output.write(summary)
        print(summary)
    if not complete:
        print("At least one sample failed or was invalid; inspect the retained raw files.", file=sys.stderr)
        return 1
    return 1 if args.blocking and result["state"] != "PASS" else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    checker = commands.add_parser("check")
    checker.add_argument("--root", type=Path, required=True)
    runner = commands.add_parser("run")
    runner.add_argument("--root", type=Path, required=True)
    runner.add_argument("--build", type=Path, required=True)
    runner.add_argument("--cc", type=Path, required=True)
    runner.add_argument("--work", type=Path, required=True)
    runner.add_argument("--family", action="append")
    runner.add_argument("--blocking", action="store_true")
    args = parser.parse_args()
    return check(args) if args.command == "check" else run(args)


if __name__ == "__main__":
    sys.exit(main())
