#!/usr/bin/env python3
"""AOT equivalence suite: every case must behave identically under VM and AOT.

Positive cases (basic/, modules/, coro/) are built to a native binary through
the public `xray build --native` path, then run alongside `xray run` and
compared on exit code and stdout. Negative cases (negative/) must be *rejected*
by the AOT build, with a recognized diagnostic -- a rejection for the wrong
reason is a failure, not a pass.

Infrastructure comes from the shared xraytest runtime: subprocess handling,
workspaces, content-addressed caches and locks, resource-tagged parallelism,
progress. This file holds only what is specific to AOT equivalence.

Replaces run_aot_tests.sh. The binary cache is keyed by the toolchain identity
and the case directory's contents, so a warm rerun skips frontend, codegen and
link entirely; the run cache additionally records that a given (args, coro-seed)
combination already matched.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import cache, platform, proc, progress, report, scheduler, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent

POSITIVE_SECTIONS = ("basic", "modules", "coro")
NEGATIVE_SECTION = "negative"

# A native build occasionally loses a race with a concurrent toolchain probe;
# the shell runner retried three times before calling it a regression.
BUILD_ATTEMPTS = 3

# Cases whose behavior depends on argv. Keyed by file stem prefix.
CASE_ARGS = {"process_args": ("100000", "abc")}

# A negative case must be rejected *for one of these reasons*. Rejection with an
# unrecognized message means the compiler failed for an unintended cause, which
# the suite reports as a failure rather than a pass.
NEGATIVE_REJECTION_PATTERNS = (
    "unsupported .*coroutine Xi op",
    "exceptions inside AOT coroutine are unsupported",
    "unsupported Xi op ERR_",
    "semantic analysis failed",
    "derived Clone .* no consumable verified plan",
    "open AOT callable target set",
    "native compilation cannot prove the target of an indirect call",
    ": error: ",
)

Status = report.Status


@dataclass
class Config:
    xray: Path
    jobs: int
    opt_level: str
    aot_cache: Path
    bin_cache: Path
    disable_run_cache: bool
    shard_total: int
    shard_index: int
    case_timeout: float | None


@dataclass
class CaseVerdict:
    name: str
    status: str
    detail: str = ""
    excerpt: str = ""


def rel_case_name(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(SCRIPT_DIR))
    except ValueError:
        return path.name


def safe_case_name(rel: str) -> str:
    stem = rel[:-3] if rel.endswith(".xr") else rel
    return "".join(ch if ch.isalnum() or ch in "_.-" else "_" for ch in stem)


def case_args(xr_file: Path) -> tuple[str, ...]:
    stem = xr_file.stem
    for prefix, args in CASE_ARGS.items():
        if stem.startswith(prefix):
            return args
    return ()


def configure_jobs(requested: str) -> int:
    if requested in ("", "auto"):
        cap = platform.env_int("XRAY_AOT_TEST_MAX_AUTO_JOBS", 8)
        return max(1, min(platform.cpu_count(), cap))
    return int(requested) if requested.isdigit() and int(requested) > 0 else 1


def build_native(config: Config, xr_file: Path, bin_out: Path, ws: workspace.Workspace
                 ) -> tuple[bool, str]:
    """Build a case to a native binary, cached and lock-protected.

    Returns (ok, detail). The binary is content-addressed, so a warm cache skips
    the whole toolchain; only one racer builds while the rest wait on the lock.
    """
    if bin_out.is_file() and os.access(bin_out, os.X_OK):
        return True, "cached"

    bin_dir = bin_out.parent
    bin_dir.mkdir(parents=True, exist_ok=True)
    lock = cache.DirLock(bin_dir.with_name(bin_dir.name + ".lock"))
    if not lock.acquire():
        return False, "cannot lock binary cache"
    try:
        if bin_out.is_file() and os.access(bin_out, os.X_OK):
            return True, "cached"
        import threading

        tmp = bin_dir / f"aot.{os.getpid()}.{threading.get_ident()}"
        last = b""
        for _ in range(BUILD_ATTEMPTS):
            try:
                tmp.unlink()
            except OSError:
                pass
            result = proc.run(
                [config.xray, "build", "--native", "-O", config.opt_level,
                 "--cache-dir", config.aot_cache, xr_file, "-o", tmp],
                timeout=config.case_timeout,
            )
            if result.ok:
                tmp.replace(bin_out)
                return True, ""
            last = result.stdout + result.stderr
        try:
            tmp.unlink()
        except OSError:
            pass
        # Positive sections must build; a persistent failure is a regression,
        # not a skip (expected-unsupported cases live under negative/).
        return False, "native build failed after retries"
    finally:
        lock.release()


def run_positive(config: Config, xr_file: Path, ws: workspace.Workspace) -> CaseVerdict:
    rel = rel_case_name(xr_file)
    name = rel[:-3] if rel.endswith(".xr") else rel
    safe = safe_case_name(rel)
    args = case_args(xr_file)

    case_key = cache.dir_key(xr_file.parent)
    bin_dir = config.bin_cache / f"{safe}-{case_key}"
    bin_out = bin_dir / "aot"

    # The run cache records that this binary already matched for this argv and
    # coroutine-scheduling configuration; a different seed is a different run.
    run_key = cache.mix([
        "args:" + "\0".join(args),
        "XRAY_CORO_DETERMINISTIC=" + os.environ.get("XRAY_CORO_DETERMINISTIC", ""),
        "XRAY_CORO_SEED=" + os.environ.get("XRAY_CORO_SEED", ""),
    ])
    run_dir = bin_dir / f"run-{run_key}"

    ok, detail = build_native(config, xr_file, bin_out, ws)
    if not ok:
        return CaseVerdict(name, Status.FAIL.value, detail)

    if not config.disable_run_cache and (run_dir / "pass").is_file():
        return CaseVerdict(name, Status.PASS.value, "cached")

    vm = proc.run([config.xray, "run", xr_file, *(["--", *args] if args else [])],
                  timeout=config.case_timeout)
    aot = proc.run([bin_out, *args], timeout=config.case_timeout)

    if vm.timed_out or aot.timed_out:
        which = "VM" if vm.timed_out else "AOT"
        return CaseVerdict(name, Status.FAIL.value,
                           f"{which} timed out after {config.case_timeout}s")

    if vm.returncode != aot.returncode:
        return CaseVerdict(name, Status.FAIL.value,
                           f"exit code: VM={vm.returncode} AOT={aot.returncode}")
    if vm.stdout != aot.stdout:
        excerpt = ("    VM:  " + _head(vm.stdout) + "\n    AOT: " + _head(aot.stdout))
        return CaseVerdict(name, Status.FAIL.value, "output mismatch", excerpt)

    if not config.disable_run_cache:
        lock = cache.DirLock(run_dir.with_name(run_dir.name + ".lock"))
        if lock.acquire():
            try:
                if not (run_dir / "pass").is_file():
                    run_dir.mkdir(parents=True, exist_ok=True)
                    (run_dir / "vm.out").write_bytes(vm.stdout)
                    (run_dir / "aot.out").write_bytes(aot.stdout)
                    (run_dir / "vm.rc").write_text(f"{vm.returncode}\n", encoding="utf-8", newline="\n")
                    (run_dir / "aot.rc").write_text(f"{aot.returncode}\n", encoding="utf-8", newline="\n")
                    (run_dir / "pass").touch()
            finally:
                lock.release()
    return CaseVerdict(name, Status.PASS.value)


def _head(data: bytes, lines: int = 5) -> str:
    return "|".join(data.decode("utf-8", "replace").splitlines()[:lines])


def run_negative(config: Config, xr_file: Path, ws: workspace.Workspace) -> CaseVerdict:
    """A negative case must fail to build, and fail for a recognized reason."""
    import re

    rel = rel_case_name(xr_file)
    name = rel[:-3] if rel.endswith(".xr") else rel
    safe = safe_case_name(rel)
    case_work = ws.subdir(safe)
    build_input = xr_file

    # A negative may need an external reachability/link contract. A companion
    # <case>.toml is staged as the project's xray.toml so cases stay isolated.
    manifest = xr_file.with_suffix(".toml")
    if manifest.is_file():
        shutil.copy2(xr_file, case_work / xr_file.name)
        shutil.copy2(manifest, case_work / "xray.toml")
        build_input = case_work / xr_file.name

    result = proc.run(
        [config.xray, "build", "--native", "-O", config.opt_level,
         "--cache-dir", config.aot_cache, build_input, "-o", case_work / "aot"],
        timeout=config.case_timeout,
    )
    if result.timed_out:
        return CaseVerdict(name, Status.FAIL.value, f"timed out after {config.case_timeout}s")
    if result.ok:
        return CaseVerdict(name, Status.FAIL.value, "unexpected AOT success")

    log = (result.stdout + result.stderr).decode("utf-8", "replace")
    for pattern in NEGATIVE_REJECTION_PATTERNS:
        if re.search(pattern, log):
            return CaseVerdict(name, Status.PASS.value, "rejected")
    return CaseVerdict(name, Status.FAIL.value, "wrong rejection",
                       "    " + "|".join(log.splitlines()[:5]))


def read_tombstones(path: Path) -> "set[str]":
    """Case names from a TOMBSTONES.tsv: first field, skipping comments/header.

    A tombstone classifies a known-failing verification asset. The inventory may
    only shrink, which is the same only-shrink policy xraytest.ratchet encodes;
    this file just supplies the entries and a size ceiling.
    """
    names: "set[str]" = set()
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        first = line.split()[0]
        if first == "case":  # header row
            continue
        names.add(first)
    return names


def tombstone_gate(tomb_file: Path, verdicts: list[CaseVerdict],
                   max_allowed: "int | None") -> int:
    """Fail-closed gate: exactly the tombstoned cases may fail.

    Any other failure fails the gate; a tombstoned case that now passes is
    reported so the inventory can be pruned. The size ceiling catches an
    inventory that grows instead of shrinking.
    """
    from xraytest import ratchet

    tombstoned = read_tombstones(tomb_file)
    if max_allowed is not None and len(tombstoned) > max_allowed:
        sys.stderr.write(
            f"error: tombstone inventory grew: {len(tombstoned)} > {max_allowed}\n"
            "Tombstones may only decrease; fix or rewrite the new failure.\n")
        return 1

    failing = {v.name for v in verdicts if v.status == Status.FAIL.value}
    skipped = {v.name for v in verdicts if v.status == Status.SKIP.value}
    verdict = ratchet.evaluate(failed=failing, baseline=tombstoned, skipped=skipped)

    print("")
    print("=== AOT tombstone gate ===")
    print(f"Tombstoned cases:   {len(tombstoned)}")
    print(f"Failing cases:      {len(failing)}")
    if verdict.now_passing:
        print("Resolved tombstones (now passing -- prune from TOMBSTONES.tsv):")
        for name in verdict.now_passing:
            print(f"  - {name}")
    if verdict.new_failures:
        print("Unexpected (non-tombstoned) failures:")
        for name in verdict.new_failures:
            print(f"  - {name}")
        print("=== GATE: FAIL ===")
        return 1
    print("=== GATE: PASS (only tombstoned cases failed) ===")
    return 0


def collect(section: str) -> list[Path]:
    directory = SCRIPT_DIR / section
    if not directory.is_dir():
        return []
    return sorted(p for p in directory.glob("*.xr") if not p.name.startswith("_"))


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="AOT VM/native equivalence suite")
    ap.add_argument("--xray", default=None)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("xray_positional", nargs="?", default=None)
    ns = ap.parse_args(argv[1:])

    xray_raw = ns.xray or ns.xray_positional or os.environ.get("XRAY_BIN")
    if not xray_raw:
        build_dir = os.environ.get("XRAY_BUILD_DIR")
        xray_raw = str(Path(build_dir) / "xray") if build_dir else str(PROJECT_DIR / "build" / "xray")
    xray = Path(xray_raw)

    opt_level = os.environ.get("XRAY_AOT_TEST_OPT", "0")
    shard_total = platform.env_int("XRAY_AOT_SHARD_TOTAL", 1)
    shard_index = int(os.environ.get("XRAY_AOT_SHARD_INDEX", "0") or 0)
    if shard_index >= shard_total:
        print(f"error: shard index {shard_index} out of range for total {shard_total}",
              file=sys.stderr)
        return 2

    config = Config(
        xray=xray,
        jobs=configure_jobs(os.environ.get("XRAY_AOT_TEST_JOBS",
                                           os.environ.get("XRAY_TEST_JOBS", "auto"))),
        opt_level=opt_level,
        aot_cache=cache.stable_cache_dir(PROJECT_DIR, "aot-objects", xray) / f"O{opt_level}",
        bin_cache=cache.stable_cache_dir(PROJECT_DIR, "aot-test-bin", xray) / f"O{opt_level}",
        disable_run_cache=platform.env_flag("XRAY_TEST_DISABLE_RUN_CACHE"),
        shard_total=shard_total,
        shard_index=shard_index,
        case_timeout=platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300),
    )

    print("=== AOT Tests (VM / native equivalence) ===")
    print(f"Binary: {xray}")
    print(f"Jobs:   {config.jobs}")
    print(f"Opt:    -O{opt_level}")
    print(f"Cache:  {config.bin_cache}")
    if shard_total > 1:
        print(f"Shard:  {shard_index} / {shard_total}")
    print("")

    if not (xray.is_file() and os.access(xray, os.X_OK)):
        print(f"FAIL: xray binary not executable: {xray}")
        print("=== Results: 0 passed, 1 failed, 0 skipped ===")
        return 1

    cases_by_section = {
        section: collect(section)
        for section in (*POSITIVE_SECTIONS, NEGATIVE_SECTION)
    }
    empty_sections = [
        section for section, cases in cases_by_section.items() if not cases
    ]
    if empty_sections:
        print(
            "FAIL: governed AOT sections have no discovered cases: "
            + ", ".join(empty_sections)
        )
        print("=== Results: 0 passed, 1 failed, 0 skipped ===")
        return 1

    positive = [
        (section, path)
        for section in POSITIVE_SECTIONS
        for path in cases_by_section[section]
    ]
    negative = [
        (NEGATIVE_SECTION, path)
        for path in cases_by_section[NEGATIVE_SECTION]
    ]
    everything = positive + negative
    if shard_total > 1:
        everything = [item for i, item in enumerate(everything)
                      if i % shard_total == shard_index]
    if not everything:
        print(
            "FAIL: selected AOT shard has no discovered cases: "
            f"index={shard_index} total={shard_total}"
        )
        print("=== Results: 0 passed, 1 failed, 0 skipped ===")
        return 1

    verdicts: list[CaseVerdict] = []
    with workspace.Workspace("xray_aot_tests") as ws:
        reporter = progress.ProgressReporter(len(everything))

        def one(section: str, path: Path) -> CaseVerdict:
            if section == NEGATIVE_SECTION:
                return run_negative(config, path, ws)
            return run_positive(config, path, ws)

        if config.jobs <= 1:
            for section, path in everything:
                v = one(section, path)
                verdicts.append(v)
                reporter.tick(v.name)
        else:
            sched = scheduler.Scheduler({scheduler.CPU: config.jobs})
            tasks = [
                scheduler.Task(key=str(i), fn=(lambda s=sec, p=path: one(s, p)),
                               tag=scheduler.CPU)
                for i, (sec, path) in enumerate(everything)
            ]
            by_key = sched.run(tasks, on_done=lambda k, r: reporter.tick(getattr(r, "name", "")))
            for i in range(len(everything)):
                value = by_key.get(str(i))
                if isinstance(value, BaseException):
                    raise value
                verdicts.append(value)
        reporter.finish()

    passed = failed = skipped = 0
    for v in verdicts:
        line = f"  {v.name:<42}"
        if v.status == Status.PASS.value:
            line += "PASS" + (f" ({v.detail})" if v.detail else "")
            passed += 1
        elif v.status == Status.SKIP.value:
            line += f"SKIP ({v.detail})"
            skipped += 1
        else:
            line += f"FAIL ({v.detail})"
            failed += 1
        print(line)
        if v.excerpt and v.status == Status.FAIL.value:
            print(v.excerpt)

    print("")
    print(f"=== Results: {passed} passed, {failed} failed, {skipped} skipped ===")

    # With a tombstone inventory the gate decides the exit code: exactly the
    # listed cases may fail. Without one, any failure fails the run.
    tomb_raw = os.environ.get("XRAY_AOT_TOMBSTONE_FILE")
    if tomb_raw:
        tomb_file = Path(tomb_raw)
        if not tomb_file.is_file():
            sys.stderr.write(f"error: tombstone file not found: {tomb_file}\n")
            return 2
        max_raw = os.environ.get("XRAY_AOT_TOMBSTONE_MAX")
        if max_raw is not None and not max_raw.isdigit():
            sys.stderr.write("error: XRAY_AOT_TOMBSTONE_MAX must be a non-negative integer\n")
            return 2
        return tombstone_gate(tomb_file, verdicts,
                              int(max_raw) if max_raw is not None else None)

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
