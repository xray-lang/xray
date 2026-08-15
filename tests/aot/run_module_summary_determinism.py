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

SOLO_V1 = "fn scale(x: int) -> int {\n    return x * 3\n}\n\nprint(scale(14))\n"
SOLO_V2 = "fn scale(x: int) -> int {\n    return x * 5\n}\n\nprint(scale(14))\n"
LEAF_V1 = "export fn scale(x: int) -> int {\n    return x * 3\n}\n"
LEAF_V2 = "export fn scale(x: int) -> int {\n    return x * 5\n}\n"
MIDDLE = ('import { scale } from "./leaf"\n\n'
          "export fn twice(x: int) -> int {\n    return scale(x) + scale(x)\n}\n")
APP = 'import { twice } from "./middle"\n\nprint(twice(7))\n'


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


SCENARIOS = {
    "determinism": run_determinism,
    "graph-identity": run_graph_identity,
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
