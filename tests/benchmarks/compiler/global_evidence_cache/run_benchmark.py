#!/usr/bin/env python3
"""Compile-time benchmark for the global evidence cache.

Each case is compiled through a fixed sequence of phases and the compiler's own
`evidence cache summary:` line is asserted per phase:

    cold          nothing cached           hits=0 misses=4
    warm          everything cached        hits=4 misses=0
    dump          warm, plus a dump        hits=4 misses=0
    body_change   a body edited            hits=2 misses=2
    decl_change   a declaration added      hits=0 misses=4
    rebuild       --rebuild                hits=0 misses=4 rebuild

The hit/miss counts are the point, not the milliseconds: they say which
evidence survived which kind of edit. A body change must invalidate less than a
declaration change, and `--rebuild` must invalidate everything -- a cache that
quietly kept stale rows would still be fast, and would still be wrong. Timings
are recorded alongside for tracking, never asserted.

The `package` case additionally checks imported-package payloads: the package
row must be imported once and then re-used, never re-produced.

Environment:
    XRAY_GLOBAL_EVIDENCE_BENCH_CASES   comma list (default: all six)
    XRAY_GLOBAL_EVIDENCE_BENCH_REPEAT  iterations (default: 1)
    XRAY_GLOBAL_EVIDENCE_BENCH_KEEP    1 = keep the work directory
    XRAY_PACKAGE_PAYLOAD_FIXTURE       the package payload fixture binary

Usage: run_benchmark.py [xray]
"""

from __future__ import annotations

import json
import os
import re
import sys
import time
from pathlib import Path
from typing import Sequence


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[4] / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402

PROJECT_DIR = Path(__file__).resolve().parents[4]

ALL_CASES = ("class", "generic", "closure", "static", "capability", "package")

DECL_EXTRA = """
fn added_decl(x: int) -> int {
    return x + 1
}
"""

SUMMARY_RE = re.compile(r"evidence cache summary: (.*)")

CLASS_TEMPLATE = """interface Shape {{
    area() -> int
}}

class Circle implements Shape {{
    r: int
    constructor(r: int) {{
        this.r = r
    }}
    area() -> int {{
        return this.r * this.r + {body}
    }}
}}

class Square implements Shape {{
    side: int
    constructor(side: int) {{
        this.side = side
    }}
    area() -> int {{
        return this.side * this.side
    }}
}}

fn score(shape: Shape) -> int {{
    return shape.area()
}}

fn tag() -> int {{
    return {extra}
}}

{decl}
print(score(Circle(3)) + score(Square(4)))
"""

GENERIC_TEMPLATE = """class Box<T> {{
    value: T
    constructor(value: T) {{
        this.value = value
    }}
    get() -> T {{
        return this.value
    }}
}}

fn id<T>(x: T) -> T {{
    return x
}}

fn tag() -> int {{
    return {extra}
}}

fn wrapper() -> int {{
    var box = Box<int>(7)
    return id<int>(box.get()) + {body}
}}

{decl}
print(wrapper())
"""

CLOSURE_TEMPLATE = """fn tag() -> int {{
    return {extra}
}}

fn directClosure() -> int {{
    const f = fn() -> int {{
        return 41 + {body}
    }}
    return f() + 1
}}

fn captured(seed: int) -> int {{
    const f = fn() -> int {{
        return seed + 2
    }}
    return f()
}}

{decl}
print(directClosure() + captured(1))
"""

STATIC_TEMPLATE = """struct StaticPair {{
    left: int
    right: int
}}

fn tag() -> int {{
    return {extra}
}}

fn run() -> int {{
    const table: [int; 3] = comptime [1, 2, 3]
    const pair = comptime StaticPair{{left: table[0], right: 4}}
    const value = comptime 7
    return value + pair.right + {body}
}}

{decl}
print(run())
"""

CAPABILITY_TEMPLATE = """fn tag() -> int {{
    return {extra}
}}

fn worker(x: int) -> int {{
    return x + 1 + {body}
}}

fn sum(xs: Array<int>) -> int {{
    return xs.length
}}

shared ch: Channel<Array<int>> = Channel(1)

{decl}
var a = go worker(41)
var xs = [1, 2, 3]
var b = go sum(copy(xs))
var ys = [4, 5]
ch.send(move ys)

print(await a)
print(await b)
"""

PACKAGE_TEMPLATE = """import "codex/pkg" as pkg

fn tag() -> int {{
    return {extra}
}}

fn value() -> int {{
    return 7 + {body}
}}

{decl}
print(value())
"""

# (template, base body expression). Non-base variants always route the body
# through tag(), which is what makes a body edit observable to the cache.
TEMPLATES: dict[str, tuple] = {
    "class": (CLASS_TEMPLATE, None),
    "generic": (GENERIC_TEMPLATE, "0"),
    "closure": (CLOSURE_TEMPLATE, None),
    "static": (STATIC_TEMPLATE, "0"),
    "capability": (CAPABILITY_TEMPLATE, None),
    "package": (PACKAGE_TEMPLATE, None),
}

PACKAGE_SOURCE = "// Synthetic package represented entirely by package_payload_fixture.\n"


class Bench:
    def __init__(self, xray: Path, fixture: Path, results: Path) -> None:
        self.xray = xray
        self.fixture = fixture
        self.results = results
        self.failures = 0

    def fail(self, message: str, log_text: str = "") -> None:
        sys.stderr.write(f"FAIL: {message}\n")
        self.failures += 1
        for line in log_text.splitlines():
            if "evidence cache" in line:
                sys.stderr.write(line + "\n")

    def expect_contains(self, log_text: str, needle: str, case: str, phase: str,
                        description: str) -> bool:
        if needle in log_text:
            return True
        self.fail(f"{case}/{phase} expected {description}", log_text)
        return False

    def expect_not_contains(self, log_text: str, needle: str, case: str,
                            phase: str, description: str) -> bool:
        if needle not in log_text:
            return True
        self.fail(f"{case}/{phase} unexpected {description}", log_text)
        return False

    def record(self, case: str, phase: str, ms: int, summary: str,
               status: str) -> None:
        row = f"{case}\t{phase}\t{ms}\t{summary}\t{status}"
        # Flushed per row: the assertions that follow write to stderr, and a
        # block-buffered stdout would push every row past them, turning an
        # interleaved report into two disjoint halves.
        print(row, flush=True)
        with self.results.open("a", encoding="utf-8", newline="\n") as handle:
            handle.write(row + "\n")

    def run_phase(self, case: str, phase: str, source: Path, out: Path,
                  cache: Path, expected: str, extra_args: Sequence = (),
                  env: dict | None = None) -> str:
        """Compile one phase, record the row, and check the cache summary."""
        start = time.time()
        result = proc.run([self.xray, "build", "--native", "--verbose",
                           "--cache-dir", cache, *extra_args, "-o", out, source],
                          env=env)
        ms = int((time.time() - start) * 1000)
        log_text = result.combined_text()
        Path(str(out) + ".log").write_text(log_text, encoding="utf-8", newline="\n")

        matches = SUMMARY_RE.findall(log_text)
        summary = matches[-1] if matches else "missing"
        status = "pass" if result.ok else "fail"
        if not result.ok:
            self.failures += 1
        self.record(case, phase, ms, summary, status)

        if status == "pass" and expected:
            if f"evidence cache summary: {expected}" not in log_text:
                self.fail(f"{case}/{phase} expected evidence cache summary "
                          f"'{expected}'", log_text)
        return log_text


def write_case(case: str, path: Path, variant: str, extra: str) -> None:
    template, base_body = TEMPLATES[case]
    body = "tag()" if variant != "base" else (base_body
                                              if base_body is not None else extra)
    decl = DECL_EXTRA if variant == "decl" else ""
    path.write_text(template.format(extra=extra, body=body, decl=decl), encoding="utf-8", newline="\n")


def prepare_package_payload(bench: Bench, directory: Path, cache: Path) -> bool:
    """Materialize a synthetic installed package described only by its payload.

    The source is deliberately declaration-free so the payload is a complete
    description of the package: if the native producer had to repair missing
    declaration or storage evidence, the import path would be doing work the
    payload is supposed to have already done, and the hit-rate assertions would
    silently measure the wrong thing.
    """
    if not (bench.fixture.is_file() and os.access(bench.fixture, os.X_OK)):
        bench.fail(f"package payload fixture not executable: {bench.fixture}")
        return False

    home = directory / "home"
    pkg_dir = home / ".xray" / "packages" / "codex" / "pkg" / "1.0.0" / "src"
    pkg_dir.mkdir(parents=True, exist_ok=True)
    pkg_src = pkg_dir / "main.xr"
    pkg_src.write_text(PACKAGE_SOURCE, encoding="utf-8", newline="\n")

    # AOT cache namespaces use the normalized target ABI, including for a
    # native build. Ask the compiler under test rather than assuming a name.
    env = dict(os.environ)
    env["HOME"] = str(home)
    probe = proc.run([bench.xray, "toolchain", "probe", "--json"], env=env)
    target = ""
    if probe.ok:
        try:
            # request.normalizedTarget, not selection.targetAbi: the cache
            # namespace follows what was asked for, and the two can differ when
            # a provider falls back.
            target = json.loads(probe.stdout.decode("utf-8"))["request"][
                "normalizedTarget"]
        except (ValueError, KeyError):
            target = ""
    if not target:
        bench.fail("cannot resolve normalized native target for package payload "
                   "fixture")
        return False

    proc.run([bench.fixture, cache / "aot" / target, "codex/pkg",
              str(pkg_src.resolve()), bench.xray.resolve()])
    return True


def expect_package_phase(bench: Bench, log_text: str, phase: str, hits: int,
                         misses: int, materialized: int) -> None:
    bench.expect_contains(log_text, "evidence cache imported packages: discovered=1",
                          "package", phase, "package payload discovery")
    bench.expect_contains(
        log_text,
        f"evidence cache preproducer summary: request_hits={hits} "
        f"request_misses={misses} materialized={materialized}",
        "package", phase, "package preproducer hit-rate summary")


def run_package_case(bench: Bench, work: Path, iteration: int) -> None:
    directory = work / f"package-{iteration}"
    directory.mkdir(parents=True, exist_ok=True)
    source = directory / "main.xr"
    cache = directory / ".cache"
    home = directory / "home"

    if not prepare_package_payload(bench, directory, cache):
        return

    env = dict(os.environ)
    env["HOME"] = str(home)

    write_case("package", source, "base", "0")
    cold = bench.run_phase("package", "cold", source, directory / "cold", cache,
                           "hits=0 misses=4", env=env)
    expect_package_phase(bench, cold, "cold", 0, 4, 0)
    bench.expect_contains(cold, "evidence cache producer skip: imported_package_summary",
                          "package", "cold", "package row import")
    bench.expect_not_contains(cold,
                              "evidence cache producer skip: global_evidence_summary",
                              "package", "cold", "global producer skip")

    warm = bench.run_phase("package", "package_import_warm", source,
                           directory / "warm", cache, "hits=4 misses=0", env=env)
    expect_package_phase(bench, warm, "package_import_warm", 4, 0, 4)
    bench.expect_contains(warm, "evidence cache producer skip: pre_mono_generic_summary",
                          "package", "package_import_warm", "pre-mono producer skip")
    bench.expect_contains(warm, "evidence cache producer skip: global_evidence_summary",
                          "package", "package_import_warm", "global producer skip")
    bench.expect_not_contains(warm,
                              "evidence cache producer skip: imported_package_summary",
                              "package", "package_import_warm", "package row re-import")

    dump = bench.run_phase("package", "package_import_dump", source,
                           directory / "dump", cache, "hits=4 misses=0",
                           ["--dump-global-evidence"], env=env)
    expect_package_phase(bench, dump, "package_import_dump", 4, 0, 4)
    bench.expect_contains(dump, "evidence cache producer skip: global_evidence_summary",
                          "package", "package_import_dump",
                          "dump warm global producer skip")

    write_case("package", source, "body", "1")
    body = bench.run_phase("package", "package_consumer_body_change", source,
                           directory / "body", cache, "hits=2 misses=2", env=env)
    expect_package_phase(bench, body, "package_consumer_body_change", 0, 4, 0)
    bench.expect_contains(body, "evidence cache producer skip: imported_package_summary",
                          "package", "package_consumer_body_change",
                          "package row import after consumer body change")


def run_one_case(bench: Bench, case: str, work: Path, iteration: int) -> None:
    if case == "package":
        run_package_case(bench, work, iteration)
        return

    directory = work / f"{case}-{iteration}"
    directory.mkdir(parents=True, exist_ok=True)
    source = directory / "main.xr"
    cache = directory / ".cache"

    write_case(case, source, "base", "0")
    bench.run_phase(case, "cold", source, directory / "cold", cache,
                    "hits=0 misses=4")
    bench.run_phase(case, "warm", source, directory / "warm", cache,
                    "hits=4 misses=0")
    bench.run_phase(case, "dump", source, directory / "dump", cache,
                    "hits=4 misses=0", ["--dump-global-evidence"])

    write_case(case, source, "body", "1")
    bench.run_phase(case, "body_change", source, directory / "body", cache,
                    "hits=2 misses=2")

    write_case(case, source, "decl", "2")
    bench.run_phase(case, "decl_change", source, directory / "decl", cache,
                    "hits=0 misses=4")
    bench.run_phase(case, "rebuild", source, directory / "rebuild", cache,
                    "hits=0 misses=4 rebuild", ["--rebuild"])


def main(argv: list[str]) -> int:
    xray = Path(argv[1] if len(argv) > 1
                else os.environ.get("XRAY_BIN", str(PROJECT_DIR / "build" / "xray")))
    fixture = Path(os.environ.get(
        "XRAY_PACKAGE_PAYLOAD_FIXTURE",
        str(PROJECT_DIR / "build" / "xray_package_payload_fixture")))
    cases = [c for c in os.environ.get("XRAY_GLOBAL_EVIDENCE_BENCH_CASES",
                                       ",".join(ALL_CASES)).split(",") if c]
    repeat = platform.env_int("XRAY_GLOBAL_EVIDENCE_BENCH_REPEAT", 1)
    if repeat < 1:
        repeat = 1
    keep = platform.env_flag("XRAY_GLOBAL_EVIDENCE_BENCH_KEEP")

    if not (xray.is_file() and os.access(xray, os.X_OK)):
        sys.stderr.write(f"FAIL: xray binary not executable: {xray}\n")
        return 1

    unknown = [c for c in cases if c not in TEMPLATES]
    if unknown:
        sys.stderr.write(f"FAIL: unknown case '{unknown[0]}'\n")
        return 1

    # A failing run keeps its workdir regardless: the phase logs are the only
    # place the cache decisions are visible after the fact.
    ws = workspace.Workspace("xray_global_evidence_cache_bench", keep=True)
    with ws:
        results = ws.path("results.tsv")
        header = "case\tphase\telapsed_ms\tevidence_cache\tstatus"
        print(header, flush=True)
        results.write_text(header + "\n", encoding="utf-8", newline="\n")

        bench = Bench(xray, fixture, results)
        for iteration in range(1, repeat + 1):
            for case in cases:
                run_one_case(bench, case, ws.root, iteration)

        if bench.failures == 0:
            sys.stderr.write(f"Benchmark results: {results}\n")
            ws.keep_on_exit(keep)
            return 0
        sys.stderr.write(f"Benchmark failed with {bench.failures} issue(s). "
                         f"Workdir: {ws.root}\n")
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
