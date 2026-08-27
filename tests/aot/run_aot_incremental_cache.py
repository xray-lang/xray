#!/usr/bin/env python3
"""Incremental AOT cache: what gets reused, what gets rebuilt, and why.

Every scenario is a sequence of steps that edit sources and rebuild, asserting
the cache's decision at each point. Two failure modes matter and they are not
symmetric: a missed *recompile* ships a stale binary, while a missed *hit* only
wastes time. So the states are asserted explicitly, per module, per step --
never inferred from a build succeeding.

Four scenarios:
  - basic-modules: body change, added export, entry change, --rebuild
  - evidence-manifest-cache: the four evidence phases, including deliberate
    sidecar and payload corruption to prove tampering forces a miss
  - class-symbols: a method nobody calls must NOT invalidate a closed-world build
  - lto-cache: cold/warm under --lto

Replaces run_aot_incremental_cache.sh.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Sequence


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent

SKIP_EXIT = 77

EVIDENCE_PHASES = ("declarations", "semantic_graph", "body_summary", "global_evidence")


class Recorder:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def ok(self, name: str) -> None:
        print(f"  PASS: {name}")
        self.passed += 1

    def bad(self, name: str, detail: str = "") -> None:
        print(f"  FAIL: {name}")
        self.failed += 1
        for line in detail.splitlines()[:40]:
            print(f"      {line}")


@dataclass
class Config:
    xray: Path
    opt_level: str
    cache_target: str
    timeout: float | None


def build(config: Config, cache: Path, entry: Path, out: Path,
          extra: Sequence = ()) -> proc.ProcResult:
    """One incremental build. --verbose is what emits the cache decisions."""
    return proc.run(
        [config.xray, "build", "--native", "-O", config.opt_level, "--verbose",
         "--cache-dir", cache, *extra, "-o", out, entry],
        timeout=config.timeout,
    )


def cache_lines(log: str) -> str:
    return "\n".join(ln for ln in log.splitlines()
                     if "compiling" in ln or "cache hit" in ln)


def evidence_lines(log: str) -> str:
    return "\n".join(ln for ln in log.splitlines() if "evidence cache" in ln)


def expect_state(rec: Recorder, log: str, module: str, want: str, label: str) -> None:
    """Assert a module recompiled or hit the cache.

    Cache units are named `<module>` plus a canonical-identity hash, so the
    match is a pattern; a literal name would go stale the moment hashing
    changed (the failure this task already found in the debug smoke).
    """
    unit = rf"{re.escape(module)}(_[0-9a-fA-F]+)?"
    if want == "compiling":
        if re.search(rf"compiling: {unit} ", log):
            rec.ok(f"{label}: {module} recompiled")
        else:
            rec.bad(f"{label}: expected {module} to recompile", cache_lines(log))
    else:
        if re.search(rf"cache hit: {unit} ", log):
            rec.ok(f"{label}: {module} cache hit")
        else:
            rec.bad(f"{label}: expected {module} cache hit", cache_lines(log))


def expect_output(rec: Recorder, config: Config, binary: Path, want: str,
                  label: str) -> None:
    result = proc.run([binary], timeout=config.timeout)
    got = result.stdout.decode("utf-8", "replace").strip()
    if got == want:
        rec.ok(f"{label}: output {want!r}")
    else:
        rec.bad(f"{label}: output {got!r} != {want!r}")


def expect_evidence_summary(rec: Recorder, log: str, hits: int, misses: int,
                            label: str, suffix: str = "") -> None:
    pattern = f"evidence cache summary: hits={hits} misses={misses}{suffix}"
    if pattern in log:
        rec.ok(f"{label}: {pattern}")
    else:
        rec.bad(f"{label}: expected {pattern}", evidence_lines(log))


def expect_preproducer_summary(rec: Recorder, log: str, hits: int, misses: int,
                               materialized: int, label: str, suffix: str = "") -> None:
    pattern = (f"evidence cache preproducer summary: request_hits={hits} "
               f"request_misses={misses} materialized={materialized}{suffix}")
    if pattern in log:
        rec.ok(f"{label}: {pattern}")
    else:
        rec.bad(f"{label}: expected {pattern}", evidence_lines(log))


def expect_producer_skip(rec: Recorder, log: str, producer: str, label: str) -> None:
    pattern = f"evidence cache producer skip: {producer}"
    if pattern in log:
        rec.ok(f"{label}: {pattern}")
    else:
        rec.bad(f"{label}: expected {pattern}", evidence_lines(log))


def expect_no_producer_skip(rec: Recorder, log: str, label: str) -> None:
    if "evidence cache producer skip:" in log:
        rec.bad(f"{label}: unexpected evidence producer skip", evidence_lines(log))
    else:
        rec.ok(f"{label}: no evidence producer skip")


def expect_phase(rec: Recorder, log: str, phase: str, state: str, label: str) -> None:
    pattern = f"evidence cache {phase}: {state} "
    if pattern in log:
        rec.ok(f"{label}: {phase} {state}")
    else:
        rec.bad(f"{label}: expected {phase} {state}", evidence_lines(log))


def phase_dir(config: Config, cache: Path, phase: str) -> Path:
    return cache / "aot" / config.cache_target / "evidence" / phase


def corrupt_phase(rec: Recorder, config: Config, cache: Path, phase: str,
                  label: str, *, payload: bool = False) -> None:
    """Tamper with one cached sidecar so the next build must treat it as a miss.

    Requiring exactly one file is deliberate: if a phase ever holds several,
    corrupting an arbitrary one would make the following hit/miss assertions
    depend on which was picked.
    """
    suffix = "*.xgpayload" if payload else "*.xgcache"
    kind = "payload" if payload else "sidecar"
    files = sorted(phase_dir(config, cache, phase).glob(suffix))
    if len(files) == 1:
        files[0].write_text(f"tampered-{'payload' if payload else 'cache'}\n",
                            encoding="utf-8")
        rec.ok(f"{label}: corrupted {phase} {kind}")
    else:
        rec.bad(f"{label}: expected one {phase} {kind}, found {len(files)}")


def expect_sidecars(rec: Recorder, config: Config, cache: Path, label: str) -> None:
    """Each phase must persist exactly one manifest and one payload, and the
    payload must carry both the v2 payload header and the v1 request header."""
    for phase in EVIDENCE_PHASES:
        directory = phase_dir(config, cache, phase)
        manifests = sorted(directory.glob("*.xgcache"))
        payloads = sorted(directory.glob("*.xgpayload"))
        good = len(manifests) == 1 and len(payloads) == 1
        if good:
            text = payloads[0].read_text(encoding="utf-8")
            good = ("xg-cache-payload v2 " in text and "xg-cache-request v1 " in text)
        if good:
            rec.ok(f"{label}: {phase} manifest+payload sidecars")
        else:
            rec.bad(f"{label}: expected {phase} manifest+payload sidecars")


def require_build(rec: Recorder, result: proc.ProcResult, label: str) -> bool:
    if result.ok:
        return True
    rec.bad(f"{label}: build failed", result.combined_text())
    return False


# --- scenarios --------------------------------------------------------------


def run_basic_modules(rec: Recorder, config: Config, ws: workspace.Workspace) -> None:
    print("--- basic-modules ---")
    d = ws.subdir("basic")
    cache = d / ".cache"
    app, lib = d / "app.xr", d / "mathlib.xr"

    app.write_text('import { triple } from "./mathlib"\n\nprint(triple(14))\n',
                   encoding="utf-8")
    lib.write_text("export fn triple(x: i64) -> i64 {\n    return x * 3\n}\n",
                   encoding="utf-8")
    r = build(config, cache, app, d / "app1")
    if not require_build(rec, r, "cold"):
        return
    expect_state(rec, r.combined_text(), "mathlib", "compiling", "cold")
    expect_state(rec, r.combined_text(), "app", "compiling", "cold")
    expect_output(rec, config, d / "app1", "42", "cold")

    # Body-only change: the library recompiles, the consumer does not.
    lib.write_text("export fn triple(x: i64) -> i64 {\n    return x * 5\n}\n",
                   encoding="utf-8")
    r = build(config, cache, app, d / "app2")
    if not require_build(rec, r, "body-change"):
        return
    expect_state(rec, r.combined_text(), "mathlib", "compiling", "body-change")
    expect_state(rec, r.combined_text(), "app", "hit", "body-change")
    expect_output(rec, config, d / "app2", "70", "body-change")

    lib.write_text("export fn triple(x: i64) -> i64 {\n    return x * 5\n}\n\n"
                   "export fn quad(x: i64) -> i64 {\n    return x * 4\n}\n",
                   encoding="utf-8")
    r = build(config, cache, app, d / "app3")
    if not require_build(rec, r, "add-export"):
        return
    expect_state(rec, r.combined_text(), "mathlib", "compiling", "add-export")
    expect_state(rec, r.combined_text(), "app", "hit", "add-export")
    expect_output(rec, config, d / "app3", "70", "add-export")

    app.write_text('import { triple } from "./mathlib"\n\n'
                   "print(triple(14))\nprint(triple(2))\n", encoding="utf-8")
    r = build(config, cache, app, d / "app4")
    if not require_build(rec, r, "entry-only"):
        return
    expect_state(rec, r.combined_text(), "mathlib", "hit", "entry-only")
    expect_state(rec, r.combined_text(), "app", "compiling", "entry-only")
    expect_output(rec, config, d / "app4", "70\n10", "entry-only")

    r = build(config, cache, app, d / "app5", ["--rebuild"])
    if not require_build(rec, r, "rebuild"):
        return
    expect_state(rec, r.combined_text(), "mathlib", "compiling", "rebuild")
    expect_state(rec, r.combined_text(), "app", "compiling", "rebuild")
    expect_output(rec, config, d / "app5", "70\n10", "rebuild")


def run_evidence_manifest_cache(rec: Recorder, config: Config,
                                ws: workspace.Workspace) -> None:
    print("--- evidence-manifest-cache ---")
    d = ws.subdir("evidence")
    cache = d / ".cache"
    app = d / "evidence.xr"

    app.write_text("fn id(x: i64) -> i64 {\n    return x\n}\n\nprint(id(7))\n",
                   encoding="utf-8")
    r = build(config, cache, app, d / "ev1")
    if not require_build(rec, r, "evidence-cold"):
        return
    log = r.combined_text()
    expect_preproducer_summary(rec, log, 0, 4, 0, "evidence-cold")
    expect_no_producer_skip(rec, log, "evidence-cold")
    expect_evidence_summary(rec, log, 0, 4, "evidence-cold")
    expect_sidecars(rec, config, cache, "evidence-cold")
    expect_output(rec, config, d / "ev1", "7", "evidence-cold")

    r = build(config, cache, app, d / "ev2")
    if not require_build(rec, r, "evidence-warm"):
        return
    log = r.combined_text()
    expect_preproducer_summary(rec, log, 4, 0, 4, "evidence-warm")
    expect_producer_skip(rec, log, "pre_mono_generic_summary", "evidence-warm")
    expect_producer_skip(rec, log, "global_evidence_summary", "evidence-warm")
    expect_evidence_summary(rec, log, 4, 0, "evidence-warm")
    expect_output(rec, config, d / "ev2", "7", "evidence-warm")

    # A tampered manifest must force exactly its own phase to miss.
    corrupt_phase(rec, config, cache, "body_summary", "evidence-corrupt")
    r = build(config, cache, app, d / "ev_corrupt")
    if not require_build(rec, r, "evidence-corrupt"):
        return
    log = r.combined_text()
    expect_phase(rec, log, "declarations", "hit", "evidence-corrupt")
    expect_phase(rec, log, "semantic_graph", "hit", "evidence-corrupt")
    expect_phase(rec, log, "body_summary", "miss", "evidence-corrupt")
    expect_phase(rec, log, "global_evidence", "hit", "evidence-corrupt")
    expect_evidence_summary(rec, log, 3, 1, "evidence-corrupt")
    expect_output(rec, config, d / "ev_corrupt", "7", "evidence-corrupt")

    # Same for a tampered payload, which is validated separately.
    corrupt_phase(rec, config, cache, "global_evidence", "evidence-payload-corrupt",
                  payload=True)
    r = build(config, cache, app, d / "ev_payload_corrupt")
    if not require_build(rec, r, "evidence-payload-corrupt"):
        return
    log = r.combined_text()
    expect_preproducer_summary(rec, log, 3, 1, 3, "evidence-payload-corrupt")
    expect_no_producer_skip(rec, log, "evidence-payload-corrupt")
    expect_phase(rec, log, "declarations", "hit", "evidence-payload-corrupt")
    expect_phase(rec, log, "semantic_graph", "hit", "evidence-payload-corrupt")
    expect_phase(rec, log, "body_summary", "hit", "evidence-payload-corrupt")
    expect_phase(rec, log, "global_evidence", "miss", "evidence-payload-corrupt")
    expect_evidence_summary(rec, log, 3, 1, "evidence-payload-corrupt")
    expect_output(rec, config, d / "ev_payload_corrupt", "7", "evidence-payload-corrupt")

    # A body edit keeps declarations/graph but invalidates body and global.
    app.write_text("fn id(x: i64) -> i64 {\n    return x + 1\n}\n\nprint(id(7))\n",
                   encoding="utf-8")
    r = build(config, cache, app, d / "ev3")
    if not require_build(rec, r, "evidence-body-change"):
        return
    log = r.combined_text()
    expect_preproducer_summary(rec, log, 0, 4, 0, "evidence-body-change")
    expect_no_producer_skip(rec, log, "evidence-body-change")
    expect_phase(rec, log, "declarations", "hit", "evidence-body-change")
    expect_phase(rec, log, "semantic_graph", "hit", "evidence-body-change")
    expect_phase(rec, log, "body_summary", "miss", "evidence-body-change")
    expect_phase(rec, log, "global_evidence", "miss", "evidence-body-change")
    expect_evidence_summary(rec, log, 2, 2, "evidence-body-change")
    expect_output(rec, config, d / "ev3", "8", "evidence-body-change")

    # A signature change invalidates every phase.
    app.write_text("fn id(x: i64, y: i64) -> i64 {\n    return x + y\n}\n\n"
                   "print(id(7, 2))\n", encoding="utf-8")
    r = build(config, cache, app, d / "ev4")
    if not require_build(rec, r, "evidence-decl-change"):
        return
    log = r.combined_text()
    for phase in EVIDENCE_PHASES:
        expect_phase(rec, log, phase, "miss", "evidence-decl-change")
    expect_evidence_summary(rec, log, 0, 4, "evidence-decl-change")
    expect_output(rec, config, d / "ev4", "9", "evidence-decl-change")

    r = build(config, cache, app, d / "ev5", ["--rebuild"])
    if not require_build(rec, r, "evidence-rebuild"):
        return
    log = r.combined_text()
    expect_preproducer_summary(rec, log, 0, 4, 0, "evidence-rebuild", " rebuild")
    expect_no_producer_skip(rec, log, "evidence-rebuild")
    expect_evidence_summary(rec, log, 0, 4, "evidence-rebuild", " rebuild")
    expect_output(rec, config, d / "ev5", "9", "evidence-rebuild")


def run_class_symbols(rec: Recorder, config: Config, ws: workspace.Workspace) -> None:
    print("--- class-symbols ---")
    d = ws.subdir("class")
    cache = d / ".cache"
    app, lib = d / "capp.xr", d / "shape.xr"

    app.write_text('import { Box } from "./shape"\n\nvar b = Box(4)\nprint(b.area())\n',
                   encoding="utf-8")
    lib.write_text("""export class Box {
    side: i64
    constructor(s: i64) {
        this.side = s
    }
    area() -> i64 {
        return this.side * this.side
    }
}
""", encoding="utf-8")
    r = build(config, cache, app, d / "capp1")
    if not require_build(rec, r, "class-cold"):
        return
    expect_state(rec, r.combined_text(), "shape", "compiling", "class-cold")
    expect_state(rec, r.combined_text(), "capp", "compiling", "class-cold")
    expect_output(rec, config, d / "capp1", "16", "class-cold")

    # An executable is closed-world, so a method no reachable code calls is not
    # emitted and shape's object is byte-identical: a hit, not a recompile. The
    # cache stays sound because the key covers what consumers actually need --
    # making the app call perimeter() does recompile shape and links.
    lib.write_text("""export class Box {
    side: i64
    constructor(s: i64) {
        this.side = s
    }
    area() -> i64 {
        return this.side * this.side
    }
    perimeter() -> i64 {
        return this.side * 4
    }
}
""", encoding="utf-8")
    r = build(config, cache, app, d / "capp2")
    if not require_build(rec, r, "class-add-method"):
        return
    expect_state(rec, r.combined_text(), "shape", "hit", "class-add-method")
    expect_state(rec, r.combined_text(), "capp", "hit", "class-add-method")
    expect_output(rec, config, d / "capp2", "16", "class-add-method")


def run_lto_cache(rec: Recorder, config: Config, ws: workspace.Workspace) -> None:
    print("--- lto-cache ---")
    d = ws.subdir("lto")
    cache = d / ".cache"
    app, lib = d / "app.xr", d / "mathlib.xr"

    app.write_text('import { triple } from "./mathlib"\n\n'
                   "print(triple(14))\nprint(triple(2))\n", encoding="utf-8")
    lib.write_text("export fn triple(x: i64) -> i64 {\n    return x * 5\n}\n\n"
                   "export fn quad(x: i64) -> i64 {\n    return x * 4\n}\n",
                   encoding="utf-8")
    r = build(config, cache, app, d / "lapp1", ["--lto"])
    if not require_build(rec, r, "lto-cold"):
        return
    expect_state(rec, r.combined_text(), "mathlib", "compiling", "lto-cold")
    expect_output(rec, config, d / "lapp1", "70\n10", "lto-cold")

    r = build(config, cache, app, d / "lapp2", ["--lto"])
    if not require_build(rec, r, "lto-warm"):
        return
    expect_state(rec, r.combined_text(), "mathlib", "hit", "lto-warm")
    expect_state(rec, r.combined_text(), "app", "hit", "lto-warm")
    expect_output(rec, config, d / "lapp2", "70\n10", "lto-warm")


def digest_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expect(rec: Recorder, condition: bool, name: str, detail: str = "") -> None:
    if condition:
        rec.ok(name)
    else:
        rec.bad(name, detail)


def build_quiet(config: Config, cache: Path, entry: Path, out: Path) -> proc.ProcResult:
    """One build on the product's default path.

    Every other scenario passes --verbose to read the cache decisions, and
    --verbose is exactly what disables the link output cache. Nothing reaches
    that cache without a build that stays quiet.
    """
    return proc.run(
        [config.xray, "build", "--native", "-O", config.opt_level,
         "--cache-dir", cache, "-o", out, entry],
        timeout=config.timeout,
    )


def code_signature_identifier(path: Path, timeout: float | None) -> str | None:
    """The Mach-O ad-hoc signing identifier, or None where there is no signing."""
    result = proc.run(["codesign", "-dv", str(path)], timeout=timeout)
    match = re.search(r"(?m)^Identifier=(.+)$", result.combined_text())
    return match.group(1).strip() if match else None


def run_link_output_identity(rec: Recorder, config: Config,
                             ws: workspace.Workspace) -> None:
    """A warm build must not hand one output the binary built for another.

    The link output cache is keyed by everything reaching the compiler and the
    linker except the output the linker was asked to produce. Where the output
    name reaches the artifact -- Mach-O ad-hoc signing takes its identifier
    from it -- the second output is served the first one's bytes.
    """
    print("--- link-output-identity ---")
    d = ws.subdir("link-output")
    app = d / "app.xr"
    app.write_text("fn main() -> i64 { return 42 }\n", encoding="utf-8")

    first = d / "out" / platform.exe_name("first_program")
    second = d / "out" / platform.exe_name("second_program")
    first.parent.mkdir(parents=True, exist_ok=True)

    # Two builds that share nothing establish whether this platform and this
    # toolchain distinguish the two outputs at all.
    cold_first = build_quiet(config, d / "cache-a", app, first)
    cold_second = build_quiet(config, d / "cache-b", app, second)
    if not require_build(rec, cold_first, "link-output-cold-first"):
        return
    if not require_build(rec, cold_second, "link-output-cold-second"):
        return
    distinguishable = digest_file(first) != digest_file(second)

    shared = d / "cache-shared"
    primed = build_quiet(config, shared, app, first)
    served = build_quiet(config, shared, app, second)
    if not require_build(rec, primed, "link-output-prime"):
        return
    if not require_build(rec, served, "link-output-warm"):
        return
    primed_digest = digest_file(first)
    served_digest = digest_file(second)

    if distinguishable:
        expect(rec, served_digest != primed_digest,
               "link-output: a warm output is not the previous output's binary",
               f"primed={primed_digest}\nserved={served_digest}")
    else:
        rec.ok("link-output: outputs are indistinguishable on this toolchain")

    # Where the platform signs, the identifier names the artifact directly and
    # says which output the served bytes were actually built for.
    identifier = code_signature_identifier(second, config.timeout)
    if identifier is None:
        rec.ok("link-output: no code signature to attribute on this platform")
        return
    expect(rec, identifier == second.name,
           "link-output: the served binary is signed as the output it names",
           f"want={second.name}\ngot={identifier}")


SCENARIOS: dict[str, Callable] = {
    "basic": run_basic_modules,
    "evidence": run_evidence_manifest_cache,
    "class": run_class_symbols,
    "lto": run_lto_cache,
    "link-output": run_link_output_identity,
}


def resolve_cache_target(config_xray: Path, timeout: float | None) -> str | None:
    """The normalized native target that names the on-disk cache directory."""
    result = proc.run([config_xray, "toolchain", "list", "--target", "native", "--json"],
                      timeout=timeout)
    if not result.ok:
        return None
    match = re.search(r'"normalizedTarget"\s*:\s*"([^"]+)"',
                      result.stdout.decode("utf-8", "replace"))
    return match.group(1) if match else None


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Incremental AOT cache suite")
    ap.add_argument("--xray", default=None)
    ap.add_argument("xray_positional", nargs="?", default=None)
    ns = ap.parse_args(argv[1:])

    xray_raw = ns.xray or ns.xray_positional or os.environ.get("XRAY_BIN")
    if not xray_raw:
        build_dir = os.environ.get("XRAY_BUILD_DIR")
        xray_raw = str(Path(build_dir) / "xray") if build_dir else str(PROJECT_DIR / "build" / "xray")
    xray = Path(xray_raw)
    opt_level = os.environ.get("XRAY_AOT_TEST_OPT", "0")
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 600)

    print("=== AOT Incremental Cache ===")
    print(f"Binary: {xray}")
    print(f"AOT opt: -O{opt_level}")

    if not (xray.is_file() and os.access(xray, os.X_OK)):
        print(f"FAIL: xray binary not executable: {xray}")
        return 1

    cache_target = resolve_cache_target(xray, timeout)
    if not cache_target:
        print("FAIL: cannot resolve normalized native AOT cache target")
        return 1
    print(f"Target: {cache_target}")
    print("")

    config = Config(xray=xray, opt_level=opt_level, cache_target=cache_target,
                    timeout=timeout)
    rec = Recorder()

    # Scenarios share no state and each owns its cache directory, but they are
    # run in order: the output is meant to be read top to bottom when a cache
    # decision regresses.
    with workspace.Workspace("xray_aot_incremental") as ws:
        for name, runner in SCENARIOS.items():
            runner(rec, config, ws)
            print("")

    print(f"=== Results: {rec.passed} passed, {rec.failed} failed ===")
    return 1 if rec.failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
