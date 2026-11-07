#!/usr/bin/env python3
"""Measure public CLI scalar workloads against same-compiler C and Lua."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import struct
import subprocess
import time
from types import SimpleNamespace

import new_path_baseline as baseline


def code_hash(path: Path) -> str:
    data = path.read_bytes()
    if data[:2] == b"MZ":
        pe = struct.unpack_from("<I", data, 0x3c)[0]
        start = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
        for index in range(struct.unpack_from("<H", data, pe + 6)[0]):
            section = start + 40 * index
            if data[section:section + 8].rstrip(b"\0") == b".text":
                size, offset = struct.unpack_from("<II", data, section + 16)
                return hashlib.sha256(data[offset:offset + size]).hexdigest()
    elif data[:4] == b"\xcf\xfa\xed\xfe":
        command = 32
        for _ in range(struct.unpack_from("<I", data, 16)[0]):
            kind, size = struct.unpack_from("<II", data, command)
            if kind == 0x19:
                for index in range(struct.unpack_from("<I", data, command + 64)[0]):
                    section = command + 72 + index * 80
                    if data[section:section + 16].rstrip(b"\0") == b"__text":
                        length = struct.unpack_from("<Q", data, section + 40)[0]
                        offset = struct.unpack_from("<I", data, section + 48)[0]
                        return hashlib.sha256(data[offset:offset + length]).hexdigest()
            command += size
    raise ValueError("Cannot locate the executable code section")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--lua", type=Path, required=True)
    parser.add_argument("--cc", type=Path, required=True)
    parser.add_argument("--alignment", type=int, choices=(16, 32, 64), required=True)
    parser.add_argument("--samples", type=int, default=9)
    args = parser.parse_args()
    if args.samples < 3:
        parser.error("At least three interleaved samples are required")
    args.root, args.build, args.work, args.lua, args.cc = (
        value.resolve() for value in (args.root, args.build, args.work, args.lua, args.cc))
    args.work.mkdir(parents=True)
    binary = baseline.tool(args.build, "xray")
    env = dict(os.environ, NO_COLOR="1", XRAY_STDLIB_PATH=str(args.root / "stdlib"))
    result = {"state": "RUNNING", "alignment": args.alignment,
              "platform": platform.platform(), "machine": platform.machine(),
              "compiler": str(args.cc), "commands": [], "samples": [], "medians": {},
              "products": {},
              "boundary": "Public CLI run includes process startup and compilation. Native and "
                          "reference C timings include startup and execution. Compilation and "
                          "linking are recorded separately. All samples are retained. Only "
                          "external C and Lua CRLF is normalized for value comparison. Native "
                          "gates use a fixed quiet serial follow-up, retaining the initial mix."}

    def save() -> None:
        (args.work / "result.json").write_text(
            json.dumps(result, indent=2) + "\n", encoding="utf-8")

    def run(name: str, command: list) -> tuple[bytes, float]:
        argv = list(map(str, command))
        start = time.perf_counter()
        try:
            child = subprocess.run(argv, env=env, capture_output=True, timeout=30)
        except subprocess.TimeoutExpired as error:
            child = SimpleNamespace(returncode=124, stdout=error.stdout or b"",
                                    stderr=error.stderr or b"")
        seconds = time.perf_counter() - start
        row = {"name": name, "command": argv, "exit": child.returncode, "seconds": seconds}
        for stream, data in (("stdout", child.stdout), ("stderr", child.stderr)):
            path = args.work / (name + "." + stream)
            path.write_bytes(data)
            row[stream] = {"path": str(path), "sha256": hashlib.sha256(data).hexdigest()}
        result["commands"].append(row)
        save()
        if child.returncode or (name.startswith("run-") and child.stderr):
            raise RuntimeError(name + " failed, see its retained streams")
        return child.stdout, seconds

    save()
    try:
        identity, _ = run("identity", [binary, "--version", "--json"])
        result["identity"] = json.loads(identity)
        head = subprocess.check_output(
            ["git", "-C", str(args.root), "rev-parse", "HEAD"], text=True).strip()
        if result["identity"]["commit"] != head or result["identity"]["dirty"]:
            raise RuntimeError("CLI must match the exact clean source")
        if result["identity"]["buildProfile"] != "Release":
            raise RuntimeError("Scalar measurements require a Release build")
        run("compiler-version", [args.cc, "--version"])
        run("lua-version", [args.lua, "-v"])
        result["executables"] = {
            name: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                   "codeSha256": code_hash(path)} for name, path in (("Xray", binary), ("Lua", args.lua))}
        result["cmakeCache"] = (args.build / "CMakeCache.txt").read_text(encoding="utf-8")
        configuration = SimpleNamespace(**dict(vars(args), no_native=True, include_carrier=False,
                                               bench=[], python=None))
        programs, _ = baseline.bench_programs(configuration, args.root)
        suffix, prefix = (".lib", "") if os.name == "nt" else (".a", "lib")
        libraries = [args.build / directory / (prefix + name + suffix)
                     for directory, name in baseline.LIBS]
        result["libraries"] = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                               for path in libraries}
        msvc = args.cc.name.lower() in ("cl.exe", "clang-cl.exe")
        flags = (["/nologo", "/std:c11", "/O2", "/Ob2", "/DNDEBUG", "/MT",
                  "/clang:-ffp-contract=off", f"/clang:-falign-functions={args.alignment}"]
                 if msvc else ["-std=c11", "-O2", "-DNDEBUG", "-ffp-contract=off",
                               f"-falign-functions={args.alignment}"])
        result["nativeCompilerFlags"] = flags
        engines = {}
        for index, (label, original) in enumerate(programs.items()):
            source = Path(original["LIR VM"][1])
            row = {"Xray VM": [binary, "run", source]}
            if "Lua" in original:
                result["products"][label] = {}
                row["Lua"] = original["Lua"]
                generated = source.with_suffix(".native.c")
                run(f"emit-{index}", [binary, "build", source, "--c-only", "-o", generated])
                reference = source.with_name(source.stem + "_ref.c")
                for engine, code in (("native", generated), ("C", reference)):
                    executable = code.with_suffix(baseline.EXE or ".out")
                    obj = code.with_suffix(".obj" if msvc else ".o")
                    command = [args.cc, *flags]
                    if engine == "native":
                        command += ["/I" + str(args.root / "include") if msvc else
                                    "-I" + str(args.root / "include")]
                    command += ["/c", code, "/Fo" + str(obj)] if msvc else ["-c", code, "-o", obj]
                    run(f"compile-{index}-{engine}", command)
                    command = [args.cc, "/MT", obj] if msvc else [args.cc, obj]
                    if engine == "native":
                        command += libraries if msvc else libraries + libraries
                        command += ["advapi32.lib", "userenv.lib"] if msvc else (
                            [] if platform.system() == "Darwin" else ["-lm", "-lpthread", "-ldl"])
                    command += ["/Fe" + str(executable), "/link", "/INCREMENTAL:NO", "/OPT:REF"] if msvc else ["-o", executable]
                    if platform.system() == "Darwin":
                        command += ["-Wl,-dead_strip"]
                    run(f"link-{index}-{engine}", command)
                    row[engine] = [executable]
                    result["products"][label][engine] = {
                        "path": str(executable), "bytes": executable.stat().st_size,
                        "sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                        "codeSha256": code_hash(executable)}
                    if engine == "native" and platform.system() == "Darwin":
                        retained = code.with_suffix(".unstripped.out")
                        comparison = [str(value) for value in command if value != "-Wl,-dead_strip"]
                        comparison[comparison.index("-o") + 1] = str(retained)
                        run(f"link-{index}-unstripped", comparison)
                        row["native-unstripped"] = [retained]
                        result["products"][label]["native-unstripped"] = {
                            "path": str(retained), "bytes": retained.stat().st_size,
                            "sha256": hashlib.sha256(retained.read_bytes()).hexdigest(),
                            "codeSha256": code_hash(retained)}
            engines[label] = row
        for sample in range(args.samples):
            for index, (label, row) in enumerate(engines.items()):
                order = list(row)
                order = order[sample % len(order):] + order[:sample % len(order)]
                if "native-unstripped" in order:
                    first, second = sorted((order.index("native"), order.index("native-unstripped")))
                    pair = ("native", "native-unstripped")
                    if sample % 2:
                        pair = pair[::-1]
                    order[first], order[second] = pair
                outputs = []
                for engine in order:
                    data, seconds = run(f"run-{sample}-{index}-{engine.replace(' ', '_')}", row[engine])
                    comparable = data.replace(b"\r\n", b"\n") if engine in ("C", "Lua") else data
                    outputs.append(comparable)
                    result["samples"].append({"sample": sample, "label": label, "engine": engine,
                                              "seconds": seconds})
                if len(set(outputs)) != 1 or (len(row) == 1 and outputs != [b"1\n"]):
                    raise RuntimeError(label + " output mismatch")
                save()
        result["medians"] = {
            label: {engine: statistics.median(row["seconds"] for row in result["samples"]
                                             if row["label"] == label and row["engine"] == engine)
                    for engine in engines[label]} for label in engines}
        limits = {"fib(32)": .25, "loop 100M": 2.3, "print(1)": .020,
                  "import regex; print(1)": 2.0}
        result["gates"] = {label: result["medians"][label]["Xray VM"] <= limit
                           for label, limit in limits.items()}
        result["nativeRatios"] = {label: row["native"] / row["C"]
                                  for label, row in result["medians"].items() if "C" in row}
        result["nativeSerialSamples"] = []
        for sample in range(max(args.samples, 9)):
            for index, (label, row) in enumerate(engines.items()):
                order = [engine for engine in row if engine in ("native", "native-unstripped", "C")]
                if sample % 2:
                    order.reverse()
                outputs = []
                for engine in order:
                    data, seconds = run(f"serial-{sample}-{index}-{engine}", row[engine])
                    outputs.append(data.replace(b"\r\n", b"\n") if engine == "C" else data)
                    result["nativeSerialSamples"].append({"sample": sample, "label": label,
                                                           "engine": engine, "seconds": seconds})
                if outputs and len(set(outputs)) != 1:
                    raise RuntimeError(label + " serial native output mismatch")
        result["nativeSerialMedians"] = {
            label: {engine: statistics.median(row["seconds"] for row in result["nativeSerialSamples"]
                                             if row["label"] == label and row["engine"] == engine)
                    for engine in engines[label] if engine in ("native", "native-unstripped", "C")}
            for label in result["nativeRatios"]}
        result["nativeSerialRatios"] = {label: row["native"] / row["C"]
                                        for label, row in result["nativeSerialMedians"].items()}
        result["gates"]["native-fib"] = result["nativeSerialRatios"]["fib(32)"] <= 1.1
        result["gates"]["native-mandelbrot"] = result["nativeSerialRatios"]["mandelbrot"] <= 1.1
        result["state"] = "PASS" if all(result["gates"].values()) else "FAIL"
    except BaseException as error:
        result.update(state="FAIL", error=str(error))
        raise
    finally:
        save()
    print(json.dumps({key: result[key] for key in ("state", "medians", "nativeRatios",
                                                 "nativeSerialRatios", "gates")}, indent=2))
    if result["state"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
