#!/usr/bin/env python3
"""Module summary determinism: the same source must yield the same identity.

The native build derives one module summary per module, publishes the resulting
dependency graph into the compiler session, and round-trips each module's XSM
artifact through the incremental cache. Two properties are asserted here, and
they are the ones a cache can silently break:

  - Determinism. A cold build and a warm build of identical sources publish the
    identical dependency-graph fingerprint. `--rebuild` recomputes everything
    and still reproduces it, so the identity depends on content alone and never
    on what happened to be cached.
  - Change detection. Editing a module moves the graph fingerprint and costs
    that module its cache hit; reverting restores both. An unchanged
    fingerprint can therefore never be the result of a build that failed to
    look.

A cache hit skips only the re-encoding and re-publication of an artifact
already proven identical: the verifier accepts a candidate only when it decodes
to a verified plan whose fingerprint equals the plan this build just produced.
Every module runs the full pipeline before the cache is consulted at all, so a
hit can never stand in for a proof the build owes.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402

PROJECT_DIR = Path(__file__).resolve().parents[2]

SUMMARY_RE = re.compile(
    r"\[module-summary\] modules=(\d+) graph=([0-9a-f]{64}) "
    r"xsm-hits=(\d+) xsm-published=(\d+) xsm-missed=(\d+) "
    r"workers=(\d+) tasks=(\d+) xsm-recomputed=(\d+) "
    r"xsm-artifacts=([0-9a-f]{64}) "
    r"xsm-publish-order=([0-9a-f]{64})"
)
PROGRAM_RE = re.compile(
    r"\[program-closure\] modules=(\d+) dependencies=(\d+) "
    r"psc=([0-9a-f]{64}) gci=([0-9a-f]{32})"
)
PROGRAM_PLAN_GATE = (
    "[program-semantic-plan] modules=2 entry=1 xi=verified "
    "semantic=verified target=verified execution=pending"
)
GRAPH_TARGET_REJECTION = (
    "XR_TARGET_1001: verified program TargetPlan execution is outside "
    "same-plan VM/AOT coverage"
)

SOLO_V1 = "fn scale(x: i64) -> i64 {\n    return x * 3\n}\n\nprint(scale(14))\n"
SOLO_V2 = "fn scale(x: i64) -> i64 {\n    return x * 5\n}\n\nprint(scale(14))\n"
LEAF_V1 = "export fn scale(x: i64) -> i64 {\n    return x * 3\n}\n"
LEAF_V2 = "export fn scale(x: i64) -> i64 {\n    return x * 5\n}\n"
MIDDLE = ('import { scale } from "./leaf"\n\n'
          "export fn twice(x: i64) -> i64 {\n    return scale(x) + scale(x)\n}\n")
APP = 'import { twice } from "./middle"\n\nprint(twice(7))\n'
PRODUCT_PRODUCER_V1 = (
    "export fn add1(value: i64) -> i64 { return value + 1 }\n"
)
PRODUCT_PRODUCER_V2 = (
    "export fn add1(value: i64) -> i64 { return value + 2 }\n"
)
PRODUCT_ENTRY = (
    'import { add1 } from "./producer"\n'
    "fn root() -> i64 { return add1(41) }\n"
)


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
class Summary:
    modules: int
    graph: str
    hits: int
    published: int
    missed: int
    workers: int
    tasks: int
    recomputed: int
    artifacts: str
    publish_order: str


@dataclass
class ProgramClosure:
    modules: int
    dependencies: int
    fingerprint: str
    generation_id: str


@dataclass
class Config:
    xray: Path
    opt_level: str
    timeout: float | None


def build(config: Config, cache: Path, entry: Path, out: Path,
          extra: Sequence = ()) -> proc.ProcResult:
    return proc.run(
        [config.xray, "build", "--native", "-O", config.opt_level, "--verbose",
         "--cache-dir", cache, *extra, "-o", out, entry],
        timeout=config.timeout,
    )


def parse_summary(rec: Recorder, result: proc.ProcResult, label: str,
                  require_build: bool = True) -> Summary | None:
    log = result.combined_text()
    if require_build and not result.ok:
        rec.bad(f"{label}: build failed", log)
        return None
    match = SUMMARY_RE.search(log)
    if not match:
        rec.bad(f"{label}: no [module-summary] report", log)
        return None
    return Summary(int(match.group(1)), match.group(2), int(match.group(3)),
                   int(match.group(4)), int(match.group(5)),
                   int(match.group(6)), int(match.group(7)),
                   int(match.group(8)), match.group(9), match.group(10))


def parse_program_closure(rec: Recorder, result: proc.ProcResult,
                          label: str) -> ProgramClosure | None:
    match = PROGRAM_RE.search(result.combined_text())
    if not match:
        rec.bad(f"{label}: no [program-closure] report", result.combined_text())
        return None
    return ProgramClosure(int(match.group(1)), int(match.group(2)),
                          match.group(3), match.group(4))


def xsm_inventory(cache: Path) -> dict[str, str]:
    return {
        str(path.relative_to(cache)): hashlib.sha256(path.read_bytes()).hexdigest()
        for root in sorted(cache.rglob("xsm"))
        if root.is_dir()
        for path in sorted(root.iterdir())
        if path.is_file() and not path.name.startswith(".")
    }


def expect(rec: Recorder, condition: bool, name: str, detail: str = "") -> None:
    if condition:
        rec.ok(name)
    else:
        rec.bad(name, detail)


def expect_output(rec: Recorder, config: Config, binary: Path, want: str, label: str) -> None:
    result = proc.run([binary], timeout=config.timeout)
    got = result.stdout.decode("utf-8", "replace").strip()
    expect(rec, got == want, f"{label}: output {want!r}", f"got {got!r}")


def expect_product_target_boundary(rec: Recorder, result: proc.ProcResult,
                                   output: Path, label: str) -> None:
    stdout = result.stdout_text("replace")
    stderr = result.stderr_text("replace")
    gate_at = stdout.find(PROGRAM_PLAN_GATE)
    summary_at = stdout.find("[module-summary]")
    closure_at = stdout.find("[program-closure]")
    forbidden = ("legacy success", "legacy fallback", "fallback succeeded")
    expect(rec, not result.ok and not result.timed_out and GRAPH_TARGET_REJECTION in stderr,
           f"{label}: verified graph reaches the exact Target fail-closed boundary",
           result.combined_text())
    expect(rec, 0 <= gate_at < summary_at < closure_at,
           f"{label}: verified program target precedes cache publication and execution fence",
           stdout)
    expect(rec, not output.exists(),
           f"{label}: Target rejection publishes no native binary", str(output))
    expect(rec, all(marker not in result.combined_text().lower() for marker in forbidden),
           f"{label}: Target rejection has no legacy recovery", result.combined_text())


def run_determinism(rec: Recorder, config: Config, ws: workspace.Workspace) -> None:
    print("--- cold, warm, and rebuild publish one identity ---")
    d = ws.subdir("determinism")
    cache = d / ".cache"
    app = d / "solo.xr"
    app.write_text(SOLO_V1, encoding="utf-8")

    cold = parse_summary(rec, build(config, cache, app, d / "app1"), "cold")
    if not cold:
        return
    expect(rec, cold.hits == 0 and cold.published == cold.modules,
           "cold: every module publishes its artifact", str(cold))
    expect(rec, cold.recomputed == cold.modules and cold.tasks > 0,
           "cold: exact module tasks report every recomputed artifact", str(cold))
    cold_inventory = xsm_inventory(cache)
    expect(rec, len(cold_inventory) == cold.modules,
           "cold: every reported module has one raw XSM object",
           str(cold_inventory))
    expect_output(rec, config, d / "app1", "42", "cold")

    warm = parse_summary(rec, build(config, cache, app, d / "app2"), "warm")
    if not warm:
        return
    expect(rec, warm.graph == cold.graph,
           "warm: identical sources publish the identical graph fingerprint",
           f"cold={cold.graph}\nwarm={warm.graph}")
    expect(rec, warm.hits == warm.modules and warm.published == 0,
           "warm: every module is proven unchanged", str(warm))
    expect(rec, warm.recomputed == 0 and warm.artifacts == cold.artifacts,
           "warm: cache hits preserve canonical artifact bytes", str(warm))
    expect(rec, xsm_inventory(cache) == cold_inventory,
           "warm: raw XSM object bytes are unchanged")
    expect_output(rec, config, d / "app2", "42", "warm")

    # --rebuild recomputes and republishes. The identity must not move, because
    # it is derived from content rather than from cache state.
    rebuilt = parse_summary(rec, build(config, cache, app, d / "app3", ["--rebuild"]), "rebuild")
    if not rebuilt:
        return
    expect(rec, rebuilt.graph == cold.graph,
           "rebuild: forced recomputation reproduces the graph fingerprint",
           f"cold={cold.graph}\nrebuild={rebuilt.graph}")
    expect(rec, rebuilt.hits == 0, "rebuild: no artifact is consulted", str(rebuilt))
    expect(rec, rebuilt.recomputed == rebuilt.modules and
           rebuilt.artifacts == cold.artifacts and
           rebuilt.publish_order == cold.publish_order,
           "rebuild: artifact bytes and publish order are deterministic",
           str(rebuilt))
    expect(rec, xsm_inventory(cache) == cold_inventory,
           "rebuild: content-addressed XSM bytes are unchanged")
    expect_output(rec, config, d / "app3", "42", "rebuild")

    # An edit must move the identity, or an unchanged fingerprint would prove
    # nothing about whether the build looked at the source at all.
    app.write_text(SOLO_V2, encoding="utf-8")
    edited = parse_summary(rec, build(config, cache, app, d / "app4"), "edit")
    if not edited:
        return
    expect(rec, edited.graph != cold.graph,
           "edit: the graph fingerprint moves with the source",
           f"cold={cold.graph}\nedit={edited.graph}")
    expect(rec, edited.hits == 0, "edit: the changed semantics cost their cache hit", str(edited))
    expect(rec, edited.recomputed == edited.modules and
           edited.artifacts != cold.artifacts,
           "edit: recomputation moves canonical artifact bytes", str(edited))
    edited_inventory = xsm_inventory(cache)
    expect(rec, all(edited_inventory.get(name) == digest
                    for name, digest in cold_inventory.items()) and
           len(edited_inventory) > len(cold_inventory),
           "edit: old immutable XSM objects remain exact-key orphans while new keys publish",
           str(edited_inventory))
    expect_output(rec, config, d / "app4", "70", "edit")

    app.write_text(SOLO_V1, encoding="utf-8")
    restored = parse_summary(rec, build(config, cache, app, d / "app5"), "revert")
    if not restored:
        return
    expect(rec, restored.graph == cold.graph,
           "revert: the original graph fingerprint returns",
           f"cold={cold.graph}\nrevert={restored.graph}")
    expect(rec, restored.hits == restored.modules,
           "revert: the artifact published before the edit is served again", str(restored))
    expect(rec, restored.artifacts == cold.artifacts,
           "revert: original canonical artifact bytes return", str(restored))
    expect(rec, xsm_inventory(cache) == edited_inventory,
           "revert: exact-key loads do not rewrite immutable XSM objects")
    expect_output(rec, config, d / "app5", "42", "revert")


def run_graph_identity(rec: Recorder, config: Config, ws: workspace.Workspace) -> None:
    """A dependency chain: the identity must cover edges, not just nodes.

    Summaries are published before target planning, and this scenario asserts
    the published identity only. Requiring the whole native build to succeed
    would tie edge coverage to whichever module shapes the current backend can
    finish, which is exactly the coverage that must not be lost.
    """
    print("--- a dependency chain publishes one identity per module ---")
    d = ws.subdir("chain")
    cache = d / ".cache"
    leaf, middle, app = d / "leaf.xr", d / "middle.xr", d / "app.xr"
    leaf.write_text(LEAF_V1, encoding="utf-8")
    middle.write_text(MIDDLE, encoding="utf-8")
    app.write_text(APP, encoding="utf-8")

    cold = parse_summary(rec, build(config, cache, app, d / "chain1"), "chain-cold", False)
    if not cold:
        return
    expect(rec, cold.modules == 3, "chain-cold: three modules summarized", str(cold))
    expect(rec, cold.hits == 0 and cold.published == 3,
           "chain-cold: every module publishes its artifact", str(cold))
    cold_inventory = xsm_inventory(cache)

    warm = parse_summary(rec, build(config, cache, app, d / "chain2"), "chain-warm", False)
    if not warm:
        return
    expect(rec, warm.graph == cold.graph,
           "chain-warm: the graph fingerprint is reproduced",
           f"cold={cold.graph}\nwarm={warm.graph}")
    expect(rec, warm.hits == 3 and warm.published == 0,
           "chain-warm: every module is proven unchanged", str(warm))
    expect(rec, warm.recomputed == 0 and warm.artifacts == cold.artifacts and
           xsm_inventory(cache) == cold_inventory,
           "chain-warm: artifact bytes and raw XSM objects are identical",
           str(warm))

    # A leaf edit must reach the identity even though the entry module's own
    # source is untouched.
    leaf.write_text(LEAF_V2, encoding="utf-8")
    edited = parse_summary(rec, build(config, cache, app, d / "chain3"), "chain-edit", False)
    if not edited:
        return
    expect(rec, edited.graph != cold.graph,
           "chain-edit: a leaf edit moves the graph fingerprint",
           f"cold={cold.graph}\nedit={edited.graph}")
    expect(rec, edited.hits < 3, "chain-edit: the changed module loses its hit", str(edited))
    expect(rec, 0 < edited.recomputed <= 3 and
           edited.recomputed == edited.missed and
           edited.artifacts != cold.artifacts,
           "chain-edit: the affected closure alone recomputes", str(edited))

    leaf.write_text(LEAF_V1, encoding="utf-8")
    restored = parse_summary(rec, build(config, cache, app, d / "chain4"), "chain-revert", False)
    if not restored:
        return
    expect(rec, restored.graph == cold.graph,
           "chain-revert: the original graph fingerprint returns",
           f"cold={cold.graph}\nrevert={restored.graph}")
    expect(rec, restored.hits == 3,
           "chain-revert: every module is served from the artifacts published before the edit",
           str(restored))
    expect(rec, restored.artifacts == cold.artifacts,
           "chain-revert: canonical artifact bytes return", str(restored))


def run_product_graph_identity(rec: Recorder, config: Config,
                               ws: workspace.Workspace) -> None:
    """The bounded source product graph must key every XSM by one PSC/GCI."""
    print("--- a two-source-module scalar product graph carries PSC/GCI authority ---")
    d = ws.subdir("product-graph")
    cache = d / ".cache"
    producer, entry = d / "producer.xr", d / "entry.xr"
    producer.write_text(PRODUCT_PRODUCER_V1, encoding="utf-8")
    entry.write_text(PRODUCT_ENTRY, encoding="utf-8")

    cold_output = d / "product1"
    cold_result = build(config, cache, entry, cold_output)
    expect_product_target_boundary(rec, cold_result, cold_output, "product-cold")
    cold = parse_summary(rec, cold_result, "product-cold", False)
    cold_program = parse_program_closure(rec, cold_result, "product-cold")
    if not cold or not cold_program:
        return
    expect(rec, cold.modules == 2 and cold_program.modules == 2 and
           cold_program.dependencies == 1,
           "product-cold: complete two-module/one-dependency authority is reported",
           f"summary={cold}\nprogram={cold_program}")
    expect(rec, cold_program.fingerprint != "0" * 64 and
           cold_program.generation_id != "0" * 32,
           "product-cold: PSC and GCI are nonzero", str(cold_program))
    expect(rec, cold.hits == 0 and cold.published == 2 and cold.recomputed == 2,
           "product-cold: both PSC-keyed XSM artifacts publish", str(cold))
    cold_inventory = xsm_inventory(cache)
    expect(rec, len(cold_inventory) == 2,
           "product-cold: one immutable XSM object exists per module",
           str(cold_inventory))

    warm_output = d / "product2"
    warm_result = build(config, cache, entry, warm_output)
    expect_product_target_boundary(rec, warm_result, warm_output, "product-warm")
    warm = parse_summary(rec, warm_result, "product-warm", False)
    warm_program = parse_program_closure(rec, warm_result, "product-warm")
    if not warm or not warm_program:
        return
    expect(rec, warm.graph == cold.graph and warm_program == cold_program,
           "product-warm: graph, PSC, and GCI identities are deterministic",
           f"cold={cold}, {cold_program}\nwarm={warm}, {warm_program}")
    expect(rec, warm.hits == 2 and warm.published == 0 and warm.recomputed == 0 and
           warm.artifacts == cold.artifacts and xsm_inventory(cache) == cold_inventory,
           "product-warm: both exact PSC/GCI cache keys hit without rewriting",
           str(warm))

    producer.write_text(PRODUCT_PRODUCER_V2, encoding="utf-8")
    edited_output = d / "product3"
    edited_result = build(config, cache, entry, edited_output)
    expect_product_target_boundary(rec, edited_result, edited_output, "product-edit")
    edited = parse_summary(rec, edited_result, "product-edit", False)
    edited_program = parse_program_closure(rec, edited_result, "product-edit")
    if not edited or not edited_program:
        return
    expect(rec, edited.graph != cold.graph and
           edited_program.fingerprint != cold_program.fingerprint and
           edited_program.generation_id != cold_program.generation_id,
           "product-edit: dependency source change rotates graph, PSC, and GCI",
           f"cold={cold}, {cold_program}\nedit={edited}, {edited_program}")
    expect(rec, edited.hits == 0 and edited.published == 2 and edited.recomputed == 2 and
           edited.artifacts != cold.artifacts,
           "product-edit: full program authority rotates both module cache keys",
           str(edited))
    edited_inventory = xsm_inventory(cache)
    expect(rec, len(edited_inventory) == 4 and
           all(edited_inventory.get(name) == digest
               for name, digest in cold_inventory.items()),
           "product-edit: old exact-key artifacts remain immutable beside new keys",
           str(edited_inventory))

    producer.write_text(PRODUCT_PRODUCER_V1, encoding="utf-8")
    restored_output = d / "product4"
    restored_result = build(config, cache, entry, restored_output)
    expect_product_target_boundary(rec, restored_result, restored_output, "product-revert")
    restored = parse_summary(rec, restored_result, "product-revert", False)
    restored_program = parse_program_closure(rec, restored_result, "product-revert")
    if not restored or not restored_program:
        return
    expect(rec, restored.graph == cold.graph and restored_program == cold_program,
           "product-revert: original graph, PSC, and GCI return",
           f"cold={cold}, {cold_program}\nrevert={restored}, {restored_program}")
    expect(rec, restored.hits == 2 and restored.artifacts == cold.artifacts and
           xsm_inventory(cache) == edited_inventory,
           "product-revert: original exact-key artifacts serve both modules",
           str(restored))


SCENARIOS = {
    "determinism": run_determinism,
    "graph-identity": run_graph_identity,
    "product-graph-identity": run_product_graph_identity,
}


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Module summary determinism suite")
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

    print("=== Module Summary Determinism ===")
    print(f"Binary: {xray}")
    if not (xray.is_file() and os.access(xray, os.X_OK)):
        print(f"FAIL: xray binary not executable: {xray}")
        return 1

    config = Config(xray=xray, opt_level=opt_level, timeout=timeout)
    rec = Recorder()
    with workspace.Workspace("xray_module_summary") as ws:
        for runner in SCENARIOS.values():
            runner(rec, config, ws)
            print("")

    print(f"=== Results: {rec.passed} passed, {rec.failed} failed ===")
    return 1 if rec.failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
