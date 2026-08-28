#!/usr/bin/env python3
"""Qualify the product-level verified program plan cache.

The unit suite proves the cache boundary in process. This runner drives the
product entry point -- `xray build --native --cache-dir` -- and qualifies what
only whole builds can show: that one program's identity is reproducible across
independent cold builds, that a warm build rewrites nothing, that an edit and
its revert move the identity and bring it back, that relocating the sources
does not cost a recomputation, and that concurrent builds sharing one cache
root converge on one published object with no residue.

Native linking of a two-module product graph does not complete on the current
base, so every assertion here reads the build's own cache report and the cache
root rather than the final executable. The cache objects are published before
the failing step, which is why the qualification is still exact.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import statistics
import sys
import time
from dataclasses import dataclass
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, report, scheduler, workspace  # noqa: E402

PROJECT_DIR = Path(__file__).resolve().parents[2]
SKIP_EXIT = 77

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
PLAN_CACHE_RE = re.compile(r"\[target-plan-cache\] (hit|miss|rebuild) ")
# Printed by the collection race case in the unit suite. The product entry
# point has no cache budget, so that is where this row is measured.
COLLECT_RACE_RE = re.compile(
    r"collect race: evicted=(\d+) readers=(\d+) recomputed=(\d+)")

PRODUCER_V1 = "export fn add1(value: i64) -> i64 { return value + 1 }\n"
PRODUCER_V2 = "export fn add1(value: i64) -> i64 { return value + 2 }\n"
ENTRY = 'import { add1 } from "./producer"\nfn main() -> i64 { return add1(41) }\n'
# The same program with its two top-level statements in the other order.
ENTRY_REORDERED = 'fn main() -> i64 { return add1(41) }\nimport { add1 } from "./producer"\n'
# The same program with the dependency under a different module name.
ENTRY_RENAMED = 'import { add1 } from "./producer2"\nfn main() -> i64 { return add1(41) }\n'


@dataclass(frozen=True)
class Identity:
    """The fields that name one program.

    `xsm-publish-order` is deliberately absent: it summarizes the modules this
    run happened to recompute, so a warm run and a cold run of one identical
    program report different values for it.
    """

    modules: int
    graph: str
    artifacts: str
    psc: str
    gci: str


@dataclass(frozen=True)
class Report:
    identity: Identity
    hits: int
    published: int
    missed: int
    recomputed: int
    plan_cache: str


@dataclass(frozen=True)
class MeasuredBuild:
    """One timed build. `did_work` is false when it never reached its cache
    decisions -- a provider probe timeout under load, most often -- and such a
    sample must be discarded rather than averaged in."""

    seconds: float
    peak_rss: int
    report: "Report | None"
    did_work: bool


@dataclass
class Config:
    xray: Path
    timeout: float | None
    samples: int


def build(config: Config, cache: Path, entry: Path, out: Path,
          extra: tuple = ()) -> proc.ProcResult:
    return proc.run(
        [config.xray, "build", "--native", "-O", "0", "--verbose",
         "--cache-dir", cache, *extra, "-o", out, entry],
        timeout=config.timeout,
    )


def parse(result: proc.ProcResult) -> Report | None:
    """The cache report a build states about itself, or None if it never got there."""
    log = result.combined_text()
    summary = SUMMARY_RE.search(log)
    program = PROGRAM_RE.search(log)
    plan_cache = PLAN_CACHE_RE.search(log)
    if not summary or not program or not plan_cache:
        return None
    identity = Identity(
        modules=int(summary.group(1)),
        graph=summary.group(2),
        artifacts=summary.group(9),
        psc=program.group(3),
        gci=program.group(4),
    )
    return Report(
        identity=identity,
        hits=int(summary.group(3)),
        published=int(summary.group(4)),
        missed=int(summary.group(5)),
        recomputed=int(summary.group(8)),
        plan_cache=plan_cache.group(1),
    )


def objects(cache: Path) -> dict[str, str]:
    """Every published object under the cache root, by relative path and digest."""
    found: dict[str, str] = {}
    for kind in ("xsm", "xtp"):
        for directory in sorted(cache.rglob(kind)):
            if not directory.is_dir():
                continue
            for path in sorted(directory.iterdir()):
                if path.is_file() and not path.name.startswith("."):
                    found[str(path.relative_to(cache))] = hashlib.sha256(
                        path.read_bytes()
                    ).hexdigest()
    return found


def residue(cache: Path) -> list[str]:
    """Names the store never finished publishing."""
    return sorted(
        str(path.relative_to(cache))
        for path in cache.rglob(".tmp-*")
        if path.is_file()
    )


def write_product(directory: Path, producer: str) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "producer.xr").write_text(producer, encoding="utf-8")
    entry = directory / "entry.xr"
    entry.write_text(ENTRY, encoding="utf-8")
    return entry


def qualify_determinism(rec: report.Report, config: Config,
                        ws: workspace.Workspace) -> None:
    """Independent cold builds of one program must agree byte for byte."""
    d = ws.subdir("determinism")
    entry = write_product(d / "src", PRODUCER_V1)
    seen: list[tuple[Identity, dict[str, str]]] = []
    for index in range(config.samples):
        cache = d / f"cache-{index}"
        result = build(config, cache, entry, d / f"out-{index}" / "app")
        parsed = parse(result)
        if not parsed:
            rec.record("determinism: every cold build reports its cache decisions",
                       report.Status.FAIL,
                       f"run {index} produced no report",
                       result.combined_text())
            return
        seen.append((parsed.identity, objects(cache)))

    first_identity, first_objects = seen[0]
    drifted = [
        f"run {index}: {identity}" for index, (identity, _) in enumerate(seen)
        if identity != first_identity
    ]
    rewritten = [
        f"run {index}: {sorted(found.items())}" for index, (_, found) in enumerate(seen)
        if found != first_objects
    ]
    rec.record(
        f"determinism: {config.samples} independent cold builds share one identity",
        report.Status.PASS if not drifted else report.Status.FAIL,
        "\n".join([str(first_identity), *drifted]) if drifted else "",
    )
    rec.record(
        "determinism: independent cold builds publish identical object bytes",
        report.Status.PASS if not rewritten else report.Status.FAIL,
        "\n".join([str(sorted(first_objects.items())), *rewritten]) if rewritten else "",
    )


def qualify_lifecycle(rec: report.Report, config: Config,
                      ws: workspace.Workspace) -> None:
    """Cold, warm, rebuild, edit, and revert over one cache root."""
    d = ws.subdir("lifecycle")
    source = d / "src"
    entry = write_product(source, PRODUCER_V1)
    cache = d / "cache"

    stages: dict[str, Report] = {}
    for name, extra in (("cold", ()), ("warm", ()), ("rebuild", ("--rebuild",))):
        result = build(config, cache, entry, d / f"out-{name}" / "app", extra)
        parsed = parse(result)
        if not parsed:
            rec.record(f"lifecycle: {name} reports its cache decisions",
                       report.Status.FAIL, f"{name} produced no report",
                       result.combined_text())
            return
        stages[name] = parsed
    cold_objects = objects(cache)

    rec.record("lifecycle: a cold build publishes every module",
               report.Status.PASS if (stages["cold"].hits == 0 and
                                      stages["cold"].published == stages["cold"].identity.modules and
                                      stages["cold"].plan_cache == "miss")
               else report.Status.FAIL, str(stages["cold"]))
    rec.record("lifecycle: a warm build proves every module unchanged",
               report.Status.PASS if (stages["warm"].hits == stages["warm"].identity.modules and
                                      stages["warm"].published == 0 and
                                      stages["warm"].recomputed == 0 and
                                      stages["warm"].plan_cache == "hit" and
                                      objects(cache) == cold_objects)
               else report.Status.FAIL, str(stages["warm"]))
    rec.record("lifecycle: a rebuild reproduces the identity it recomputes",
               report.Status.PASS if (stages["rebuild"].identity == stages["cold"].identity and
                                      stages["rebuild"].plan_cache == "rebuild" and
                                      objects(cache) == cold_objects)
               else report.Status.FAIL,
               f"cold={stages['cold'].identity}\nrebuild={stages['rebuild'].identity}")

    (source / "producer.xr").write_text(PRODUCER_V2, encoding="utf-8")
    edited = parse(build(config, cache, entry, d / "out-edit" / "app"))
    if not edited:
        rec.record("lifecycle: an edit reports its cache decisions",
                   report.Status.FAIL, "edit produced no report")
        return
    edited_objects = objects(cache)
    rec.record("lifecycle: a dependency edit rotates the whole program identity",
               report.Status.PASS if (edited.identity != stages["cold"].identity and
                                      edited.identity.psc != stages["cold"].identity.psc and
                                      edited.identity.gci != stages["cold"].identity.gci and
                                      edited.published == edited.identity.modules)
               else report.Status.FAIL,
               f"cold={stages['cold'].identity}\nedit={edited.identity}")
    rec.record("lifecycle: an edit leaves the objects it replaced immutable",
               report.Status.PASS if all(edited_objects.get(name) == digest
                                         for name, digest in cold_objects.items())
               else report.Status.FAIL, str(sorted(edited_objects)))

    (source / "producer.xr").write_text(PRODUCER_V1, encoding="utf-8")
    reverted = parse(build(config, cache, entry, d / "out-revert" / "app"))
    if not reverted:
        rec.record("lifecycle: a revert reports its cache decisions",
                   report.Status.FAIL, "revert produced no report")
        return
    rec.record("lifecycle: a revert returns the original identity from cache",
               report.Status.PASS if (reverted.identity == stages["cold"].identity and
                                      reverted.hits == reverted.identity.modules and
                                      reverted.published == 0 and
                                      objects(cache) == edited_objects)
               else report.Status.FAIL,
               f"cold={stages['cold'].identity}\nrevert={reverted.identity}")



def qualify_reorder(rec: report.Report, config: Config,
                    ws: workspace.Workspace) -> None:
    """Rewriting the sources must move the identity, never silently reuse it.

    Cache identity covers the normalized source text and the module set. Both
    reorderings below leave the program's meaning alone, so a cache that
    answered for them would be guessing rather than proving; a miss here is the
    safe direction and the one the key is built to take.
    """
    d = ws.subdir("reorder")
    cache = d / "cache"
    source = d / "src"
    entry = write_product(source, PRODUCER_V1)
    base = parse(build(config, cache, entry, d / "out-base" / "app"))
    if not base:
        rec.record("reorder: the baseline build reports its cache decisions",
                   report.Status.FAIL, "no report")
        return
    base_objects = objects(cache)

    entry.write_text(ENTRY_REORDERED, encoding="utf-8")
    reordered = parse(build(config, cache, entry, d / "out-reordered" / "app"))
    if not reordered:
        rec.record("reorder: the reordered build reports its cache decisions",
                   report.Status.FAIL, "no report")
        return
    rec.record("reorder: reordering the entry's statements moves the identity",
               report.Status.PASS if (reordered.identity != base.identity and
                                      reordered.hits == 0 and
                                      reordered.published == reordered.identity.modules)
               else report.Status.FAIL,
               f"base={base.identity}\nreordered={reordered.identity}\n{reordered}")

    (source / "producer2.xr").write_text(PRODUCER_V1, encoding="utf-8")
    entry.write_text(ENTRY_RENAMED, encoding="utf-8")
    renamed = parse(build(config, cache, entry, d / "out-renamed" / "app"))
    if not renamed:
        rec.record("reorder: the renamed build reports its cache decisions",
                   report.Status.FAIL, "no report")
        return
    rec.record("reorder: renaming the dependency module moves the identity",
               report.Status.PASS if (renamed.identity != base.identity and
                                      renamed.identity != reordered.identity and
                                      renamed.hits == 0)
               else report.Status.FAIL,
               f"base={base.identity}\nrenamed={renamed.identity}\n{renamed}")
    rec.record("reorder: neither rewrite disturbs the objects already published",
               report.Status.PASS if all(objects(cache).get(name) == digest
                                         for name, digest in base_objects.items())
               and not residue(cache)
               else report.Status.FAIL,
               f"base={sorted(base_objects)}\nnow={sorted(objects(cache))}\n"
               f"residue={residue(cache)}")


def qualify_relocation(rec: report.Report, config: Config,
                       ws: workspace.Workspace) -> None:
    """Moving the sources must not cost a recomputation.

    Cache identity is derived from content. If any part of it reached for a
    path, the same program under a new directory would miss its own objects.
    """
    d = ws.subdir("relocation")
    cache = d / "cache"
    original = write_product(d / "original", PRODUCER_V1)
    cold = parse(build(config, cache, original, d / "out-original" / "app"))
    if not cold:
        rec.record("relocation: the original build reports its cache decisions",
                   report.Status.FAIL, "no report")
        return
    published = objects(cache)

    moved = write_product(d / "moved", PRODUCER_V1)
    relocated = parse(build(config, cache, moved, d / "out-moved" / "app"))
    if not relocated:
        rec.record("relocation: the relocated build reports its cache decisions",
                   report.Status.FAIL, "no report")
        return
    rec.record("relocation: the same content under a new path keeps one identity",
               report.Status.PASS if relocated.identity == cold.identity
               else report.Status.FAIL,
               f"original={cold.identity}\nmoved={relocated.identity}")
    rec.record("relocation: a relocated build is served, not recomputed",
               report.Status.PASS if (relocated.hits == relocated.identity.modules and
                                      relocated.published == 0 and
                                      objects(cache) == published)
               else report.Status.FAIL, str(relocated))



def object_paths(cache: Path, kind: str) -> list[Path]:
    return sorted(
        path
        for directory in cache.rglob(kind) if directory.is_dir()
        for path in directory.iterdir()
        if path.is_file() and not path.name.startswith(".")
    )


def qualify_hostile(rec: report.Report, config: Config,
                    ws: workspace.Workspace) -> None:
    """Another program's objects, planted under this program's keys.

    A key is an address, not a proof of what lives at it. These are the cases
    where the planted bytes are a whole, self-consistent artifact of a
    different program -- carrying a different PSC and generation -- so nothing
    short of reconstructing the identity and re-verifying can reject them. A
    miss is not a correctness fallback: the rebuild has to land back on the
    same identity and the same bytes.
    """
    d = ws.subdir("hostile")
    mine = d / "mine"
    theirs = d / "theirs"
    my_cache = d / "cache-mine"
    their_cache = d / "cache-theirs"
    my_entry = write_product(mine, PRODUCER_V1)
    their_entry = write_product(theirs, PRODUCER_V2)

    baseline = parse(build(config, my_cache, my_entry, d / "out-mine" / "app"))
    foreign = parse(build(config, their_cache, their_entry, d / "out-theirs" / "app"))
    if not baseline or not foreign:
        rec.record("hostile: both programs report their cache decisions",
                   report.Status.FAIL, "a build produced no report")
        return
    canonical = objects(my_cache)
    rec.record("hostile: the two programs are distinct authorities",
               report.Status.PASS if (baseline.identity.psc != foreign.identity.psc and
                                      baseline.identity.gci != foreign.identity.gci)
               else report.Status.FAIL,
               f"mine={baseline.identity}\ntheirs={foreign.identity}")

    for kind in ("xtp", "xsm"):
        mine_objects = object_paths(my_cache, kind)
        their_objects = object_paths(their_cache, kind)
        if not mine_objects or not their_objects:
            rec.record(f"hostile: {kind} objects exist to plant",
                       report.Status.FAIL,
                       f"mine={len(mine_objects)} theirs={len(their_objects)}")
            return

        # A whole object from the other program, at the address this program
        # will look up.
        mine_objects[0].write_bytes(their_objects[0].read_bytes())
        planted = parse(build(config, my_cache, my_entry, d / f"out-{kind}-planted" / "app"))
        if not planted:
            rec.record(f"hostile: the build over a planted {kind} reports its decisions",
                       report.Status.FAIL, "no report")
            return
        rec.record(f"hostile: a foreign {kind} does not become this program's answer",
                   report.Status.PASS if (planted.identity == baseline.identity and
                                          objects(my_cache) == canonical)
                   else report.Status.FAIL,
                   f"want={baseline.identity}\ngot={planted.identity}\n"
                   f"objects={sorted(objects(my_cache).items())}")

        # Half an object, which no verifier may complete from context.
        whole = mine_objects[0].read_bytes()
        mine_objects[0].write_bytes(whole[: len(whole) // 2])
        truncated = parse(build(config, my_cache, my_entry,
                                d / f"out-{kind}-truncated" / "app"))
        if not truncated:
            rec.record(f"hostile: the build over a truncated {kind} reports its decisions",
                       report.Status.FAIL, "no report")
            return
        rec.record(f"hostile: a truncated {kind} costs a rebuild of the same bytes",
                   report.Status.PASS if (truncated.identity == baseline.identity and
                                          objects(my_cache) == canonical)
                   else report.Status.FAIL,
                   f"want={baseline.identity}\ngot={truncated.identity}\n"
                   f"objects={sorted(objects(my_cache).items())}")

    rec.record("hostile: no rejection leaves unfinished residue",
               report.Status.PASS if not residue(my_cache) else report.Status.FAIL,
               "\n".join(residue(my_cache)))


def qualify_parallel(rec: report.Report, config: Config, ws: workspace.Workspace,
                     workers: int) -> None:
    """Builds sharing one cache root must converge on one published object.

    Whichever build wins the publication race, the root must end up holding the
    objects a serial cold build publishes, with nothing half-written.
    """
    d = ws.subdir("parallel")
    entry = write_product(d / "src", PRODUCER_V1)
    reference_cache = d / "cache-reference"
    reference = parse(build(config, reference_cache, entry, d / "out-reference" / "app"))
    if not reference:
        rec.record("parallel: the serial reference reports its cache decisions",
                   report.Status.FAIL, "no report")
        return
    expected_objects = objects(reference_cache)

    for phase in ("cold", "warm"):
        cache = d / f"cache-{phase}"
        if phase == "warm" and not parse(build(config, cache, entry, d / "out-prime" / "app")):
            rec.record("parallel: the warm phase primes its cache",
                       report.Status.FAIL, "priming build produced no report")
            return
        sched = scheduler.Scheduler({scheduler.LINK: workers})
        tasks = [
            scheduler.Task(
                key=str(index),
                fn=(lambda i=index: build(config, cache, entry, d / f"out-{phase}-{i}" / "app")),
                tag=scheduler.LINK,
            )
            for index in range(workers)
        ]
        finished = sched.run(tasks)
        reports: list[Report] = []
        for index in range(workers):
            value = finished.get(str(index))
            if isinstance(value, BaseException):
                rec.record(f"parallel-{phase}: every worker completes",
                           report.Status.FAIL, f"worker {index} raised {value!r}")
                return
            parsed = parse(value)
            if not parsed:
                rec.record(f"parallel-{phase}: every worker reports its cache decisions",
                           report.Status.FAIL, f"worker {index} produced no report",
                           value.combined_text())
                return
            reports.append(parsed)

        drifted = [str(item.identity) for item in reports
                   if item.identity != reference.identity]
        rec.record(f"parallel-{phase}: every concurrent build reports one identity",
                   report.Status.PASS if not drifted else report.Status.FAIL,
                   "\n".join([str(reference.identity), *drifted]) if drifted else "")
        found = objects(cache)
        rec.record(f"parallel-{phase}: the root holds exactly the serial objects",
                   report.Status.PASS if found == expected_objects else report.Status.FAIL,
                   f"want={sorted(expected_objects.items())}\ngot={sorted(found.items())}")
        left = residue(cache)
        rec.record(f"parallel-{phase}: no worker leaves an unfinished object",
                   report.Status.PASS if not left else report.Status.FAIL, "\n".join(left))
        published = sum(item.published for item in reports)
        if phase == "cold":
            rec.record("parallel-cold: publication totals the modules, however it raced",
                       report.Status.PASS if published == reference.identity.modules
                       else report.Status.FAIL,
                       f"want={reference.identity.modules} got={published} "
                       f"per-worker={[item.published for item in reports]}")
        else:
            served = all(item.hits == item.identity.modules and item.published == 0
                         for item in reports)
            rec.record("parallel-warm: every concurrent build is served, none republishes",
                       report.Status.PASS if served else report.Status.FAIL,
                       str([(item.hits, item.published) for item in reports]))


def process_rss_bytes(pid: int) -> int:
    """Resident size of one live process, or 0 where it cannot be read."""
    status = Path(f"/proc/{pid}/status")
    if status.is_file():
        try:
            match = re.search(r"(?m)^VmRSS:\s+(\d+)\s+kB$",
                              status.read_text(encoding="ascii"))
            return int(match.group(1)) * 1024 if match else 0
        except (OSError, ValueError):
            return 0
    result = proc.run(["ps", "-o", "rss=", "-p", str(pid)], timeout=5)
    try:
        return int(result.stdout_text().strip()) * 1024 if result.ok else 0
    except ValueError:
        return 0


def distribution(values: list[float], unit: str) -> dict:
    ordered = sorted(values)
    if not ordered:
        return {"samples": 0, "unit": unit}

    def at(quantile: float) -> float:
        position = (len(ordered) - 1) * quantile
        low, high = int(position), min(int(position) + 1, len(ordered) - 1)
        fraction = position - low
        return ordered[low] * (1.0 - fraction) + ordered[high] * fraction

    return {
        "samples": len(ordered),
        "unit": unit,
        "p50": round(at(0.50), 6),
        "p95": round(at(0.95), 6),
        "max": round(ordered[-1], 6),
        "mean": round(statistics.fmean(ordered), 6),
    }


def measure_build(config: Config, cache: Path, entry: Path, out: Path,
                  extra: tuple = ()) -> "MeasuredBuild":
    """Wall time, peak resident size, and the cache report of one build.

    The kernel reports the child's high-water mark exactly; polling its live
    resident size at any interval under-reports a build this short. Where the
    kernel does not offer it, polling is the honest fallback and says so by
    being the only number available. The shared process helper cannot be used
    here because neither reading needs the child's pid while it runs.
    """
    import subprocess

    argv = [str(config.xray), "build", "--native", "-O", "0", "--verbose",
            "--cache-dir", str(cache), *extra, "-o", str(out), str(entry)]
    out.parent.mkdir(parents=True, exist_ok=True)
    log = out.with_suffix(".log")
    started = time.perf_counter()
    with open(log, "wb") as handle:
        child = subprocess.Popen(argv, stdout=handle, stderr=subprocess.STDOUT)
        if hasattr(os, "wait4"):
            _, status, usage = os.wait4(child.pid, 0)
            child.returncode = os.waitstatus_to_exitcode(status)
            # ru_maxrss is bytes on Darwin and kilobytes everywhere else POSIX.
            peak = int(usage.ru_maxrss if sys.platform == "darwin"
                       else usage.ru_maxrss * 1024)
        else:
            peak = 0
            while True:
                peak = max(peak, process_rss_bytes(child.pid))
                if child.poll() is not None:
                    break
                time.sleep(0.01)
    elapsed = time.perf_counter() - started
    text = log.read_bytes()
    parsed = parse(proc.ProcResult(tuple(argv), child.returncode, text, b"", False))
    # A build that never reached its cache decisions did not do the work being
    # timed. Under load the native provider probe times out at ten seconds, and
    # folding those into a median reports the probe as the cache's cost.
    return MeasuredBuild(elapsed, peak, parsed, parsed is not None)


def directory_bytes(path: Path) -> int:
    return sum(item.stat().st_size for item in path.rglob("*") if item.is_file())


def _first_line(argv: list) -> str:
    result = proc.run(argv, timeout=30)
    text = result.stdout_text("replace").strip()
    return text.splitlines()[0] if result.ok and text else ""


def load_average() -> list:
    """The machine's run-queue length, which decides whether these numbers mean
    anything. This host is shared with other lanes; a measurement taken under
    someone else's compile is not a measurement of this cache."""
    try:
        return [round(value, 2) for value in os.getloadavg()]
    except (OSError, AttributeError):
        return []


def contention(cold: dict, warm: dict, load: dict) -> dict:
    """How much of this measurement belongs to this cache.

    This host is shared. A median is robust to a handful of slow samples --
    another lane's compile has to occupy more than half the run to move it --
    but a p95 is not: one interrupted sample is enough. Reporting a single
    usable/unusable verdict throws away the half that survived, so say which
    statistic the spread condemns and which it does not.
    """
    def spread(phase: dict) -> float:
        seconds = phase.get("seconds", {})
        median = seconds.get("p50") or 0.0
        return round((seconds.get("p95") or 0.0) / median, 3) if median else 0.0

    cold_spread = spread(cold)
    warm_spread = spread(warm)
    samples = cold.get("seconds", {}).get("samples", 0)
    # A quiet machine holds p95 within roughly half again the median.
    tail_usable = max(cold_spread, warm_spread) <= 1.5
    # Fewer than five surviving samples leaves the median itself to one bad
    # draw, and a discarded pair means a build did not run at all.
    median_usable = samples >= 5 and cold.get("discarded_pairs", 0) == 0
    return {
        "cold_p95_over_p50": cold_spread,
        "warm_p95_over_p50": warm_spread,
        "peak_load_average_1m": max(
            [load.get("before", [0])[0] if load.get("before") else 0,
             load.get("after", [0])[0] if load.get("after") else 0]),
        "surviving_samples": samples,
        "median_usable": median_usable,
        "tail_usable": tail_usable,
        "note": "" if tail_usable else
                "another lane was building during this run: p95 and max belong "
                "to that, not to this cache. The medians, counts, byte totals "
                "and invalidation breadth are unaffected"
                if median_usable else
                "too few surviving samples to trust any statistic here",
    }


def environment(config: Config) -> dict:
    """What a later run has to match for these numbers to be comparable.

    A measurement without its machine is a number nobody can reproduce or
    refute, so every field that cannot be read is recorded as empty rather
    than omitted.
    """
    facts = {
        "commit": _first_line(["git", "-C", str(PROJECT_DIR), "rev-parse", "HEAD"]),
        "tree_dirty": bool(
            proc.run(["git", "-C", str(PROJECT_DIR), "status", "--porcelain"],
                     timeout=30).stdout.strip()
        ),
        "compiler": _first_line([str(config.xray), "--version"]),
        "host": " ".join(os.uname()[:3]) if hasattr(os, "uname") else sys.platform,
        "logical_cpus": platform.cpu_count(),
        "cpu": "",
        "memory_bytes": "",
        "filesystem": "",
        "power_policy": "",
    }
    if sys.platform == "darwin":
        facts["cpu"] = _first_line(["sysctl", "-n", "machdep.cpu.brand_string"])
        facts["memory_bytes"] = _first_line(["sysctl", "-n", "hw.memsize"])
        facts["power_policy"] = _first_line(["pmset", "-g", "ps"])
        facts["filesystem"] = _first_line(
            ["sh", "-c", f"df -P {PROJECT_DIR} | tail -1 | awk '{{print $1}}'"])
    elif sys.platform.startswith("linux"):
        facts["cpu"] = _first_line(["sh", "-c", "grep -m1 'model name' /proc/cpuinfo"])
        facts["memory_bytes"] = _first_line(["sh", "-c", "grep -m1 MemTotal /proc/meminfo"])
        facts["filesystem"] = _first_line(["stat", "-f", "-c", "%T", str(PROJECT_DIR)])
        facts["power_policy"] = _first_line(
            ["sh", "-c", "cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"])
    return facts


def measure_paired(config: Config, d: Path, entry: Path) -> tuple[dict, dict]:
    """Time cold and warm back to back inside one sample.

    This machine is shared with other lanes, so its load drifts during a run.
    Measuring all the cold samples and then all the warm ones lets that drift
    land on one phase and not the other -- enough to report a warm build as
    slower than a cold one. Alternating puts the same drift on both.

    One cache per sample, cold first and warm immediately after it, so the warm
    build is the same program against the objects the cold one just published.
    The first sample is discarded: the first build of a session pays for page
    cache and toolchain discovery that no later build repeats.
    """
    cold_times: list[float] = []
    cold_rss: list[float] = []
    warm_times: list[float] = []
    warm_rss: list[float] = []
    cold_report = None
    warm_report = None
    discarded = 0
    cache = d / "paired-0"
    for sample in range(config.samples + 1):
        cache = d / f"paired-{sample}"
        cold_build = measure_build(config, cache, entry, d / f"paired-cold-{sample}" / "app")
        warm_build = measure_build(config, cache, entry, d / f"paired-warm-{sample}" / "app")
        if sample == 0:
            continue
        # Both halves of a pair are kept or dropped together: half a pair says
        # nothing about the relation the pairing exists to measure.
        if not (cold_build.did_work and warm_build.did_work):
            discarded += 1
            continue
        cold_times.append(cold_build.seconds)
        cold_rss.append(float(cold_build.peak_rss))
        warm_times.append(warm_build.seconds)
        warm_rss.append(float(warm_build.peak_rss))
        cold_report = cold_build.report or cold_report
        warm_report = warm_build.report or warm_report

    cold = {
        "seconds": distribution(cold_times, "s"),
        "peak_rss": distribution(cold_rss, "bytes"),
        "objects_on_disk": len(objects(cache)),
        "disk_bytes": directory_bytes(cache),
        "discarded_pairs": discarded,
    }
    warm = {
        "seconds": distribution(warm_times, "s"),
        "peak_rss": distribution(warm_rss, "bytes"),
        "objects_on_disk": len(objects(cache)),
        "disk_bytes": directory_bytes(cache),
    }
    if cold_report:
        cold["hits"] = cold_report.hits
        cold["published"] = cold_report.published
        cold["recomputed"] = cold_report.recomputed
        cold["identity"] = str(cold_report.identity)
    if warm_report:
        warm["hits"] = warm_report.hits
        warm["published"] = warm_report.published
        warm["recomputed"] = warm_report.recomputed
        warm["identity"] = str(warm_report.identity)
        # A warm build that rewrote one byte would have had to republish.
        warm["rewritten_bytes"] = 0 if warm_report.published == 0 else -1
    return cold, warm


def measure(config: Config, ws: workspace.Workspace, workers: int) -> dict:
    """Cost and invalidation breadth, reported rather than gated.

    A threshold here would encode this machine. The point is a repeatable
    number a later run on the same hardware can be compared against, which is
    why the environment and the machine's load travel with it.
    """
    d = ws.subdir("measure")
    source = d / "src"
    entry = write_product(source, PRODUCER_V1)
    load_before = load_average()
    cold, warm = measure_paired(config, d, entry)
    warm_cache = d / f"paired-{config.samples}"
    warm_objects = objects(warm_cache)

    # The edit and its revert run against the warm cache, so their breadth is
    # measured against a store that already holds the unedited program.
    (source / "producer.xr").write_text(PRODUCER_V2, encoding="utf-8")
    edit_build = measure_build(config, warm_cache, entry, d / "edit-out" / "app")
    edit_seconds, edit_rss, edited = (edit_build.seconds, edit_build.peak_rss,
                                      edit_build.report)
    edited_objects = objects(warm_cache)
    leaf_edit = {
        "seconds": round(edit_seconds, 6),
        "peak_rss": edit_rss,
        "invalidated_modules": edited.missed if edited else -1,
        "recomputed": edited.recomputed if edited else -1,
        "modules": edited.identity.modules if edited else -1,
        "new_objects": len(edited_objects) - len(warm_objects),
        "disk_bytes": directory_bytes(warm_cache),
    }

    (source / "producer.xr").write_text(PRODUCER_V1, encoding="utf-8")
    revert_build = measure_build(config, warm_cache, entry, d / "revert-out" / "app")
    revert_seconds, revert_rss, reverted = (revert_build.seconds, revert_build.peak_rss,
                                            revert_build.report)
    revert = {
        "seconds": round(revert_seconds, 6),
        "peak_rss": revert_rss,
        # Restored means the identity the unedited program had, not merely one
        # that differs from the edit.
        "identity_restored": bool(reverted and warm.get("identity") and
                                  str(reverted.identity) == warm["identity"]),
        "hits": reverted.hits if reverted else -1,
        # Objects the edit and the revert added beside the warm set.
        "extra_objects": len(objects(warm_cache)) - len(warm_objects),
        "disk_bytes": directory_bytes(warm_cache),
    }

    load = {"before": load_before, "after": load_average()}
    return {
        "environment": environment(config),
        "load_average": load,
        "contention": contention(cold, warm, load),
        "replay": {
            # Nothing here draws on a random source: repeats differ only in the
            # scheduler's interleaving, and the concurrent phases align their
            # workers with a barrier rather than a sleep. Re-running with the
            # same samples and workers on the same commit reproduces the shape.
            "samples": config.samples,
            "discarded_warmups_per_phase": 1,
            "workers": workers,
            "random_source": "none",
            "command": f"run_program_plan_cache_qualification.py <xray> "
                       f"--samples {config.samples} --workers {workers} --measure",
        },
        "cold": cold,
        "warm": warm,
        "leaf_edit": leaf_edit,
        "revert": revert,
        "relocation": measure_relocation(config, d, workers),
        "crash_residue": measure_crash_residue(config, d),
        "concurrent": measure_concurrent(config, d, entry, workers),
        "quota_gc": measure_quota_gc(config, d),
    }


def measure_concurrent(config: Config, d: Path, entry: Path, workers: int) -> dict:
    """What N builds sharing one root cost, repeated like every other phase.

    One sample of a concurrent phase reports whatever the scheduler happened to
    do that time. The spread across repeats is the part worth keeping.
    """
    result = {}
    discarded = 0
    for phase in ("cold", "warm"):
        walls: list[float] = []
        times: list[float] = []
        peaks: list[float] = []
        recomputed: list[float] = []
        for sample in range(config.samples + 1):
            cache = d / f"concurrent-{phase}-{sample}"
            if phase == "warm":
                measure_build(config, cache, entry, d / f"concurrent-prime-{sample}" / "app")
            sched = scheduler.Scheduler({scheduler.LINK: workers})
            tasks = [
                scheduler.Task(
                    key=str(index),
                    fn=(lambda i=index, c=cache, sm=sample: measure_build(
                        config, c, entry, d / f"concurrent-{phase}-{sm}-{i}" / "app")),
                    tag=scheduler.LINK,
                )
                for index in range(workers)
            ]
            started = time.perf_counter()
            finished = sched.run(tasks)
            wall = time.perf_counter() - started
            if sample == 0:
                continue
            round_recomputed = 0
            for index in range(workers):
                value = finished.get(str(index))
                if isinstance(value, BaseException) or value is None:
                    continue
                if not value.did_work:
                    discarded += 1
                    continue
                times.append(value.seconds)
                peaks.append(float(value.peak_rss))
                round_recomputed += value.report.recomputed
            walls.append(wall)
            recomputed.append(float(round_recomputed))
        result[phase] = {
            "workers": workers,
            "wall_seconds": distribution(walls, "s"),
            "per_build_seconds": distribution(times, "s"),
            "peak_rss": distribution(peaks, "bytes"),
            # Serial cold recomputes each module once; anything beyond that is
            # work the concurrent builds duplicated.
            "recomputed_per_round": distribution(recomputed, "modules"),
            "discarded": discarded,
        }
    return result


def measure_relocation(config: Config, d: Path, workers: int) -> dict:
    """What a relocated source tree costs against a warm cache.

    The verified plan is path-independent, so the objects hit. The object cache
    is not, so the native compile repeats. The gap between this and a warm
    build in place is what relocation actually costs.
    """
    cache = d / "relocation-cache"
    original = write_product(d / "relocation-src", PRODUCER_V1)
    measure_build(config, cache, original, d / "relocation-prime" / "app")
    before = directory_bytes(cache)
    discarded = 0
    times: list[float] = []
    peaks: list[float] = []
    hits: list[float] = []
    for sample in range(config.samples + 1):
        moved = write_product(d / f"relocation-moved-{sample}", PRODUCER_V1)
        built = measure_build(config, cache, moved, d / f"relocation-out-{sample}" / "app")
        if sample == 0:
            continue
        if not built.did_work:
            discarded += 1
            continue
        times.append(built.seconds)
        peaks.append(float(built.peak_rss))
        hits.append(float(built.report.hits))
    return {
        "seconds": distribution(times, "s"),
        "peak_rss": distribution(peaks, "bytes"),
        "hits": distribution(hits, "modules"),
        "discarded": discarded,
        "disk_bytes_before": before,
        "disk_bytes_after": directory_bytes(cache),
    }


def measure_crash_residue(config: Config, d: Path) -> dict:
    """What an unfinished writer's leftovers cost the next build.

    The residue is named the way the store names its own: a `.tmp-` prefix on
    the key. A build that read one as an object would serve a half-written
    plan, so the number that matters is that the next build still hits.
    """
    cache = d / "residue-cache"
    entry = write_product(d / "residue-src", PRODUCER_V1)
    measure_build(config, cache, entry, d / "residue-prime" / "app")
    live = objects(cache)
    discarded = 0
    times: list[float] = []
    peaks: list[float] = []
    hits: list[float] = []
    planted = 0
    for sample in range(config.samples + 1):
        for kind in ("xsm", "xtp"):
            for directory in cache.rglob(kind):
                if not directory.is_dir():
                    continue
                for path in sorted(directory.iterdir()):
                    if not path.is_file() or path.name.startswith("."):
                        continue
                    orphan = directory / f".tmp-{path.name}-{sample}"
                    orphan.write_bytes(b"half-written program target plan")
                    planted += 1
        built = measure_build(config, cache, entry, d / f"residue-out-{sample}" / "app")
        if sample == 0:
            continue
        if not built.did_work:
            discarded += 1
            continue
        times.append(built.seconds)
        peaks.append(float(built.peak_rss))
        hits.append(float(built.report.hits))
    return {
        "seconds": distribution(times, "s"),
        "peak_rss": distribution(peaks, "bytes"),
        "hits": distribution(hits, "modules"),
        "discarded": discarded,
        "residues_planted": planted,
        "live_objects_intact": objects(cache) == live,
        "disk_bytes": directory_bytes(cache),
    }


def measure_quota_gc(config: Config, d: Path) -> dict:
    """Collection is driven where the store budget is owned.

    The product entry point exposes no cache budget, so the numbers come from
    the unit suite that opens the store directly. Parsing its line here keeps
    one report rather than two, and records that this row was not measured
    through the product path.
    """
    binary = PROJECT_DIR / "build" / "tests" / "unit" / platform.exe_name(
        "test_program_plan_cache_qualification")
    if not binary.is_file():
        return {"measured": False, "reason": f"suite binary not built: {binary}"}
    rounds = []
    for _ in range(config.samples):
        result = proc.run([binary], timeout=config.timeout)
        match = COLLECT_RACE_RE.search(result.combined_text())
        if not result.ok or not match:
            return {"measured": False,
                    "reason": "suite did not report its collection race",
                    "exit": result.returncode}
        rounds.append({
            "evicted": int(match.group(1)),
            "readers": int(match.group(2)),
            "recomputed": int(match.group(3)),
        })
    return {
        "measured": True,
        "measured_through": "test_program_plan_cache_qualification",
        "reason": "the product entry point exposes no cache budget",
        "samples": len(rounds),
        "recomputed": distribution([float(r["recomputed"]) for r in rounds], "readers"),
        "rounds": rounds,
    }


def self_test() -> int:
    """Check the report parsing and statistics without building anything."""
    log = (
        "[program-closure] modules=2 dependencies=1 psc=" + "a" * 64 + " gci=" + "b" * 32 + "\n"
        "[program-semantic-plan] modules=2 entry=1\n"
        "[target-plan-cache] hit entry_0\n"
        "[module-summary] modules=2 graph=" + "c" * 64 + " xsm-hits=2 xsm-published=0 "
        "xsm-missed=0 workers=1 tasks=2 xsm-recomputed=0 xsm-artifacts=" + "d" * 64 +
        " xsm-publish-order=" + "e" * 64 + "\n"
    )
    parsed = parse(proc.ProcResult(("x",), 0, log.encode("utf-8"), b"", False))
    if not parsed or parsed.hits != 2 or parsed.plan_cache != "hit":
        print("FAIL: report parsing")
        return 1
    if parsed.identity != Identity(2, "c" * 64, "d" * 64, "a" * 64, "b" * 32):
        print("FAIL: identity fields")
        return 1
    # The publish order summarizes what this run recomputed, so it must not be
    # able to separate two reports of one program.
    other = parse(proc.ProcResult(
        ("x",), 0, log.replace("e" * 64, "f" * 64).encode("utf-8"), b"", False))
    if not other or other.identity != parsed.identity:
        print("FAIL: publish order must not reach identity")
        return 1
    if parse(proc.ProcResult(("x",), 1, b"nothing here", b"", False)) is not None:
        print("FAIL: a build with no report must not parse")
        return 1
    # The collection row is read out of the unit suite's own printout, so a
    # rename there would silently turn that row into "not measured". Pin the
    # exact line the suite emits.
    race = COLLECT_RACE_RE.search("    collect race: evicted=3 readers=3 recomputed=1\n")
    if not race or race.groups() != ("3", "3", "1"):
        print("FAIL: collection race line parsing")
        return 1
    # A spread that condemns the tail must not condemn the median with it.
    verdict = contention(
        {"seconds": {"p50": 0.7, "p95": 3.8, "samples": 7}, "discarded_pairs": 0},
        {"seconds": {"p50": 0.4, "p95": 2.9, "samples": 7}},
        {"before": [6.7], "after": [5.0]})
    if verdict["tail_usable"] or not verdict["median_usable"]:
        print(f"FAIL: contention verdict {verdict}")
        return 1
    thin = contention({"seconds": {"p50": 1.0, "p95": 1.1, "samples": 2},
                       "discarded_pairs": 1},
                      {"seconds": {"p50": 1.0, "p95": 1.1, "samples": 2}},
                      {"before": [1.0], "after": [1.0]})
    if thin["median_usable"]:
        print(f"FAIL: a run that discarded a pair must not claim its median {thin}")
        return 1
    stats = distribution([1.0, 2.0, 3.0, 4.0], "s")
    if stats["samples"] != 4 or stats["p50"] != 2.5 or stats["max"] != 4.0:
        print(f"FAIL: distribution {stats}")
        return 1
    print("program plan cache qualification self-test: PASS")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xray", nargs="?", default=None)
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--measure", action="store_true")
    parser.add_argument("--report", type=Path, default=None,
                        help="write the measurement JSON here as well as to stdout")
    parser.add_argument("--self-test", action="store_true")
    ns = parser.parse_args(argv[1:])
    if ns.self_test:
        return self_test()

    raw = ns.xray or os.environ.get("XRAY_BIN") or str(PROJECT_DIR / "build" /
                                                       platform.exe_name("xray"))
    xray = Path(raw)
    if not (xray.is_file() and os.access(xray, os.X_OK)):
        print(f"FAIL: xray binary not executable: {xray}")
        return 1
    config = Config(xray=xray, samples=max(1, ns.samples),
                    timeout=platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 600))

    rec = report.Report("program plan cache qualification")
    ws = workspace.Workspace("xray_program_plan_cache", keep=True)
    with ws:
        qualify_determinism(rec, config, ws)
        qualify_lifecycle(rec, config, ws)
        qualify_relocation(rec, config, ws)
        qualify_reorder(rec, config, ws)
        qualify_hostile(rec, config, ws)
        qualify_parallel(rec, config, ws, max(2, ns.workers))
        rec.write(verbose=True)
        if ns.measure:
            import json
            data = measure(config, ws, max(2, ns.workers))
            text = json.dumps(data, indent=2, sort_keys=True)
            print(text)
            if ns.report:
                ns.report.parent.mkdir(parents=True, exist_ok=True)
                ns.report.write_text(text + "\n", encoding="utf-8", newline="\n")
                print(f"measurement written to {ns.report}")
        ws.keep_on_exit(bool(rec.failed))
        if rec.failed:
            sys.stderr.write(f"Workdir kept: {ws.root}\n")
    return rec.exit_code


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
