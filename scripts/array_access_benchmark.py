#!/usr/bin/env python3
"""Prepare, measure and assert fixed Array workloads without mixing the stages.

prepare --root . --build build --cc /absolute/compiler --work NEW [--case push]
measure --prepared NEW/prepared.json --work ANOTHER_NEW
assert --prepared NEW/prepared.json --result ANOTHER_NEW/result.json --scope native
self-test --work NEW

Preparation performs real CLI C generation and native compilation, but executes
no workload. Measurement uses five forward/reverse rounds and a fixed 30-second
product timeout. Run it under the caller's exclusive performance resource lease.
Optional --lua/--python require matching --lua-receipt/--python-receipt paths;
the Python running this tool is never selected as a reference automatically.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import time
from types import SimpleNamespace

import xray_process

PRODUCT_TIMEOUT = 30
ROUNDS = 5
NATIVE_LIMITS = {"push": 1.3, "inplace": 1.1}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path: Path, value: dict) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def environment(root: Path) -> dict:
    result = dict(os.environ, NO_COLOR="1", VSLANG="1033", XRAY_STDLIB_PATH=str(root / "stdlib"))
    for key in list(result):
        if key in ("CL", "_CL_", "LINK", "_LINK_", "CFLAGS", "CXXFLAGS", "LDFLAGS") or key.startswith("LUA_INIT"):
            result.pop(key)
    return result


def execute(argv: list, directory: Path, name: str, env: dict, expected: bytes | None = None,
            *, timeout: float = PRODUCT_TIMEOUT) -> dict:
    """Observe the existing process owner; never replace its cleanup or drain."""
    captured = []

    class ObservedPopen(subprocess.Popen):
        def __init__(self, *args, **kwargs):
            self.observation = {"started": time.perf_counter(), "parentPID": os.getpid()}
            super().__init__(*args, **kwargs)
            self.observation["pid"] = self.pid
            captured.append(self.observation)

        def wait(self, *args, **kwargs):
            code = super().wait(*args, **kwargs)
            if "finished" not in self.observation:
                self.observation.update(finished=time.perf_counter(), exit=code)
            return code

    argv = list(map(str, argv))
    paths = {key: directory / (name + "." + key) for key in ("stdout", "stderr")}
    row = {"argv": argv, "runnerPID": os.getpid(), "runnerParentPID": os.getppid(),
           "timeoutSeconds": timeout, "state": "RUNNING", "processes": captured}
    previous = xray_process.subprocess
    xray_process.subprocess = SimpleNamespace(**(vars(subprocess) | {"Popen": ObservedPopen}))
    failure = None
    code = None
    try:
        with paths["stdout"].open("xb") as out, paths["stderr"].open("xb") as err:
            start = time.perf_counter()
            try:
                code = xray_process.run_child(argv, cwd=directory, env=env, stdin=subprocess.DEVNULL,
                                             stdout=out, stderr=err, timeout=timeout)
            except BaseException as error:
                failure = error
            row["ownedWallSeconds"] = time.perf_counter() - start
    finally:
        xray_process.subprocess = previous
    row.update(exit=code, timedOut=isinstance(failure, subprocess.TimeoutExpired),
               state="NEEDS_RECOVERY" if isinstance(failure, xray_process.RecoveryRequired) else "TERMINAL_DRAINED",
               streams={key: {"path": str(path), "sha256": digest(path)} for key, path in paths.items()})
    if captured and "finished" in captured[0]:
        row["processWallSeconds"] = captured[0]["finished"] - captured[0]["started"]
        row["exitAfterCleanup"] = captured[0]["exit"]
    if failure:
        row["error"] = repr(failure)
    row["ok"] = failure is None and code == 0 and (expected is None or
        (paths["stdout"].read_bytes() == expected and not paths["stderr"].read_bytes()))
    return row


def fingerprint(path: Path) -> dict:
    return {"path": str(path.resolve()), "sha256": digest(path)}


def verify_fingerprints(rows: list) -> None:
    for row in rows:
        if digest(Path(row["path"])) != row["sha256"]:
            raise ValueError("Changed prepared input or product: " + row["path"])


def clean_head(root: Path) -> str:
    if subprocess.check_output(["git", "-C", root, "status", "--porcelain=v1"], text=True).strip():
        raise ValueError("Benchmark source checkout is dirty")
    return subprocess.check_output(["git", "-C", root, "rev-parse", "HEAD"], text=True).strip()


def verify_prepared(data: dict) -> None:
    required = ("identity", "driver", "root", "stdlib", "references", "thresholds", "open", "fingerprints", "frozenTrees")
    if (data.get("schema") != 1 or data.get("state") != "PREPARED_NOT_MEASURED" or
            data.get("rounds") != ROUNDS or data.get("timeoutSeconds") != PRODUCT_TIMEOUT or
            data.get("scope") not in ("native", "array") or any(key not in data for key in required)):
        raise ValueError("Incomplete or unsupported prepared benchmark schema")
    if not data.get("engines"):
        raise ValueError("No benchmark cases were declared")
    for case, engines in data["engines"].items():
        if (case not in NATIVE_LIMITS or not {"native", "C"}.issubset(engines) or
                not set(engines).issubset({"native", "C", "VM", "lua", "python"}) or
                (data["scope"] == "array" and "VM" not in engines) or
                (case == "push" and not set(data["references"]).issubset(engines)) or
                data["thresholds"][case]["nativeOverC"] != NATIVE_LIMITS[case] or
                not data["thresholds"][case]["source"]):
            raise ValueError("Missing engine or changed fixed native limit")
        for command in engines.values():
            if not command.get("argv") or not all(isinstance(arg, str) for arg in command["argv"]):
                raise ValueError("A complete command vector is required")
            if not bytes.fromhex(command["expectedHex"]):
                raise ValueError("An independent output oracle is required")
    verify_fingerprints(data["fingerprints"])
    for tree in data["frozenTrees"]:
        files = {str(path.relative_to(tree["root"])): digest(path)
                 for path in Path(tree["root"]).rglob("*") if path.is_file()}
        if files != tree["files"]:
            raise ValueError("Frozen tree inventory changed: " + tree["root"])


def reference(kind: str, executable: Path, receipt: Path) -> dict:
    data = json.loads(receipt.read_text(encoding="utf-8"))
    if kind == "python":
        config = data.get("configuration", {})
        if (data.get("state") != "PASS" or config.get("normalGIL") is not True or
                config.get("debug") is not False or config.get("experimentalJIT") is not False or
                not config.get("optimization") or not data.get("commands") or
                not isinstance(data.get("compiler"), dict)):
            raise ValueError("CPython requires the completed official optimized provider receipt schema")
        for command in data["commands"]:
            if command.get("exit") != 0 or command.get("ownedChildrenDrained") is not True:
                raise ValueError("Reference provider command did not complete successfully")
        identity = data.get("binary", {})
        compiler = data["compiler"]
    elif (data.get("state") == "PREPARED_NOT_TIMED" and data.get("lua_jump_table") == 1 and
          str(data.get("lua_version", "")).startswith("Lua 5.5.") and
          re.fullmatch(r"[0-9a-f]{40}", str(data.get("lua_head", ""))) and
          data.get("common_flags") and data.get("compiler") and data.get("compiler_sha256")):
        identity = {"path": data["lua_executable"], "sha256": data["lua_sha256"]}
        compiler = {"path": data["compiler"], "sha256": data["compiler_sha256"]}
    else:
        raise ValueError("Lua requires the completed pinned jump-table provider receipt schema")
    if not identity or not os.path.samefile(executable, identity["path"]) or digest(executable) != identity["sha256"]:
        raise ValueError("Reference requires an exact sealed executable receipt: " + kind)
    if digest(Path(compiler["path"])) != compiler["sha256"]:
        raise ValueError("Reference compiler differs from its sealed identity")
    dependencies = [fingerprint(executable), fingerprint(receipt), fingerprint(Path(compiler["path"]))]
    if kind == "python":
        for command in data["commands"]:
            for key in ("stdout", "stderr"):
                stream = command[key]
                if digest(Path(stream["path"])) != stream["sha256"]:
                    raise ValueError("Reference provider raw stream changed")
                dependencies.append(fingerprint(Path(stream["path"])))
    if "coreDLL" in data:
        dll = data["coreDLL"]
        if digest(Path(dll["path"])) != dll["sha256"]:
            raise ValueError("Reference core DLL differs from its receipt")
        dependencies.append(fingerprint(Path(dll["path"])))
    return {"kind": kind, "executable": str(executable), "receipt": str(receipt),
            "fingerprints": dependencies, "source": data}


def compiler_flags(compiler: Path, header: str, version: bytes) -> tuple[list, str]:
    if os.name == "nt":
        name = compiler.name.lower()
        if name not in ("cl.exe", "clang-cl.exe"):
            raise ValueError("NOT_RUN: Windows requires explicit cl.exe or clang-cl.exe")
        crt = int(re.search(r"#define XRT_NATIVE_CRT (\d+)", header)[1])
        if crt not in (0, 2):
            raise ValueError("Measurements require a non-debug runtime")
        flags = ["/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/O2", ("/MT", "/MTd", "/MD", "/MDd")[crt]]
        flags.append("/clang:-ffp-contract=off" if name == "clang-cl.exe" else "/fp:precise")
        if name == "cl.exe":
            flags.append("/wd4723")
        return flags, "clang-cl" if name == "clang-cl.exe" else "MSVC"
    if sys.platform not in ("linux", "darwin"):
        raise ValueError("NOT_RUN: unsupported host platform " + sys.platform)
    text = version.decode("utf-8", errors="replace").lower()
    family = "Clang" if "clang" in text else "GCC" if "gcc" in text or "free software foundation" in text else None
    if family is None:
        raise ValueError("NOT_RUN: explicit Unix compiler is neither Clang nor GCC")
    return ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-O2", "-ffp-contract=off"], family


def prepare(args) -> None:
    args.work.mkdir(parents=True)
    root, build, work = args.root.resolve(), args.build.resolve(), args.work.resolve()
    env = environment(root)
    fixtures = root / "bench/array-access"
    manifest = json.loads((fixtures / "manifest.json").read_text(encoding="utf-8"))
    verify_fingerprints([{"path": str(fixtures / name), "sha256": value} for name, value in manifest["sha256"].items()])
    selected = args.case or list(manifest["cases"])
    if len(selected) != len(set(selected)):
        raise ValueError("A benchmark case may only be declared once")
    binary = build / ("xray.exe" if os.name == "nt" else "xray")
    compiler = args.cc.resolve(strict=True)
    result = {"schema": 1, "state": "PREPARING", "scope": args.scope, "root": str(root),
              "platform": sys.platform, "driver": fingerprint(Path(__file__)),
              "commands": [], "engines": {}, "fingerprints": [], "references": {},
              "thresholds": {}, "open": manifest["open"], "measurement": False}

    def run(name, argv):
        row = execute(argv, work, name, env)
        result["commands"].append(row)
        save(work / "preparation.json", result)
        if not row["ok"]:
            raise RuntimeError("Preparation failed: " + name)
        return b"".join(Path(row["streams"][key]["path"]).read_bytes() for key in ("stdout", "stderr"))

    try:
        head = clean_head(root)
        identity = run("identity", [binary, "--version", "--json"])
        result["identity"] = json.loads(identity)
        if result["identity"]["commit"] != head or result["identity"]["dirty"] or result["identity"]["buildProfile"] != "Release":
            raise ValueError("CLI must identify the exact clean Release source commit")
        frozen = work / binary.name
        shutil.copyfile(binary, frozen)
        header_path = build / "generated/xray_native_link_paths.h"
        header = header_path.read_text(encoding="utf-8")
        if "#define XRT_NATIVE_ASAN 0" not in header or "#define XRT_NATIVE_UBSAN 0" not in header:
            raise ValueError("Performance products must use uninstrumented runtime libraries")
        runtime = re.findall(r'"([^"]+)"', header.split("xrt_native_development_libraries[] = {", 1)[1].split("};", 1)[0])
        if len(runtime) != 5:
            raise ValueError("Expected all five effective runtime libraries")
        library_dir = work / "libraries"
        library_dir.mkdir()
        for path in runtime:
            shutil.copyfile(path, library_dir / Path(path).name)
        for filename in ("CMakeCache.txt", "compile_commands.json"):
            shutil.copyfile(build / filename, work / filename)
        shutil.copyfile(header_path, work / header_path.name)
        shutil.copytree(root / "include", work / "include")
        shutil.copytree(root / "stdlib", work / "stdlib")
        result["stdlib"] = str(work / "stdlib")
        env["XRAY_STDLIB_PATH"] = result["stdlib"]
        result["nativeFlagSource"] = fingerprint(root / "src/app/cli/xcli_lir_native.c")
        version = run("compiler-version", [compiler, "/?"] if compiler.name.lower() == "cl.exe" else [compiler, "--version"])
        flags, family = compiler_flags(compiler, header, version)
        rows = json.loads((work / "compile_commands.json").read_text(encoding="utf-8"))
        runtime_rows = [row for row in rows if row["file"].replace("\\", "/").endswith("/xnative_array.c")]
        if len(runtime_rows) != 1:
            raise ValueError("Missing unique effective Array runtime compiler command")
        command = runtime_rows[0].get("arguments")
        if command is None:
            match = re.match(r'^"([^"]+)"|^(\S+)', runtime_rows[0]["command"])
            command = [match[1] or match[2]]
        if not os.path.samefile(command[0], compiler):
            raise ValueError("Runtime and C reference must use the same actual compiler")
        result["provider"] = {"compiler": fingerprint(compiler), "family": family, "flags": flags,
                              "runtimeCompileCommand": runtime_rows[0], "includeSearch": env.get("INCLUDE"), "librarySearch": env.get("LIB")}
        for kind in ("lua", "python"):
            executable, receipt = getattr(args, kind), getattr(args, kind + "_receipt")
            if (executable is None) != (receipt is None):
                raise ValueError("Both reference executable and sealed receipt are required: " + kind)
            if executable:
                result["references"][kind] = reference(kind, executable.resolve(strict=True), receipt.resolve(strict=True))
                run(kind + "-identity", [executable, "-E", "--version"] if kind == "python" else [executable, "-v"])
        inputs = work / "inputs"
        shutil.copytree(fixtures, inputs)
        for name in selected:
            case = manifest["cases"][name]
            result["thresholds"][name] = {"nativeOverC": case["nativeOverC"], "source": case["thresholdSource"]}
            source, ref = inputs / case["xr"], inputs / case["c"]
            generated, native = work / (name + ".c"), work / (name + "-native" + (".exe" if os.name == "nt" else ""))
            run(name + "-generate", [binary, "build", "--c-only", source, "-o", generated])
            verbose = run(name + "-native-build", [binary, "build", "-v", "--cc", compiler, source, "-o", native])
            choices = [line[len("C compiler "):].strip() for line in verbose.decode(errors="replace").splitlines() if line.startswith("C compiler ")]
            if len(choices) != 1 or not os.path.samefile(choices[0], compiler):
                raise ValueError("CLI did not report the requested actual native compiler")
            obj = work / (name + (".obj" if os.name == "nt" else ".o"))
            executable = work / (name + "-C" + (".exe" if os.name == "nt" else ""))
            if os.name == "nt":
                run(name + "-C-compile", [compiler, *flags, "/c", ref, "/Fo" + str(obj)])
                run(name + "-C-link", [compiler, "/nologo", flags[6], obj, "/Fe" + str(executable)])
            else:
                run(name + "-C-compile", [compiler, *flags, "-c", ref, "-o", obj])
                run(name + "-C-link", [compiler, obj, "-o", executable])
            inspection = work / (name + "-native-inspection" + obj.suffix)
            run(name + "-native-object", [compiler, *flags, "/c", "/I" + str(work / "include"), generated, "/Fo" + str(inspection)] if os.name == "nt" else
                [compiler, *flags, "-I" + str(work / "include"), "-c", generated, "-o", inspection])
            logical = case["stdout"].encode()
            text = logical.replace(b"\n", b"\r\n") if os.name == "nt" else logical
            engines = {"native": {"argv": [str(native)], "expectedHex": logical.hex()},
                       "C": {"argv": [str(executable)], "expectedHex": text.hex()}}
            if args.scope == "array":
                engines["VM"] = {"argv": [str(frozen), "run", str(source)], "expectedHex": logical.hex()}
            for kind, provider in result["references"].items():
                if kind in case:
                    engines[kind] = {"argv": [provider["executable"], *(["-E"] if kind == "python" else []), str(inputs / case[kind])], "expectedHex": text.hex()}
            result["engines"][name] = engines
        dependencies = [p for p in work.rglob("*") if p.is_file() and p.name not in ("preparation.json", "prepared.json")]
        dependencies += [Path(__file__), Path(xray_process.__file__), compiler]
        result["fingerprints"] = [fingerprint(path) for path in sorted(set(dependencies))]
        for provider in result["references"].values():
            result["fingerprints"] += provider["fingerprints"]
        result["fingerprints"].append(result["nativeFlagSource"])
        result["frozenTrees"] = [{"root": str(work / directory), "files": {
            str(path.relative_to(work / directory)): digest(path)
            for path in (work / directory).rglob("*") if path.is_file()}}
            for directory in ("stdlib", "include", "inputs", "libraries")]
        if clean_head(root) != head:
            raise ValueError("Source commit changed during preparation")
        result.update(state="PREPARED_NOT_MEASURED", rounds=ROUNDS, timeoutSeconds=PRODUCT_TIMEOUT)
        save(work / "preparation.json", result)
        save(work / "prepared.json", result)
    except BaseException as error:
        result.update(state="FAIL", error=repr(error))
        save(work / "preparation.json", result)
        raise


def measure(args) -> None:
    prepared = json.loads(args.prepared.read_text(encoding="utf-8"))
    verify_prepared(prepared)
    if prepared["state"] != "PREPARED_NOT_MEASURED" or prepared["driver"] != fingerprint(Path(__file__)):
        raise ValueError("Preparation identity differs from this driver")
    if prepared["platform"] != sys.platform:
        raise ValueError("NOT_RUN: preparation and measurement hosts differ")
    head = clean_head(Path(prepared["root"]))
    if head != prepared["identity"]["commit"]:
        raise ValueError("Prepared source commit changed before measurement")
    verify_fingerprints(prepared["fingerprints"])
    args.work.mkdir(parents=True)
    env = environment(Path(prepared["root"]))
    env["XRAY_STDLIB_PATH"] = prepared["stdlib"]
    result = {"schema": 1, "state": "RUNNING", "prepared": fingerprint(args.prepared),
              "samples": [], "medians": {}, "rounds": ROUNDS, "scope": prepared["scope"]}
    try:
        for case, engines in prepared["engines"].items():
            for round_index in range(ROUNDS):
                order = list(engines) if round_index % 2 == 0 else list(reversed(engines))
                for engine in order:
                    command = engines[engine]
                    row = execute(command["argv"], args.work, f"{case}-{round_index}-{engine}", env,
                                  bytes.fromhex(command["expectedHex"]))
                    row.update(case=case, engine=engine, round=round_index)
                    result["samples"].append(row)
                    save(args.work / "result.json", result)
                    if row["state"] == "NEEDS_RECOVERY":
                        raise xray_process.RecoveryRequired(row["error"])
        verify_prepared(prepared)
        if clean_head(Path(prepared["root"])) != head:
            raise ValueError("Source commit changed during measurement")
        complete = all(row["ok"] for row in result["samples"])
        for case, engines in prepared["engines"].items():
            result["medians"][case] = {}
            for engine in engines:
                rows = [r for r in result["samples"] if r["case"] == case and r["engine"] == engine]
                if len(rows) == ROUNDS and all(r["ok"] for r in rows):
                    result["medians"][case][engine] = {key: statistics.median(r[key] for r in rows)
                        for key in ("processWallSeconds", "ownedWallSeconds")}
                else:
                    result["medians"][case][engine] = {"state": "FAIL", "reason": "No valid-only median"}
        result["state"] = "PASS_EXECUTION_ONLY" if complete else "FAIL"
    except BaseException as error:
        result.update(state="FAIL", error=repr(error))
        raise
    finally:
        save(args.work / "result.json", result)
    if not complete:
        raise RuntimeError("At least one declared sample failed; inspect retained raw files")


def assert_result(args) -> int:
    prepared = json.loads(args.prepared.read_text(encoding="utf-8"))
    verify_prepared(prepared)
    result = json.loads(args.result.read_text(encoding="utf-8"))
    report = {"scope": args.scope, "gates": {}, "notRun": dict(prepared["open"]),
              "boundary": "Single-build native Array subgate only. Other layouts, platforms, structural hot-path gates and complete Task339 qualification remain OPEN."}
    valid = result["state"] == "PASS_EXECUTION_ONLY" and result["prepared"] == fingerprint(args.prepared)
    expected_count = ROUNDS * sum(len(engines) for engines in prepared["engines"].values())
    valid &= len(result["samples"]) == expected_count
    for case, engines in prepared["engines"].items():
        rows = [r for r in result["samples"] if r["case"] == case]
        valid &= len(rows) == ROUNDS * len(engines) and all(r["ok"] for r in rows)
        actual = {}
        for engine, command in engines.items():
            samples = [row for row in rows if row["engine"] == engine]
            valid &= len(samples) == ROUNDS and sorted(row["round"] for row in samples) == list(range(ROUNDS))
            for row in samples:
                for stream in row["streams"].values():
                    if digest(Path(stream["path"])) != stream["sha256"]:
                        raise ValueError("Changed measured raw stream")
                valid &= row["argv"] == command["argv"] and row["exit"] == 0 and not row["timedOut"]
                valid &= row["state"] == "TERMINAL_DRAINED" and "error" not in row
                valid &= Path(row["streams"]["stdout"]["path"]).read_bytes().hex() == command["expectedHex"]
                valid &= not Path(row["streams"]["stderr"]["path"]).read_bytes()
                valid &= math.isfinite(row.get("processWallSeconds", 0)) and row.get("processWallSeconds", 0) > 0
            if valid:
                actual[engine] = statistics.median(row["processWallSeconds"] for row in samples)
        if valid:
            ratio = actual["native"] / actual["C"]
            limit = prepared["thresholds"][case]["nativeOverC"]
            report["gates"][case] = {"ratio": ratio, "limit": limit, "pass": ratio <= limit,
                                      "source": prepared["thresholds"][case]["source"]}
    missing = [kind for kind in ("lua", "python") if kind not in prepared["references"]]
    if missing:
        report["notRun"]["reference observations"] = missing
    if args.scope == "complete":
        report["state"] = "NOT_RUN"
        report["reason"] = "Record, Map, whole-VM and module qualification are outside this native Array subgate"
    else:
        passed = valid and len(report["gates"]) == len(prepared["engines"]) and all(g["pass"] for g in report["gates"].values())
        report["state"] = "PASS_NATIVE_ARRAY_ONLY" if passed else "FAIL"
    output = args.output or args.result.parent / ("gate-" + args.scope + ".json")
    if output.exists():
        raise FileExistsError("Gate evidence already exists: " + str(output))
    save(output, report)
    print(json.dumps(report, indent=2))
    return 0 if report["state"] == "PASS_NATIVE_ARRAY_ONLY" else 1


def self_test(args) -> None:
    args.work.mkdir(parents=True)
    cases = {"good": ("import sys;sys.stdout.buffer.write(b'ok\\n')", PRODUCT_TIMEOUT, True),
             "wrong-output": ("print('wrong')", PRODUCT_TIMEOUT, False),
             "stderr": ("import sys;sys.stdout.buffer.write(b'ok\\n');sys.stderr.write('bad')", PRODUCT_TIMEOUT, False),
             "nonzero": ("import sys;sys.stdout.buffer.write(b'ok\\n');sys.exit(7)", PRODUCT_TIMEOUT, False),
             "timeout": ("import time;time.sleep(2)", 0.1, False)}
    rows = []
    for name, (source, timeout, expected) in cases.items():
        row = execute([sys.executable, "-E", "-c", source], args.work, name, environment(Path.cwd()),
                      b"ok\n", timeout=timeout)
        rows.append(dict(row, name=name))
        save(args.work / "self-test.json", {"state": "RUNNING", "rows": rows, "benchmarkMeasurement": False})
        if row["ok"] != expected or row["state"] != "TERMINAL_DRAINED" or (name == "timeout" and not row["timedOut"]):
            raise RuntimeError("Hard-failure self-test failed: " + name)
    rejected = []
    for state in ("NOT_RUN", "UNKNOWN", "RUNNING", "FAIL"):
        receipt = args.work / ("bad-receipt-" + state + ".json")
        save(receipt, {"state": state, "binary": fingerprint(Path(sys.executable))})
        try:
            reference("python", Path(sys.executable), receipt)
        except ValueError:
            rejected.append(state)
        else:
            raise RuntimeError("An incomplete reference receipt was accepted")
    save(args.work / "self-test.json", {"state": "PASS", "rows": rows,
                                        "rejectedReceiptStates": rejected, "benchmarkMeasurement": False})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare_parser = commands.add_parser("prepare")
    for name in ("root", "build", "cc", "work"):
        prepare_parser.add_argument("--" + name, type=Path, required=True)
    prepare_parser.add_argument("--scope", choices=("native", "array"), default="native")
    prepare_parser.add_argument("--case", choices=("push", "inplace"), action="append")
    for name in ("lua", "lua-receipt", "python", "python-receipt"):
        prepare_parser.add_argument("--" + name, type=Path)
    measure_parser = commands.add_parser("measure")
    measure_parser.add_argument("--prepared", type=Path, required=True)
    measure_parser.add_argument("--work", type=Path, required=True)
    assert_parser = commands.add_parser("assert")
    assert_parser.add_argument("--prepared", type=Path, required=True)
    assert_parser.add_argument("--result", type=Path, required=True)
    assert_parser.add_argument("--scope", choices=("native", "complete"), required=True)
    assert_parser.add_argument("--output", type=Path)
    test_parser = commands.add_parser("self-test")
    test_parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    for name, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, name, value.resolve())
    if args.command == "assert":
        return assert_result(args)
    {"prepare": prepare, "measure": measure, "self-test": self_test}[args.command](args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
