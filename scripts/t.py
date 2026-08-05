#!/usr/bin/env python3
"""Tiered test runner.

WHY TIERS, AND WHY THIS PARTICULAR CUT

A full `ctest -j16` run is ~8 minutes, and the cost is not spread across the
tests -- a handful of lanes decide the wall time because each one declares most
of the machine (PROCESSORS 4..16, or RUN_SERIAL) and so cannot overlap with the
others. Measured, 18 cores:

    backend_diff                    141s
    asan_focused                    139s   (RUN_SERIAL; builds an ASan compiler)
    aot_filetests                   105s
    aot_standalone_suite             70s
    ------------------------------------
    ~250 in-process tests            <1s   each; ~20s for all of them together

The load-bearing property is NOT "how many tests" and not even "does this test
spawn a C compiler" -- it is "does this test occupy the whole machine". While
one of those lanes runs, everything scheduled beside it is starved, and ctest
charges that starvation to the innocent test: test_arena reports 0.02s on its
own and 9.85s inside a full run. That is the cut below.

Each tier is a superset of the one before it. Nothing is sampled or truncated
inside a tier by default -- a tier either runs a suite completely or does not
claim it -- and every run prints what it did not cover, so a green t0 is never
mistaken for a green suite. XR_SHARDS can trade corpus coverage for speed, but
it is opt-in, it says so on every run, and t3 refuses it.

The build step builds exactly the targets the selected tests need -- a full
build's correctness without a full build's ~195 links.

USAGE                                          measured, warm tree, 18 cores
    scripts/t.py t0      after an edit         ~23s  (build + 255 tests)
    scripts/t.py t0 -R <re>  one test          ~3s   (builds only that test)
    scripts/t.py t1      before a commit       ~1min
    scripts/t.py t2      before a push         ~4min
    scripts/t.py t3      periodic / release    everything, ~8min
    scripts/t.py auto    pick a tier from the working-tree diff

Extra arguments are forwarded to ctest, e.g.
    scripts/t.py t1 --rerun-failed
    scripts/t.py t0 -R parser

Environment:
    XR_BUILD_DIR   build directory (default: build)
    XR_JOBS        parallelism (default: cores - 2)
    XR_NO_BUILD=1  skip the incremental build step
    XR_FAST=1      t0/t1 only: build in build-fast without the stdlib VM
                   fastpaths. That generator is a single ~70s serial edge that
                   reruns after any src/ edit; dropping it takes the same
                   one-file rebuild from 67.0s to 1.4s. Semantics are
                   unaffected (the corpus fails identically), but t2/t3 gate
                   the fastpaths themselves and reject the flag.
    XR_SHARDS=N    run only 1/N of the two big corpora (backend diff, AOT
                   filetests). OFF by default and deliberately so: t2 exists
                   precisely to catch a backend divergence, and a tier that
                   silently samples is a tier that silently stops catching
                   them. Use it when you knowingly want a faster t2 and accept
                   that 1-1/N of those cases did not run -- the shard is stable
                   (same cases every time for a given N) and every run prints
                   what it skipped. t3 ignores it; a release tier runs
                   everything.
    XR_SHARD_INDEX which shard to run with XR_SHARDS (default: 0)
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Set, Tuple


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402

# This script narrates around children that write straight to fd 1 (ctest, the
# build). Block-buffered output would land after theirs and scramble the
# report, so match a shell's line-at-a-time behaviour.
sys.stdout.reconfigure(line_buffering=True)

REPO_ROOT = Path(__file__).resolve().parent.parent

USE_COLOR = sys.stdout.isatty() and not os.environ.get("NO_COLOR")
RED = "\033[0;31m" if USE_COLOR else ""
GREEN = "\033[0;32m" if USE_COLOR else ""
YELLOW = "\033[0;33m" if USE_COLOR else ""
BLUE = "\033[0;34m" if USE_COLOR else ""
BOLD = "\033[1m" if USE_COLOR else ""
NC = "\033[0m" if USE_COLOR else ""

# Tests that drive an external C toolchain and, in doing so, saturate the
# machine. Excluded below t3 -- not because they matter less, but because each
# one costs more than every in-process test combined AND starves whatever ctest
# schedules alongside it. Ordered by measured cost.
SLOW_EXTERNAL = ("aot_standalone_suite|asan_focused|lsan_strict|aot_ubsan|"
                 "test_string_native_error_abi|test_crypto_native_error_abi|"
                 "test_param_contract_aot|test_compress_native_error_abi")

# QEMU-backed cross targets: slow, and unavailable on most developer machines.
SLOW_QEMU = ("aot_freestanding_qemu_smoke|aot_freestanding_riscv_qemu_smoke|"
             "aot_freestanding_thumb_qemu_smoke|aot_cross_smoke|"
             "aot_bundled_zig_smoke")

# Everything that runs in-process: unit tests plus the static gates. No
# toolchain spawn.
T0_INCLUDE = (r"^(test_|.*_residue$|.*_convergence.*|.*_sync$|.*_inventory.*|"
              r"harness_|check_|stdlib_boundary_|stdlib_def_|stdlib_metadata|"
              r"contract_freeze|surface_drift)")

TIERS: Dict[str, Tuple[str, str, str]] = {
    # tier: (include, exclude, not_covered)
    "t0": (T0_INCLUDE, f"{SLOW_EXTERNAL}|{SLOW_QEMU}",
           "VM/AOT differential, regression corpus, AOT suites, sanitizers"),
    # + the VM-executed corpora: regression, syntax, bytecode, stdlib.
    "t1": ("", f"{SLOW_EXTERNAL}|{SLOW_QEMU}|^backend_diff|^task190_|^aot_|"
           "^ffi_|^install_|^native_output|^binary_|^dap_|^raw_scalar|"
           "^global_evidence|^byte_array_aot",
           "VM/AOT differential, AOT suites, sanitizers, QEMU cross"),
    # + differential and AOT, including the generated-C UBSan lane. This is the
    # tier that can actually catch a backend divergence.
    "t2": ("", f"aot_standalone_suite|asan_focused|lsan_strict|{SLOW_QEMU}",
           "ASan/LSan lanes, aot_standalone_suite, QEMU cross targets"),
    "t3": ("", "", ""),
}

TEST_NAME_RE = re.compile(r"^\s*Test\s*#[0-9]+:\s*(\S+)")
NINJA_TARGET_RE = re.compile(r"^([A-Za-z0-9_][A-Za-z0-9_.-]*): phony")
BACKEND_TOUCHED = re.compile(r"^src/(aot|ir|coro|vm|runtime)/|^CMakeLists\.txt$|^xisa/")

REGRESSION_BASELINE = REPO_ROOT / "tests" / "regression" / "baseline_failures.txt"


def usage(code: int = 0) -> int:
    doc = __doc__ or ""
    start = doc.find("USAGE")
    print(doc[start:].split("\n\nEnvironment:")[0] if start >= 0 else doc)
    return code


def default_jobs() -> int:
    cores = platform.cpu_count()
    return max(1, cores - 2) if cores > 3 else 1


def git_lines(args: Sequence[str]) -> List[str]:
    result = proc.run(["git", *args], cwd=REPO_ROOT)
    if not result.ok:
        return []
    return [line for line in result.stdout.decode("utf-8", "replace").splitlines()
            if line.strip()]


def choose_tier_from_diff() -> str:
    """Pick a TIER from what the working tree touches, not a bespoke test list.

    Tier selection is a coverage floor that can be reasoned about; a hand-picked
    per-change test list is where coverage goes missing without anyone noticing.
    """
    changed = sorted(set(git_lines(["diff", "--name-only", "HEAD"])
                         + git_lines(["ls-files", "--others", "--exclude-standard"])))
    if not changed:
        print("auto: working tree is clean — running t0")
        return "t0"

    if any(BACKEND_TOUCHED.search(path) for path in changed):
        tier = "t2"
        print(f"auto: backend / runtime / build changes — running {tier}")
    elif any(path.startswith("src/") for path in changed):
        tier = "t1"
        print(f"auto: compiler source changes — running {tier}")
    else:
        tier = "t0"
        print(f"auto: no compiler source changes — running {tier}")

    for path in changed[:12]:
        print(f"       {path}")
    if len(changed) > 12:
        print("       ...")
    return tier


def ctest_names(build_dir: Path, args: Sequence[str]) -> List[str]:
    result = proc.run(["ctest", "-N", *args], cwd=build_dir)
    names = []
    for line in result.stdout.decode("utf-8", "replace").splitlines():
        match = TEST_NAME_RE.match(line)
        if match:
            names.append(match.group(1))
    return names


def build_selected(build_dir: Path, selected: Sequence[str], jobs: int) -> bool:
    """Build exactly what this run needs.

    Every tier needs current binaries; running a tier against a stale one is
    worse than not running it at all. Building everything fixes that but links
    ~195 executables on every iteration, so resolve the selected test names to
    build targets and build only those, plus `xray`, which the script-driven
    tests invoke. A selected test with no target of the same name is a
    script/gate test and needs nothing beyond `xray`.
    """
    targets: List[str] = ["xray"]
    known = proc.run(["ninja", "-C", str(build_dir), "-t", "targets", "all"])
    if known.ok:
        available = {match.group(1)
                     for match in (NINJA_TARGET_RE.match(line) for line in
                                   known.stdout.decode("utf-8", "replace").splitlines())
                     if match}
        targets.extend(sorted(set(selected) & available))
    else:
        # No target list available: fall back to a full build rather than
        # silently under-building and testing stale binaries.
        targets = []

    if targets:
        print(f"{BLUE}==>{NC} building {len(targets)} target(s)")
        argv = ["cmake", "--build", str(build_dir), "-j", str(jobs)]
        for target in targets:
            argv += ["--target", target]
    else:
        print(f"{BLUE}==>{NC} building (all targets)")
        argv = ["cmake", "--build", str(build_dir), "-j", str(jobs)]

    log = build_dir / ".t-build.log"
    result = proc.run(argv)
    platform.write_text_lf(log, result.combined_text())
    if result.ok:
        return True
    print(f"{RED}BUILD FAILED{NC}")
    shown = 0
    for line in result.combined_text().splitlines():
        if "error:" in line:
            print(line)
            shown += 1
            if shown >= 20:
                break
    return False


def run_regression_corpus(build_dir: Path, skip_diff: bool) -> bool:
    """Gate the tests/regression corpus against an only-shrink ratchet.

    The corpus has no ctest entry: it only ever ran in one non-blocking nightly
    lane, and cases had rotted unnoticed. Any failure not in the baseline fails
    the tier, and a baseline entry that starts passing fails too, so the list
    can only shrink.
    """
    suffix = ", VM only" if skip_diff else ""
    print(f"{BLUE}==>{NC} regression corpus{suffix}")

    env = dict(os.environ)
    env["XRAY_SKIP_BACKEND_DIFF"] = "1" if skip_diff else "0"
    env["XRAY_BIN"] = str(build_dir / platform.exe_name("xray"))
    env["XRAY_BUILD_DIR"] = str(build_dir)

    with workspace.Workspace("xray_t_regression") as ws:
        report = ws.path("regression.json")
        result = proc.run([sys.executable,
                           REPO_ROOT / "scripts" / "run_regression_tests.py",
                           "--json", report], env=env, cwd=REPO_ROOT)
        text = result.combined_text()
        for line in text.splitlines():
            if line.startswith(("总文件数", "通过", "失败")):
                print(f"    {line}")
        # Structured result, not a grep over a localized summary: the runner
        # owns the format and this reads it.
        if not report.is_file():
            print(f"{RED}    regression: runner produced no report{NC}")
            sys.stdout.write(text)
            return False
        actual = set(json.loads(report.read_text(encoding="utf-8"))["failed_tests"])

    expected = {line.strip()
                for line in REGRESSION_BASELINE.read_text(encoding="utf-8").splitlines()
                if line.strip() and not line.startswith("#")}

    newly_broken = sorted(actual - expected)
    newly_fixed = sorted(expected - actual)
    ok = True
    if newly_broken:
        print(f"{RED}    regression: newly broken{NC}")
        for name in newly_broken:
            print(f"      {name}")
        ok = False
    if newly_fixed:
        print(f"{YELLOW}    regression: now passing — delete these from "
              f"{REGRESSION_BASELINE.relative_to(REPO_ROOT)}{NC}")
        for name in newly_fixed:
            print(f"      {name}")
        ok = False
    if ok:
        print(f"{GREEN}    regression: no change against baseline{NC}")
    return ok


def main(argv: List[str]) -> int:
    if len(argv) < 2:
        return usage(1)
    tier = argv[1]
    extra = argv[2:]
    if tier in ("-h", "--help", "help"):
        return usage(0)

    if tier == "auto":
        tier = choose_tier_from_diff()
    if tier not in TIERS:
        print(f"Unknown tier '{tier}'")
        return usage(1)

    build_dir = Path(os.environ.get("XR_BUILD_DIR", "build"))
    jobs = platform.env_int("XR_JOBS", default_jobs())

    # XR_FAST drops the hosted VM stdlib fastpath generator from the edit cycle.
    # That generator is one ~70s edge that cannot be parallelized and reruns
    # whenever xray_stdlib_bcgen relinks -- after ANY src/ edit, even when its
    # output is byte-identical. It is safe to drop below t2 because the
    # fastpaths are a VM performance layer, not a semantic one. It is NOT safe
    # at t2/t3, which exist to gate the generated fastpaths themselves.
    if platform.env_flag("XR_FAST"):
        if tier in ("t2", "t3"):
            print(f"{RED}Error{NC}: XR_FAST=1 is not accepted by {tier}.")
            print(f"       {tier} gates the stdlib VM fastpaths; a tree built "
                  "without")
            print("       them would report a pass those tiers never established.")
            return 1
        build_dir = Path(os.environ.get("XR_BUILD_DIR", "build-fast"))
        if not (build_dir / "CMakeCache.txt").is_file():
            print(f"{BLUE}==>{NC} configuring {build_dir} (no stdlib VM fastpaths)")
            configure = proc.run(["cmake", "-S", str(REPO_ROOT), "-B", str(build_dir),
                                  "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                                  "-DXRAY_STDLIB_VM_FASTPATHS=OFF",
                                  "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"])
            if not configure.ok:
                return 1
        print(f"{YELLOW}XR_FAST{NC}: using {build_dir} without stdlib VM fastpaths")

    shards = platform.env_int("XR_SHARDS", 1)
    shard_index = platform.env_int("XR_SHARD_INDEX", 0)
    if shards < 1:
        print(f"{RED}Error{NC}: XR_SHARDS must be >= 1")
        return 1
    if not 0 <= shard_index < shards:
        print(f"{RED}Error{NC}: XR_SHARD_INDEX must be in [0,{shards})")
        return 1
    if shards > 1 and tier == "t3":
        print(f"{RED}Error{NC}: XR_SHARDS is not allowed for t3 — a release "
              "tier runs every case.")
        return 1

    if not build_dir.is_dir():
        print(f"{RED}Error{NC}: build directory '{build_dir}' does not exist.")
        print("Configure one first:  cmake --preset default   "
              "(Ninja + Release in build/)")
        return 1

    print(f"{BOLD}tier {tier}{NC}  build={build_dir}  jobs={jobs}")
    print("=" * 72)
    started = time.time()

    include, exclude, not_covered = TIERS[tier]
    ctest_args = ["--output-on-failure", "-j", str(jobs)]
    if include:
        ctest_args += ["-R", include]
    if exclude:
        ctest_args += ["-E", exclude]

    selected = ctest_names(build_dir, ctest_args + extra)
    total = len(ctest_names(build_dir, []))
    print(f"{BLUE}==>{NC} ctest: {len(selected)}/{total} tests")

    if not platform.env_flag("XR_NO_BUILD"):
        if not build_selected(build_dir, selected, jobs):
            return 1

    env = dict(os.environ)
    if shards > 1:
        # Both big corpora already implement a stable 0-based shard of their
        # case list, so this only forwards the request.
        env.update({
            "XRAY_DIFF_SHARD_TOTAL": str(shards),
            "XRAY_DIFF_SHARD_INDEX": str(shard_index),
            "XRAY_AOT_SHARD_TOTAL": str(shards),
            "XRAY_AOT_SHARD_INDEX": str(shard_index),
        })
        print(f"{YELLOW}==>{NC} sharding: backend-diff and AOT corpora run "
              f"shard {shard_index}/{shards} only")
        dropped = f"{100 - 100 // shards}% of the backend-diff and AOT filetest cases (XR_SHARDS={shards})"
        not_covered = f"{not_covered}, {dropped}" if not_covered else dropped

    code = subprocess.call(["ctest", *ctest_args, *extra],
                           cwd=str(build_dir), env=env)

    if tier == "t1" and not run_regression_corpus(build_dir, True):
        code = 1
    elif tier in ("t2", "t3") and not run_regression_corpus(build_dir, False):
        code = 1

    # t0 additionally runs the compile-error corpus: the fastest broad check of
    # parser and analyzer diagnostics there is.
    if tier == "t0" and code == 0:
        print(f"{BLUE}==>{NC} compile-error corpus")
        env["XRAY_BIN"] = str(build_dir / platform.exe_name("xray"))
        corpus = proc.run([sys.executable, REPO_ROOT / "tests" / "compile_errors"
                           / "run_compile_error_tests.py"], env=env, cwd=REPO_ROOT)
        for line in corpus.combined_text().splitlines()[-6:]:
            print(line)
        if not corpus.ok:
            code = 1

    elapsed = int(time.time() - started)
    print("=" * 72)
    verdict = f"{GREEN}PASS{NC}" if code == 0 else f"{RED}FAIL{NC}"
    print(f"tier {BOLD}{tier}{NC}  {verdict}  {elapsed // 60}m{elapsed % 60:02d}s")

    # A tier that stayed quiet about its own limits is how a green check turns
    # into false confidence. Say what was not covered, every time.
    if not_covered:
        print(f"{YELLOW}not covered by {tier}{NC}: {not_covered}")
        nxt = {"t0": "t1", "t1": "t2", "t2": "t3"}.get(tier)
        if nxt:
            print(f"  next: scripts/t.py {nxt}")

    return code


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
