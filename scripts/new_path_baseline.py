#!/usr/bin/env python3
"""Compare source-backed LIR drivers and reference engines on one build.

Build first (macOS needs the extra define until the checkout fixes it):

    cmake -S <root> -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS=-D_DARWIN_C_SOURCE
    ninja -C <build> xray lir_vm_run lir_c_run xray_native_runtime

Then:

    python3 new_path_baseline.py --root <root> --build <build> --work <dir> --lua <lua>

For feedback during implementation, select only the affected benchmarks:

    python3 new_path_baseline.py --root <root> --build <build> --work <new-dir> \
        --no-census --bench fib --bench loop10m --lua <lua> --python <python>

For the independent regex user-module scaling sample gate:

    python3 new_path_baseline.py --root <root> --build <build> --work <new-dir> \
        --compile-scale --no-census --no-bench

The scaling mode retains all samples and returns 1 when its M2 sample gate fails.

The driver census runs every original smoke input through lir_vm_run, then
emits, links and runs the generated C. lir_c_run takes no standard library
path: it resolves imports from the source table embedded at build time, so
its result matches --stdlib only when both come from the same commit.
Product scores come from tests/smoke/run_smoke.py, including real test suites.
Public CLI scalar measurements use scripts/run_scalar_performance.py.

Pass --lua to add a Lua interpreter, built with the same compiler and -O2, to
the benchmarks; the scalar VM gate compares against it. Lua enables
computed-goto dispatch only when __GNUC__ is defined, and clang-cl does not
define it, while the Xray register VM enables it under __clang__. Build Lua
with -DLUA_USE_JUMPTABLE=1 under clang-cl so both use the same dispatch. The retired carrier is
not timed unless --include-carrier is requested. Linking is implemented
for clang and gcc. On MSVC pass --no-native; the row
still reports whether C was emitted, and tests/lir/test_lir_c_native.py holds
the MSVC command line.
"""
import argparse
import collections
import hashlib
import importlib.util
import json
import os
import pathlib
import platform
import re
import statistics
import subprocess
import sys
import threading
import time

EXE = ".exe" if os.name == "nt" else ""
BENCH_LABELS = {"fib": "fib(32)", "loop10m": "loop 10M", "loop100m": "loop 100M",
                "mandelbrot": "mandelbrot", "hello": "print(1)",
                "regex": "import regex; print(1)"}
LIBS = ("xray_native_runtime", "xray_xir_scalar", "xray_xir_declarations",
        "xray_xir_admission", "xray_compile_resources")
ADAPTER = """#include <stdint.h>
extern {result} xray_lir_entry(void);
int main(void) {{ (void)xray_lir_entry(); return 0; }}
"""
FIB_XR = """fn fib(n: i64) -> i64 { return n < 2 ? n : fib(n - 1) + fib(n - 2) }
print(fib(32))
"""
LOOP_XR = """fn work(n: i64) -> i64 {
    var i = 0
    var t = 0
    while (i < n) {
        t = t + i * 3 - (i % 7)
        i = i + 1
    }
    return t
}
print(work(@N@))
"""
FIB_C = """#include <stdint.h>
#include <stdio.h>
static int64_t fib(int64_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
int main(void) { printf("%lld\\n", (long long)fib(32)); return 0; }
"""
LOOP_C = """#include <stdint.h>
#include <stdio.h>
__attribute__((noinline)) static int64_t work(int64_t n) {
    int64_t i = 0, t = 0;
    while (i < n) { t = t + i * 3 - (i % 7); i = i + 1; }
    return t;
}
int main(void) { printf("%lld\\n", (long long)work(@N@)); return 0; }
"""
FIB_PY = """def fib(n): return n if n < 2 else fib(n - 1) + fib(n - 2)
print(fib(32))
"""
LOOP_PY = """def work(n):
    i = 0
    t = 0
    while i < n:
        t = t + i * 3 - (i % 7)
        i = i + 1
    return t
print(work(@N@))
"""


FIB_LUA = """local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
print(fib(32))
"""
LOOP_LUA = """local function work(n)
  local i, t = 0, 0
  while i < n do t = t + i * 3 - (i % 7); i = i + 1 end
  return t
end
print(work(@N@))
"""
MANDEL_XR = """fn mandel(w: i64, h: i64) -> i64 {
    var count = 0
    var y = 0
    while (y < h) {
        var x = 0
        while (x < w) {
            var cr = 2.0 * (x as f64) / (w as f64) - 1.5
            var ci = 2.0 * (y as f64) / (h as f64) - 1.0
            var zr = 0.0
            var zi = 0.0
            var i = 0
            while (i < 50 && zr * zr + zi * zi <= 4.0) {
                var t = zr * zr - zi * zi + cr
                zi = 2.0 * zr * zi + ci
                zr = t
                i = i + 1
            }
            if (i == 50) { count = count + 1 }
            x = x + 1
        }
        y = y + 1
    }
    return count
}
print(mandel(800, 800))
"""
MANDEL_C = """#include <stdint.h>
#include <stdio.h>
__attribute__((noinline)) static int64_t mandel(int64_t w, int64_t h) {
    int64_t count = 0;
    for (int64_t y = 0; y < h; y++) for (int64_t x = 0; x < w; x++) {
        double cr = 2.0 * (double)x / (double)w - 1.5, ci = 2.0 * (double)y / (double)h - 1.0, zr = 0.0, zi = 0.0;
        int64_t i = 0;
        while (i < 50 && zr * zr + zi * zi <= 4.0) {
            double t = zr * zr - zi * zi + cr;
            zi = 2.0 * zr * zi + ci;
            zr = t;
            i = i + 1;
        }
        if (i == 50) count = count + 1;
    }
    return count;
}
int main(void) { printf("%lld\\n", (long long)mandel(800, 800)); return 0; }
"""
MANDEL_PY = """def mandel(w, h):
    count = 0
    y = 0
    while y < h:
        x = 0
        while x < w:
            cr = 2.0 * x / w - 1.5
            ci = 2.0 * y / h - 1.0
            zr = 0.0
            zi = 0.0
            i = 0
            while i < 50 and zr * zr + zi * zi <= 4.0:
                t = zr * zr - zi * zi + cr
                zi = 2.0 * zr * zi + ci
                zr = t
                i = i + 1
            if i == 50:
                count = count + 1
            x = x + 1
        y = y + 1
    return count
print(mandel(800, 800))
"""
MANDEL_LUA = """local function mandel(w, h)
  local count, y = 0, 0
  while y < h do
    local x = 0
    while x < w do
      local cr = 2.0 * x / w - 1.5
      local ci = 2.0 * y / h - 1.0
      local zr, zi, i = 0.0, 0.0, 0
      while i < 50 and zr * zr + zi * zi <= 4.0 do
        local t = zr * zr - zi * zi + cr
        zi = 2.0 * zr * zi + ci
        zr = t
        i = i + 1
      end
      if i == 50 then count = count + 1 end
      x = x + 1
    end
    y = y + 1
  end
  return count
end
print(mandel(800, 800))
"""


def tool(build, name):
    hits = [p for p in build.rglob(name + EXE) if p.is_file()]
    if not hits:
        sys.exit(f"missing {name}: build it in {build}")
    return min(hits, key=lambda p: len(p.parts))


def load_smoke(root):
    sys.path.insert(0, str(root / "tests/smoke"))
    spec = importlib.util.spec_from_file_location("run_smoke", root / "tests/smoke/run_smoke.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["run_smoke"] = module
    spec.loader.exec_module(module)
    return module


def libraries(build):
    found = []
    for name in LIBS:
        hits = sorted(build.rglob(f"lib{name}.a"), key=lambda p: len(p.parts))
        if not hits:
            sys.exit(f"missing lib{name}.a: build xray_native_runtime in {build}")
        found.append(str(hits[0]))
    return found + found


def link_generated(args, root, c_file, exe):
    """Link one LIR C file with the native runtime; returns compiler stderr."""
    text = c_file.read_text(errors="replace")
    match = re.search(r"(\w+) xray_lir_entry\(void\)", text)
    result = match.group(1) if match else "int64_t"
    adapter = c_file.with_suffix(".main.c")
    adapter.write_text(ADAPTER.format(result=result))
    strip = ["-Wl,-dead_strip", "-Wl,-w"] if platform.system() == "Darwin" else ["-Wl,--gc-sections"]
    system = [] if platform.system() == "Darwin" else ["-lm", "-lpthread", "-ldl"]
    command = [args.cc, "-std=c11", "-O2", "-ffp-contract=off", f"-I{root}/include", f"-I{args.build}",
               f"-I{args.build}/include", *strip, str(c_file), str(adapter),
               *libraries(args.build), *system, "-o", str(exe)]
    done = subprocess.run(command, capture_output=True, text=True)
    return "" if done.returncode == 0 else done.stderr[-400:]


def census(args, root, smoke):
    work = args.work / "census"
    work.mkdir(parents=True, exist_ok=True)
    cases = json.load(open(root / "tests/smoke/cases.json"))["cases"]
    fixed = {c["id"] for c in json.load(open(root / "tests/smoke/original-baseline.json"))["cases"]}
    sources = smoke.discover(root)
    vm, cgen = tool(args.build, "lir_vm_run"), tool(args.build, "lir_c_run")
    env = dict(os.environ, NO_COLOR="1")
    rows = []
    for case in cases:
        if case["id"] not in fixed:
            continue
        source = sources[case["id"].split("native:", 1)[-1]]
        stem = re.sub(r"[^A-Za-z0-9_]", "_", case["id"])
        path = root / source.path
        if source.text is not None:
            path = work / f"{stem}.xr"
            path.write_text(source.text, encoding="utf-8", newline="\n")
        expected = (root / case["stdout"]).read_bytes()
        row = {"id": case["id"], "current_path": case["expect"]}
        try:
            done = subprocess.run([str(vm), str(path), "--stdlib", str(root / "stdlib")],
                                  capture_output=True, env=env, cwd=root, timeout=args.timeout)
            row["vm"] = done.returncode == 0 and done.stdout == expected
            row["first_reject"] = done.stderr.decode("utf-8", "replace").strip().split("\n")[0][:600]
        except subprocess.TimeoutExpired:
            row["vm"], row["first_reject"] = False, "timeout"
        c_file = work / f"{stem}.c"
        done = subprocess.run([str(cgen), str(path), str(c_file)], capture_output=True, env=env, cwd=root)
        row["c_emitted"] = done.returncode == 0
        row["native"] = None
        if row["c_emitted"] and not args.no_native:
            exe = work / (stem + EXE)
            error = link_generated(args, root, c_file, exe)
            if error:
                row["native"], row["link_error"] = False, error
            else:
                ran = subprocess.run([str(exe)], capture_output=True, cwd=root, timeout=args.timeout)
                row["native"] = ran.returncode == 0 and ran.stdout == expected
        rows.append(row)
    return rows


def report_census(rows, no_native):
    passed = [r for r in rows if r["vm"]]
    print(f"\nfixed 60 rows: {len(rows)}; VM pass {len(passed)}; C emitted "
          f"{sum(r['c_emitted'] for r in rows)}" +
          ("" if no_native else f"; native pass {sum(bool(r['native']) for r in rows)}"))
    groups = collections.Counter()
    for row in rows:
        if row["vm"]:
            continue
        reject = row["first_reject"]
        groups["frontend (source)" if ": source " in reject else
               "lowering: module slots" if "module " in reject else
               "lowering: other" if ": lower " in reject else "other"] += 1
    for name, count in groups.most_common():
        print(f"  {count:3d} {name}")
    print("passing rows:")
    for row in passed:
        print(f"  {row['id']}")


def bench_programs(args, root):
    work = args.work / "bench"
    work.mkdir(parents=True, exist_ok=True)
    selected = {BENCH_LABELS[name] for name in args.bench} if args.bench else set(BENCH_LABELS.values())
    vm = tool(args.build, "lir_vm_run")
    scalar = selected - {BENCH_LABELS["hello"], BENCH_LABELS["regex"]}
    cgen = tool(args.build, "lir_c_run") if scalar and not args.no_native else None
    xray = tool(args.build, "xray") if args.include_carrier else None
    stdlib = root / "stdlib"
    programs = {"fib(32)": (FIB_XR, FIB_C, FIB_PY, FIB_LUA)}
    for label, n in (("loop 10M", 10_000_000), ("loop 100M", 100_000_000)):
        programs[label] = tuple(t.replace("@N@", str(n)) for t in (LOOP_XR, LOOP_C, LOOP_PY, LOOP_LUA))
    programs["mandelbrot"] = (MANDEL_XR, MANDEL_C, MANDEL_PY, MANDEL_LUA)
    env = dict(os.environ, XRAY_STDLIB_PATH=str(stdlib), NO_COLOR="1")
    engines = {}
    for label, (xr, c, py, lua) in programs.items():
        if label not in selected:
            continue
        stem = re.sub(r"\W", "_", label)
        xr_path, c_path, py_path = work / f"{stem}.xr", work / f"{stem}_ref.c", work / f"{stem}.py"
        xr_path.write_text(xr)
        c_path.write_text(c)
        py_path.write_text(py)
        row = {"LIR VM": [str(vm), str(xr_path), "--stdlib", str(stdlib)],
               "CPython": [str(args.python), str(py_path)]}
        if args.lua:
            lua_path = work / f"{stem}.lua"
            lua_path.write_text(lua)
            row["Lua"] = [str(args.lua), str(lua_path)]
        if not args.no_native:
            ref = work / f"{stem}_ref{EXE}"
            subprocess.run([args.cc, "-O2", "-ffp-contract=off", str(c_path), "-o", str(ref)], check=True)
            row["C -O2"] = [str(ref)]
            lir_c = work / f"{stem}_lir.c"
            subprocess.run([str(cgen), str(xr_path), str(lir_c)], check=True, env=env, timeout=args.timeout)
            lir_exe = work / f"{stem}_lir{EXE}"
            error = link_generated(args, root, lir_c, lir_exe)
            if error:
                print(f"{label}: LIR native skipped: {error[:160]}")
            else:
                row["LIR native"] = [str(lir_exe)]
        if args.include_carrier and label not in ("loop 100M", "mandelbrot"):
            row["current VM"] = [str(xray), "run", str(xr_path)]
            if not args.no_native:
                current = work / f"{stem}_current{EXE}"
                subprocess.run([str(xray), "build", str(xr_path), "-o", str(current)],
                               check=True, env=env, capture_output=True, timeout=args.timeout)
                row["current native"] = [str(current)]
        engines[label] = row
    for label, text in (("print(1)", "print(1)\n"), ("import regex; print(1)", "import regex\nprint(1)\n")):
        if label not in selected:
            continue
        stem = re.sub(r"\W", "_", label)
        path = work / f"{stem}.xr"
        path.write_text(text)
        engines[label] = {"LIR VM": [str(vm), str(path), "--stdlib", str(stdlib)]}
        if args.include_carrier:
            engines[label]["current VM"] = [str(xray), "run", str(path)]
    return engines, env


def bench(args, root):
    engines, env = bench_programs(args, root)
    samples = collections.defaultdict(list)
    outputs = collections.defaultdict(set)
    for _ in range(args.samples):  # Alternate engines so drift hits them evenly.
        for label, row in engines.items():
            for engine, command in row.items():
                start = time.perf_counter()
                done = subprocess.run(command, capture_output=True, env=env, timeout=args.timeout)
                samples[(label, engine)].append(time.perf_counter() - start)
                if done.returncode != 0:
                    sys.exit(f"{label} {engine} failed: {done.stderr[:300]}")
                outputs[label].add(done.stdout)
    for label, seen in outputs.items():
        if len(seen) != 1:
            sys.exit(f"{label}: engines disagree on output: {seen}")
    print(f"\nwall time, median of {args.samples} (process start to exit, seconds):")
    result = {}
    for label, row in engines.items():
        cells = {e: statistics.median(samples[(label, e)]) for e in row}
        result[label] = cells
        print(f"  {label:24s} " + "  ".join(f"{e} {t:.3f}" for e, t in sorted(cells.items(), key=lambda kv: -kv[1])))
    details = {label: {engine: {"command": command,
                               "samples_seconds": samples[(label, engine)]}
                       for engine, command in row.items()}
               for label, row in engines.items()}
    for label, seen in outputs.items():
        for detail in details[label].values():
            detail["stdout_sha256"] = hashlib.sha256(next(iter(seen))).hexdigest()
    return result, details


def compile_scale_command(command, env, prefix, timeout):
    """Observe the first program output, then drain and retain both streams."""
    first_output = []
    stdout_path = prefix.with_suffix(".stdout")
    stderr_path = prefix.with_suffix(".stderr")
    started = time.perf_counter()
    with stdout_path.open("xb") as output, stderr_path.open("xb") as errors:
        child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=errors,
                                 stdin=subprocess.DEVNULL, env=env)

        def receive():
            first = child.stdout.read(1)
            if first:
                first_output.append(time.perf_counter() - started)
            output.write(first)
            output.write(child.stdout.read())

        reader = threading.Thread(target=receive, daemon=True)
        reader.start()
        timed_out = False
        try:
            child.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            child.kill()
            child.wait()
        reader.join(timeout=5)
        if reader.is_alive():
            raise RuntimeError("Compile-scale stdout did not drain")
        child.stdout.close()
    output = stdout_path.read_bytes()
    errors = stderr_path.read_bytes()
    status = "PASS"
    if timed_out:
        status = "TIMEOUT"
    elif child.returncode != 0 or output != b"1\n" or errors or not first_output:
        status = "FAIL"
    return {"command": command, "status": status, "exit": child.returncode,
            "first_output_seconds": first_output[0] if first_output else None,
            "total_seconds": time.perf_counter() - started,
            "stdout": str(stdout_path), "stderr": str(stderr_path),
            "stdout_sha256": hashlib.sha256(output).hexdigest(),
            "stderr_sha256": hashlib.sha256(errors).hexdigest()}


def regex_compile_scale(args, root):
    """Compile independent, full regex source copies as ordinary user modules."""
    work = args.work / "compile-scale"
    work.mkdir()
    source = root / "stdlib/regex/regex.xr"
    original = source.read_bytes()
    text = original.decode("utf-8")
    if "__unicodePropId" not in text or "__unicodeHasProp" not in text:
        raise ValueError("The regex Unicode leaves changed. Update the compile fixture explicitly.")
    text = text.replace("__unicodePropId", "benchmarkUnicodePropId")
    text = text.replace("__unicodeHasProp", "benchmarkUnicodeHasProp")
    text += ("\n// Ordinary leaves for compilation-only inputs.\n"
             "fn benchmarkUnicodePropId(name: string) -> i64 { return len(name) }\n"
             "fn benchmarkUnicodeHasProp(cp: i64, propertyId: i64) -> bool { return cp == propertyId }\n")
    fixture = text.encode("utf-8")
    counts = (1, 2, 4, 8, 16)
    result = {"source": str(source), "source_sha256": hashlib.sha256(original).hexdigest(),
              "fixture_sha256": hashlib.sha256(fixture).hexdigest(),
              "fixture_lines": len(text.splitlines()), "rows": [], "doubling_ratios": {},
              "boundary": "Fresh process and unique module paths for every sample. Full regex "
                          "source with its two private Unicode leaves replaced by ordinary "
                          "compile-fixture functions. Modules never call each other. The entry "
                          "only prints 1. First-output observation includes process startup. "
                          "OS file cache is uncontrolled. Single-build feedback.",
              "single_copy_limit_seconds": 0.030, "doubling_limit": 2.2,
              "timeout_seconds": min(args.timeout, 30)}
    rows = {count: {"copies": count, "samples": [], "median_seconds": None} for count in counts}
    env = dict(os.environ, NO_COLOR="1", XRAY_STDLIB_PATH=str(root / "stdlib"))
    executable = str(tool(args.build, "xray"))
    for sample in range(args.samples):
        order = counts[sample % len(counts):] + counts[:sample % len(counts)]
        for count in order:
            row = rows[count]
            if any(value["status"] != "PASS" for value in row["samples"]):
                continue
            directory = work / f"sample-{sample}-copies-{count}"
            directory.mkdir()
            for index in range(count):
                (directory / f"regex_copy_{index}.xr").write_bytes(fixture)
            entry = directory / "main.xr"
            imports = "".join(f'import "./regex_copy_{index}" as copy{index}\n' for index in range(count))
            entry.write_text(imports + "print(1)\n", encoding="utf-8", newline="\n")
            value = compile_scale_command([executable, "run", str(entry)], env,
                                          directory / "run", result["timeout_seconds"])
            value["entry_sha256"] = hashlib.sha256(entry.read_bytes()).hexdigest()
            row["samples"].append(value)
            result["rows"] = list(rows.values())
            (work / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
            print(f"regex copies {count}, sample {sample}: {value['status']} "
                  f"first output {value['first_output_seconds']}", flush=True)
    measured = {}
    for count, row in rows.items():
        complete = len(row["samples"]) == args.samples and all(
            value["status"] == "PASS" for value in row["samples"])
        row["status"] = "COMPLETE" if complete else "INCOMPLETE"
        if complete:
            row["median_seconds"] = statistics.median(
                value["first_output_seconds"] for value in row["samples"])
            measured[count] = row["median_seconds"]
    for previous, current in zip(counts, counts[1:]):
        if previous in measured and current in measured:
            result["doubling_ratios"][f"{previous}->{current}"] = measured[current] / measured[previous]
    result["sample_gate"] = "PASS" if (
        len(measured) == len(counts) and measured[1] <= result["single_copy_limit_seconds"] and
        all(ratio <= result["doubling_limit"] for ratio in result["doubling_ratios"].values())) else "FAIL"
    result["rows"] = list(rows.values())
    (work / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=pathlib.Path, required=True)
    parser.add_argument("--build", type=pathlib.Path, required=True)
    parser.add_argument("--work", type=pathlib.Path, required=True)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--no-native", action="store_true", help="skip linking generated C (use on MSVC)")
    parser.add_argument("--no-bench", action="store_true")
    parser.add_argument("--no-census", action="store_true")
    parser.add_argument("--compile-scale", action="store_true",
                        help="measure 1/2/4/8/16 independent regex user modules against the M2 sample gate")
    parser.add_argument("--lua", type=pathlib.Path, help="Lua interpreter to compare against")
    parser.add_argument("--python", type=pathlib.Path, default=pathlib.Path(sys.executable),
                        help="CPython interpreter to compare against")
    parser.add_argument("--bench", action="append", choices=BENCH_LABELS,
                        help="run only this benchmark; repeat for multiple affected benchmarks")
    parser.add_argument("--include-carrier", action="store_true",
                        help="also time the archived execution path (off during the cutover)")
    args = parser.parse_args()
    if args.samples < 1 or args.timeout <= 0:
        parser.error("--samples and --timeout must be positive")
    args.root, args.build, args.work = (p.resolve() for p in (args.root, args.build, args.work))
    args.work.mkdir(parents=True, exist_ok=True)
    version = subprocess.run([str(tool(args.build, "xray")), "--version", "--json"], capture_output=True, text=True).stdout
    print("build:", version.strip())
    result = {"version": version.strip()}
    if not args.no_census:
        result["rows"] = census(args, args.root, load_smoke(args.root))
        report_census(result["rows"], args.no_native)
    if not args.no_bench:
        result["bench"], result["bench_details"] = bench(args, args.root)
    if args.compile_scale:
        result["compile_scale"] = regex_compile_scale(args, args.root)
    (args.work / "baseline.json").write_text(json.dumps(result, ensure_ascii=False, indent=1))
    print("\nfull result:", args.work / "baseline.json")
    if args.compile_scale and result["compile_scale"]["sample_gate"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
