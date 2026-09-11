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
    scripts/t.py t0      after an edit         exact bounded smoke inventory
    scripts/t.py infra   test-runner-only edit exact Python self-tests
    scripts/t.py t0 -R <re>  one test          ~3s   (builds only that test)
    scripts/t.py canonical  canonical Program edit preflight; exact inventory
    scripts/t.py h2        shared H2 semantics; exact deduplicated aggregate
    scripts/t.py h2-reference  CoreSpec + Reference/verifier inner loop
    scripts/t.py h2-source  Program/Reference/source-owner exact gate
    scripts/t.py h2-vm      VM-private exact gate
    scripts/t.py h2-aot     AOT-private exact gate
    scripts/t.py t1      before a commit       ~1min
    scripts/t.py t2      before a push         ~4min
    scripts/t.py t3      periodic / release    everything, ~8min
    scripts/t.py auto    pick a tier from the working-tree diff

Extra arguments are forwarded to ctest, e.g.
    scripts/t.py t1 --rerun-failed
    scripts/t.py t0 -R parser

An empty test selection is an error, including after a build refresh. CTest
execution also enforces --no-tests=error if the inventory changes again.

Each run reports toolchain setup, configure/manifest, incremental build, CTest,
and auxiliary-corpus wall times separately. Build time includes generated-source
validation; use .ninja_log to distinguish those checks from compile/link time.
The total includes toolchain setup and fast-tree configuration, but not Python
process startup. A skipped or unreached phase has no timing record.

Environment:
    XR_BUILD_DIR   build directory (default: build)
    XR_JOBS        build parallelism (default: cores - 2)
    XR_CTEST_JOBS  CTest parallelism (default: XR_JOBS, capped at 8 on Windows
                   where process/toolchain contention makes higher values slower)
    XR_BUILD_LOCK_TIMEOUT seconds to wait for exclusive ownership of the build
                   tree (default: 600; 0 fails immediately)
    XR_NO_BUILD=1  skip the incremental build step
    XR_FAST=1      t0/t1/exact preflights: build in build-fast (build-fast-clang on
                   Windows), load stdlib source
                   from disk, and omit stdlib VM fastpaths. This removes both
                   self-hosted stdlib generation edges from the edit loop; the
                   fastpath edge alone costs ~70s after any src/ edit. t2/t3
                   gate embedded stdlib and fastpaths and reject the flag.
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
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Sequence, Tuple


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import buildlock, platform, proc, sanitizer, workspace  # noqa: E402
import canonical_program_test_profile as canonical_profile  # noqa: E402

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

# Exhaustive mutation suites consume wall time without saturating the machine.
# Keep their bounded fast profiles in edit/pre-commit tiers and reserve the
# complete inventories for the periodic/release tier.
SLOW_EXHAUSTIVE = "xi_generator_self_test"
XI_GENERATOR_FAST = "xi_generator_fast_self_test"

# QEMU-backed cross targets: slow, and unavailable on most developer machines.
SLOW_QEMU = ("aot_freestanding_qemu_smoke|aot_freestanding_riscv_qemu_smoke|"
             "aot_freestanding_thumb_qemu_smoke|aot_cross_smoke|"
             "aot_bundled_zig_smoke")

# T0 is an edit-feedback contract, not "every test whose current name happens
# to start with test_".  The old open-ended regex grew from 255 to 458 CTests
# and forced 344 executable links.  Keep this inventory explicit and compose
# it with the canonical Program registry so new source fixtures enter through
# one machine-owned source of truth.
T0_EXTRA_CTEST_NAMES = (
    "expected_format_self_test",
    "stdlib_analyzer_builtins_sync",
    "test_analyzer",
    "test_build_tree_lock",
    "test_canonical_program_test_profile",
    "test_tiered_test_runner",
)
T0_CTEST_NAMES = tuple(dict.fromkeys(
    canonical_profile.CTEST_NAMES + T0_EXTRA_CTEST_NAMES))


def exact_ctest_regex(names: Sequence[str]) -> str:
    return "^(" + "|".join(re.escape(name) for name in names) + ")$"


T0_INCLUDE = exact_ctest_regex(T0_CTEST_NAMES)

TIERS: Dict[str, Tuple[str, str, str]] = {
    # tier: (include, exclude, not_covered)
    "t0": (T0_INCLUDE, f"{SLOW_EXTERNAL}|{SLOW_QEMU}|{SLOW_EXHAUSTIVE}",
           "unselected unit/meta tests, full compile-error corpus, exhaustive Xi "
           "generator mutations, VM/AOT differential, regression corpus, broad "
           "AOT suites and sanitizers"),
    # + the VM-executed corpora: regression, syntax, bytecode, stdlib.
    "t1": ("", f"{SLOW_EXTERNAL}|{SLOW_QEMU}|{SLOW_EXHAUSTIVE}|^backend_diff|^task190_|^aot_|"
           "^ffi_|^install_|^native_output|^binary_|^dap_|^raw_scalar|"
           "^global_evidence|^byte_array_aot",
           "exhaustive Xi generator mutations, VM/AOT differential, AOT suites, "
           "sanitizers, QEMU cross"),
    # + differential and AOT, including the generated-C UBSan lane. This is the
    # tier that can actually catch a backend divergence.
    "t2": ("", f"aot_standalone_suite|asan_focused|lsan_strict|{SLOW_QEMU}|{SLOW_EXHAUSTIVE}",
           "exhaustive Xi generator mutations, ASan/LSan lanes, "
           "aot_standalone_suite, QEMU cross targets"),
    "t3": ("", "", ""),
}

TEST_NAME_RE = re.compile(r"^\s*Test\s*#[0-9]+:\s*(\S+)")
NINJA_TARGET_RE = re.compile(r"^([A-Za-z0-9_][A-Za-z0-9_.-]*): phony")
BACKEND_TOUCHED = re.compile(r"^src/(aot|ir|coro|vm|runtime)/|^CMakeLists\.txt$|^xisa/")
FOCUSED_CTEST_OPTIONS = {
    "-R", "--tests-regex", "-L", "--label-regex", "-I", "--tests-information",
    "--rerun-failed", "--tests-from-file",
}

REGRESSION_BASELINE = REPO_ROOT / "tests" / "regression" / "baseline_failures.txt"
PRODUCTION_CACHE_VALUES = {
    "CMAKE_GENERATOR": "Ninja",
    "CMAKE_BUILD_TYPE": "Release",
    "XR_STDLIB_FROM_FILE": "OFF",
    "XRAY_STDLIB_VM_FASTPATHS": "ON",
    "ENABLE_ASAN": "OFF",
    "ENABLE_UBSAN": "OFF",
    "ENABLE_TSAN": "OFF",
    "ENABLE_MSAN": "OFF",
}


@dataclass(frozen=True)
class ExactProfile:
    tests: Tuple[str, ...]
    targets: Tuple[str, ...]
    include_xray: bool
    not_covered: str
    allow_no_build_targets: bool = False


EXACT_PROFILES = {
    "canonical": ExactProfile(
        tests=canonical_profile.CTEST_NAMES,
        targets=canonical_profile.BUILD_TARGETS,
        include_xray=False,
        not_covered=("known-red terminal readiness/residue gates, broad language/runtime "
                     "suites, full backend differential, full ASan/LSan, QEMU and release "
                     "qualification"),
    ),
    "generic-identity": ExactProfile(
        tests=canonical_profile.GENERIC_IDENTITY_CTEST_NAMES,
        targets=canonical_profile.GENERIC_IDENTITY_BUILD_TARGETS,
        include_xray=False,
        not_covered=("remaining canonical Program proofs, broad language/runtime suites, "
                     "VM/AOT differential, full sanitizers and release qualification"),
    ),
    "h2": ExactProfile(
        tests=canonical_profile.H2_CTEST_NAMES,
        targets=canonical_profile.H2_BUILD_TARGETS,
        include_xray=False,
        not_covered=("H2.3/H2.4 exit, graph-copy and cycle cases, the remaining canonical "
                     "product/native-fixture inventory, broad language/runtime suites, the "
                     "full t2 backend differential, full ASan/LSan, QEMU and release "
                     "qualification"),
    ),
    "h2-reference": ExactProfile(
        tests=canonical_profile.H2_REFERENCE_CTEST_NAMES,
        targets=canonical_profile.H2_REFERENCE_BUILD_TARGETS,
        include_xray=False,
        not_covered=("source-to-Xi projection, source fixture matrix, VM fixed/decoded paths, "
                     "AOT/native parity, product routes, broad contracts, full sanitizers and "
                     "release qualification"),
    ),
    "h2-source": ExactProfile(
        tests=canonical_profile.H2_SOURCE_CTEST_NAMES,
        targets=canonical_profile.H2_SOURCE_BUILD_TARGETS,
        include_xray=False,
        not_covered=("VM/AOT execution of pending class-reference fixtures, product routes, "
                     "broad language/runtime suites, full sanitizers and release qualification"),
    ),
    "h2-vm": ExactProfile(
        tests=canonical_profile.H2_VM_CTEST_NAMES,
        targets=canonical_profile.H2_VM_BUILD_TARGETS,
        include_xray=False,
        not_covered=("source fixture matrix, AOT/native parity, product routes, broad contracts, "
                     "full sanitizers and release qualification"),
    ),
    "h2-aot": ExactProfile(
        tests=canonical_profile.H2_AOT_CTEST_NAMES,
        targets=canonical_profile.H2_AOT_BUILD_TARGETS,
        include_xray=False,
        not_covered=("source fixture matrix, VM parity, native class-reference fixtures, product "
                     "routes, full sanitizers and release qualification"),
    ),
    "infra": ExactProfile(
        tests=("test_build_tree_lock", "test_tiered_test_runner",
               "test_canonical_program_test_profile"),
        targets=(),
        include_xray=False,
        allow_no_build_targets=True,
        not_covered=("all product compiler/runtime tests, compile-error corpora, native "
                     "toolchains, sanitizers and release qualification"),
    ),
}

INFRA_EDIT_PATH = re.compile(
    r"^(scripts/t\.py|tests/lib/xraytest/buildlock\.py|"
    r"tests/lib/tests/test_(t_runner|buildlock)\.py)$")
CANONICAL_EDIT_PATH = re.compile(
    r"^(src/program/|src/aot/program/|src/vm/xr_(program_vm|typed_)|"
    r"src/execution/xr_|tests/unit/program/|tests/unit/vm/.*xr_program|"
    r"tests/unit/aot/.*xr_program|tests/unit/ir/test_xr_program|"
    r"scripts/(canonical_program_test_profile|program_source_fixtures|"
    r"run_canonical_program_gate|"
    r"check_xr_program)[^/]*\.py$|"
    r"tests/lib/(program_source_fixtures\.py|tests/test_(canonical_program|"
    r"xr_program)[^/]*\.py)$)")
DOCUMENTATION_PATH = re.compile(r"(^|/)(README[^/]*|[^/]+\.md)$")


def usage(code: int = 0) -> int:
    doc = __doc__ or ""
    start = doc.find("USAGE")
    print(doc[start:].split("\n\nEnvironment:")[0] if start >= 0 else doc)
    return code


def default_jobs() -> int:
    cores = platform.cpu_count()
    return max(1, cores - 2) if cores > 3 else 1


def default_ctest_jobs(build_jobs: int) -> int:
    return min(build_jobs, 8) if platform.IS_WINDOWS else build_jobs


def git_lines(args: Sequence[str]) -> List[str]:
    result = proc.run(["git", *args], cwd=REPO_ROOT)
    if not result.ok:
        return []
    return [line for line in result.stdout.decode("utf-8", "replace").splitlines()
            if line.strip()]


def choose_run_for_paths(changed: Sequence[str]) -> Tuple[str, str]:
    """Return a named exact profile or a conservative broad tier.

    Exact profiles are permitted only when every non-documentation path has a
    declared owner. Mixed or unknown paths escalate; they never inherit the
    narrowest rule that happened to match one file.
    """
    if not changed:
        return "t0", "working tree is clean"

    substantive = [path.replace("\\", "/") for path in changed
                   if not DOCUMENTATION_PATH.search(path.replace("\\", "/"))]
    if not substantive:
        return "t0", "documentation-only change"

    if all(INFRA_EDIT_PATH.search(path) for path in substantive):
        return "infra", "test-runner infrastructure is fully owned by the infra profile"

    if all(CANONICAL_EDIT_PATH.search(path) for path in substantive):
        return "canonical", "canonical Program private paths have an exact profile"

    if any(BACKEND_TOUCHED.search(path) for path in substantive):
        return "t2", "backend, runtime, ISA or build-graph change"
    if any(path.startswith("src/") for path in substantive):
        return "t1", "compiler source outside an exact owned profile"
    return "t1", "unowned or mixed change; conservatively escalated"


def choose_run_from_diff() -> str:
    """Choose a bounded profile/tier and explain the complete path decision."""
    changed = sorted(set(git_lines(["diff", "--name-only", "HEAD"])
                         + git_lines(["ls-files", "--others", "--exclude-standard"])))
    run, reason = choose_run_for_paths(changed)
    print(f"auto rule: {reason}; running {run}")

    for path in changed[:12]:
        print(f"       {path}")
    if len(changed) > 12:
        print("       ...")
    return run


def print_exact_inventory(label: str, items: Sequence[str]) -> None:
    """Print an exact machine selection without one unbounded output line."""
    print(f"{BLUE}==>{NC} exact {label} ({len(items)})")
    line = "    "
    for item in items:
        addition = item if line == "    " else f", {item}"
        if len(line) + len(addition) > 108:
            print(line)
            line = f"    {item}"
        else:
            line += addition
    if line != "    ":
        print(line)


def validate_exact_inventory(label: str, selected: Sequence[str],
                             expected: Sequence[str]) -> bool:
    selected_set = set(selected)
    expected_set = set(expected)
    missing = sorted(expected_set - selected_set)
    unexpected = sorted(selected_set - expected_set)
    if not missing and not unexpected and len(selected) == len(expected):
        return True
    print(f"{RED}{label} inventory mismatch{NC}")
    for name in missing:
        print(f"    missing: {name}")
    for name in unexpected:
        print(f"    unexpected: {name}")
    if len(selected) != len(set(selected)):
        print("    duplicate selected CTest name")
    return False


def ctest_names(build_dir: Path, args: Sequence[str]) -> List[str]:
    result = proc.run(["ctest", "-N", *args], cwd=build_dir)
    names = []
    for line in result.stdout.decode("utf-8", "replace").splitlines():
        match = TEST_NAME_RE.match(line)
        if match:
            names.append(match.group(1))
    return names


@contextmanager
def timed_phase(name: str):
    """Report wall time even on failure without changing the phase outcome."""
    started = time.perf_counter()
    try:
        yield
    finally:
        print(f"{BLUE}==>{NC} timing: {name}={time.perf_counter() - started:.3f}s")


def has_explicit_ctest_selection(args: Sequence[str]) -> bool:
    """Return whether forwarded ctest arguments intentionally narrow the run."""
    for argument in args:
        if argument in FOCUSED_CTEST_OPTIONS:
            return True
        if ((argument.startswith("-R") or argument.startswith("-L")) and
                len(argument) > 2):
            return True
        if any(argument.startswith(f"{option}=")
               for option in FOCUSED_CTEST_OPTIONS if option.startswith("--")):
            return True
    return False


def tier_ctest_filters(tier: str, focused_selection: bool) -> Tuple[str, str, str]:
    """Return tier filters while avoiding duplicate exhaustive Xi execution."""
    include, exclude, not_covered = TIERS[tier]
    if tier == "t3" and not focused_selection:
        exclude = exact_ctest_regex((XI_GENERATOR_FAST,))
    return include, exclude, not_covered


def has_ctest_option(args: Sequence[str], option: str) -> bool:
    """Return whether forwarded arguments already claim an owned CTest option."""
    return option in args or any(argument.startswith(f"{option}=") for argument in args)


def run_ctest(build_dir: Path, args: Sequence[str], env: dict[str, str],
              expected: Sequence[str] = ()) -> int:
    """Run CTest and prove an exact profile executed its complete inventory."""
    command = ["ctest", *args, "--no-tests=error"]
    if not expected:
        return subprocess.call(command, cwd=str(build_dir), env=env)

    with workspace.Workspace("xray_t_ctest_evidence") as evidence:
        report = evidence.path("ctest.xml")
        code = subprocess.call([*command, "--output-junit", str(report)],
                               cwd=str(build_dir), env=env)
        try:
            executed = canonical_profile.executed_ctest_names(report)
        except canonical_profile.source_fixtures.FixtureError as error:
            print(f"{RED}CTest execution inventory invalid{NC}: {error}")
            return code or 1
        if not validate_exact_inventory("executed CTest", executed, expected):
            return code or 1
        print_exact_inventory("executed CTest names", executed)
        return code


def cache_contains_all(build_dir: Path, entries: Sequence[str]) -> bool:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return False
    text = cache.read_text(encoding="utf-8", errors="replace")
    return all(entry in text for entry in entries)


def production_build_identity_errors(build_dir: Path) -> Tuple[str, ...]:
    """Return every reason a tree cannot provide production t2/t3 evidence."""
    cache = build_dir / "CMakeCache.txt"
    try:
        lines = cache.read_text(encoding="utf-8", errors="strict").splitlines()
    except (OSError, UnicodeError) as error:
        return (f"cannot read {cache}: {error}",)

    required = {"CMAKE_HOME_DIRECTORY", *PRODUCTION_CACHE_VALUES}
    values: Dict[str, str] = {}
    errors: List[str] = []
    for line in lines:
        key, separator, value = line.partition("=")
        if not separator or ":" not in key:
            continue
        name = key.split(":", 1)[0]
        if name not in required:
            continue
        if name in values:
            errors.append(f"duplicate CMake cache entry: {name}")
            continue
        values[name] = value.strip()

    for name in sorted(required - set(values)):
        errors.append(f"missing CMake cache entry: {name}")

    home = values.get("CMAKE_HOME_DIRECTORY")
    if home:
        home_path = Path(home)
        if not home_path.is_absolute():
            errors.append(f"CMAKE_HOME_DIRECTORY is not absolute: {home!r}")
        else:
            actual_root = os.path.normcase(os.path.normpath(
                str(home_path.resolve(strict=False))))
            expected_root = os.path.normcase(os.path.normpath(
                str(REPO_ROOT.resolve(strict=False))))
        if home_path.is_absolute() and actual_root != expected_root:
            errors.append(
                f"CMAKE_HOME_DIRECTORY is {home!r}, expected {str(REPO_ROOT)!r}"
            )

    for name, expected in PRODUCTION_CACHE_VALUES.items():
        actual = values.get(name)
        if actual is not None and actual != expected:
            errors.append(f"{name} is {actual!r}, expected {expected!r}")
    return tuple(errors)


def validate_production_build(build_dir: Path) -> bool:
    """Fail closed unless a t2/t3 tree is the exact production configuration."""
    errors = production_build_identity_errors(build_dir)
    if not errors:
        print(f"{BLUE}==>{NC} production build identity verified")
        return True
    print(f"{RED}PRODUCTION BUILD PREFLIGHT FAILED{NC}: {build_dir}")
    for error in errors:
        print(f"    {error}")
    print("    configure a separate Ninja Release tree with embedded stdlib, "
          "VM fastpaths enabled, and sanitizers disabled")
    return False


@timed_phase("manifest/configure")
def refresh_cmake_manifest(build_dir: Path, jobs: int) -> bool:
    """Refresh Ninja's CMake/CTest inventory before selecting build targets.

    `ninja -t targets` and `ctest -N` do not trigger Ninja's automatic CMake
    regeneration.  Without this preflight a newly registered test appears only
    during the subsequent build, after target selection, and CTest then reports
    a missing executable.  Building the `build.ninja` manifest is a no-op on an
    up-to-date tree and performs only the required reconfigure on a stale one.
    """
    if not (build_dir / "build.ninja").is_file():
        return True
    result = proc.run(["cmake", "--build", str(build_dir), "-j", str(jobs),
                       "--target", "build.ninja"])
    if result.ok:
        return True
    print(f"{RED}CMAKE MANIFEST REFRESH FAILED{NC}")
    sys.stdout.write(result.combined_text())
    return False


@timed_phase("build (dependency checks + compile + link)")
def build_selected(build_dir: Path, selected: Sequence[str], jobs: int,
                   include_xray: bool = True,
                   required_targets: Sequence[str] = (),
                   allow_no_targets: bool = False,
                   explain_targets: bool = False) -> bool:
    """Build exactly what this run needs.

    Every tier needs current binaries; running a tier against a stale one is
    worse than not running it at all. Building everything fixes that but links
    ~195 executables on every iteration, so resolve the selected test names to
    build targets and build only those. Broad tiers also build `xray`, which
    their script-driven tests invoke. A named profile may opt out when its exact
    inventory contains no product-CLI test.
    """
    targets: List[str] = ["xray"] if include_xray else []
    known = proc.run(["ninja", "-C", str(build_dir), "-t", "targets", "all"])
    if known.ok:
        available = {match.group(1)
                     for match in (NINJA_TARGET_RE.match(line) for line in
                                   known.stdout.decode("utf-8", "replace").splitlines())
                     if match}
        missing_targets = sorted(set(required_targets) - available)
        if missing_targets:
            print(f"{RED}BUILD PROFILE INVALID{NC}: required Ninja target(s) missing")
            for target in missing_targets:
                print(f"    {target}")
            return False
        targets.extend(required_targets)
        targets.extend(sorted(set(selected) & available))
        targets = list(dict.fromkeys(targets))
        if not targets and allow_no_targets:
            print(f"{BLUE}==>{NC} building 0 targets (script-only exact profile)")
            return True
    else:
        # No target list available: fall back to a full build rather than
        # silently under-building and testing stale binaries.
        targets = []

    if targets:
        print(f"{BLUE}==>{NC} building {len(targets)} target(s)")
        if explain_targets:
            print_exact_inventory("build targets", targets)
        argv = ["cmake", "--build", str(build_dir), "-j", str(jobs)]
        for target in targets:
            argv += ["--target", target]
    else:
        print(f"{BLUE}==>{NC} building (all targets)")
        argv = ["cmake", "--build", str(build_dir), "-j", str(jobs)]

    log = build_dir / ".t-build.log"
    result = proc.run(argv)
    log.write_text(result.combined_text(), encoding="utf-8", newline="\n")
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


@timed_phase("regression corpus")
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


def _run_main(argv: List[str]) -> int:
    if len(argv) < 2:
        return usage(1)
    tier = argv[1]
    extra = argv[2:]
    if tier in ("-h", "--help", "help"):
        return usage(0)

    if tier == "auto":
        tier = choose_run_from_diff()
    exact_profile = EXACT_PROFILES.get(tier)
    if tier not in TIERS and exact_profile is None:
        print(f"Unknown tier '{tier}'")
        return usage(1)

    started = time.perf_counter()

    def toolchain_log(message: str, *, error: bool = False) -> None:
        stream = sys.stderr if error else sys.stdout
        color = RED if error else BLUE
        print(f"{color}==>{NC} {message}", file=stream)

    with timed_phase("toolchain setup"):
        if not sanitizer.activate_windows_msvc_environment(toolchain_log):
            return 1

    build_dir = Path(os.environ.get("XR_BUILD_DIR", "build"))
    build_jobs = platform.env_int("XR_JOBS", default_jobs())
    ctest_jobs = platform.env_int("XR_CTEST_JOBS", default_ctest_jobs(build_jobs))

    # XR_FAST removes both self-hosted stdlib generation edges from the edit
    # cycle.  Source loading preserves the language/compiler checks while
    # avoiding an embedded-bytecode rebuild that cannot succeed during parts of
    # the bootstrap.  Fastpaths are a VM performance layer, not a semantic one.
    # canonical/t2/t3 explicitly gate production configuration edges and reject
    # the flag. Narrow exact profiles identify themselves as partial preflights.
    if platform.env_flag("XR_FAST"):
        if tier in ("canonical", "t2", "t3"):
            print(f"{RED}Error{NC}: XR_FAST=1 is not accepted by {tier}.")
            print(f"       {tier} gates production stdlib bootstrap edges; a tree built")
            print("       without them would report a pass that tier never established.")
            return 1
        fast_default = "build-fast-clang" if platform.IS_WINDOWS else "build-fast"
        build_dir = Path(os.environ.get("XR_BUILD_DIR", fast_default))
        fast_cache = (
            "XR_STDLIB_FROM_FILE:BOOL=ON",
            "XRAY_STDLIB_VM_FASTPATHS:BOOL=OFF",
        )
        if not cache_contains_all(build_dir, fast_cache):
            print(f"{BLUE}==>{NC} configuring {build_dir} (source stdlib, no VM fastpaths)")
            configure_args = [
                "cmake", "-S", str(REPO_ROOT), "-B", str(build_dir), "-G", "Ninja",
                "-DCMAKE_BUILD_TYPE=Release", "-DXR_STDLIB_FROM_FILE=ON",
                "-DXRAY_STDLIB_VM_FASTPATHS=OFF", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            ]
            if platform.IS_WINDOWS:
                configure_args.extend((
                    f"-DCMAKE_C_COMPILER={sanitizer.resolve_compiler_command('clang-cl')}",
                    f"-DCMAKE_CXX_COMPILER={sanitizer.resolve_compiler_command('clang-cl')}",
                ))
            with timed_phase("fast-tree configure"):
                configure = proc.run(configure_args)
            if not configure.ok:
                sys.stdout.write(configure.combined_text())
                return 1
        print(f"{YELLOW}XR_FAST{NC}: using {build_dir} with source stdlib and no VM fastpaths")

    shards = platform.env_int("XR_SHARDS", 1)
    shard_index = platform.env_int("XR_SHARD_INDEX", 0)
    if shards < 1:
        print(f"{RED}Error{NC}: XR_SHARDS must be >= 1")
        return 1
    if not 0 <= shard_index < shards:
        print(f"{RED}Error{NC}: XR_SHARD_INDEX must be in [0,{shards})")
        return 1
    if shards > 1 and exact_profile is not None:
        print(f"{RED}Error{NC}: XR_SHARDS is not accepted by exact profile "
              f"{tier}.")
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

    if tier in ("t2", "t3") and not validate_production_build(build_dir):
        return 1

    kind = "profile" if exact_profile is not None else "tier"
    print(f"{BOLD}{kind} {tier}{NC}  build={build_dir}  "
          f"build_jobs={build_jobs}  ctest_jobs={ctest_jobs}")
    print("=" * 72)

    if not refresh_cmake_manifest(build_dir, build_jobs):
        return 1

    focused_selection = exact_profile is None and has_explicit_ctest_selection(extra)
    if exact_profile is not None:
        include = exact_ctest_regex(exact_profile.tests)
        exclude = ""
        not_covered = exact_profile.not_covered
    else:
        include, exclude, not_covered = tier_ctest_filters(tier, focused_selection)
    if focused_selection:
        focus_gap = f"unselected {tier} tests and auxiliary corpora"
        not_covered = f"{focus_gap}, {not_covered}" if not_covered else focus_gap
    ctest_args = ["--output-on-failure", "-j", str(ctest_jobs)]
    if include:
        ctest_args += ["-R", include]
    if exclude:
        ctest_args += ["-E", exclude]

    selected = ctest_names(build_dir, ctest_args + extra)
    total = len(ctest_names(build_dir, []))
    print(f"{BLUE}==>{NC} ctest: {len(selected)}/{total} tests")
    if not selected:
        print(f"{RED}TEST SELECTION FAILED{NC}: no tests match the selected tier and filters")
        return 1

    exact_expected: Sequence[str] = ()
    required_targets: Sequence[str] = ()
    include_xray = True
    allow_no_targets = False
    if exact_profile is not None:
        exact_expected = exact_profile.tests
        required_targets = exact_profile.targets
        include_xray = exact_profile.include_xray
        allow_no_targets = exact_profile.allow_no_build_targets
    elif tier == "t0" and not focused_selection:
        exact_expected = T0_CTEST_NAMES
        required_targets = canonical_profile.BUILD_TARGETS

    if exact_expected:
        if not validate_exact_inventory(f"{tier} exact profile", selected,
                                        exact_expected):
            return 1
        if has_ctest_option(extra, "--output-junit"):
            print(f"{RED}Error{NC}: exact profiles own --output-junit evidence output")
            return 1
        print_exact_inventory("CTest names", selected)

    if not platform.env_flag("XR_NO_BUILD"):
        if not build_selected(
            build_dir,
            selected,
            build_jobs,
            include_xray=include_xray,
            required_targets=required_targets,
            allow_no_targets=allow_no_targets,
            explain_targets=bool(exact_expected),
        ):
            return 1
        refreshed = ctest_names(build_dir, ctest_args + extra)
        if not refreshed:
            print(f"{RED}TEST SELECTION FAILED{NC}: no tests remain after the build refresh")
            return 1
        if refreshed != selected:
            print(f"{YELLOW}==>{NC} CTest inventory changed during build; rebuilding exact "
                  "selection")
            selected = refreshed
            total = len(ctest_names(build_dir, []))
            print(f"{BLUE}==>{NC} ctest: {len(selected)}/{total} tests (refreshed)")
            if not build_selected(
                build_dir,
                selected,
                build_jobs,
                include_xray=include_xray,
                required_targets=required_targets,
                allow_no_targets=allow_no_targets,
                explain_targets=bool(exact_expected),
            ):
                return 1
        if exact_expected and not validate_exact_inventory(
                f"{tier} exact profile", selected, exact_expected):
            return 1

    env = dict(os.environ)
    env.setdefault("XRAY_TEST_JOBS", str(ctest_jobs))
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

    with timed_phase("ctest"):
        code = run_ctest(build_dir, [*ctest_args, *extra], env, exact_expected)

    if not focused_selection:
        if tier == "t1" and not run_regression_corpus(build_dir, True):
            code = 1
        elif tier in ("t2", "t3") and not run_regression_corpus(build_dir, False):
            code = 1

    # t0 runs an exact negative smoke inventory. t1 already includes the full
    # compile_error_tests CTest, so repeating all cases here made t0 slower than
    # the tier above it and let one divergent case consume the entire edit loop.
    if tier == "t0" and code == 0 and not focused_selection:
        print(f"{BLUE}==>{NC} compile-error smoke")
        env["XRAY_BIN"] = str(build_dir / platform.exe_name("xray"))
        with timed_phase("compile-error smoke"):
            corpus = proc.run([sys.executable, REPO_ROOT / "tests" / "compile_errors"
                               / "run_compile_error_tests.py", "--case-list",
                               REPO_ROOT / "tests" / "compile_errors"
                               / "t0_cases.txt"], env=env, cwd=REPO_ROOT)
        for line in corpus.combined_text().splitlines()[-6:]:
            print(line)
        if not corpus.ok:
            code = 1

    elapsed = int(time.perf_counter() - started)
    print("=" * 72)
    verdict = f"{GREEN}PASS{NC}" if code == 0 else f"{RED}FAIL{NC}"
    print(f"{kind} {BOLD}{tier}{NC}  {verdict}  {elapsed // 60}m{elapsed % 60:02d}s")

    # A tier that stayed quiet about its own limits is how a green check turns
    # into false confidence. Say what was not covered, every time.
    if not_covered:
        print(f"{YELLOW}not covered by {tier}{NC}: {not_covered}")
        nxt = {"t0": "t1", "t1": "t2", "t2": "t3"}.get(tier)
        if nxt:
            print(f"  next: scripts/t.py {nxt}")

    return code


def requested_build_dir() -> Path:
    """Resolve the exact tree before configure so every writer shares a lock."""
    if platform.env_flag("XR_FAST"):
        default = "build-fast-clang" if platform.IS_WINDOWS else "build-fast"
    else:
        default = "build"
    return Path(os.environ.get("XR_BUILD_DIR", default)).resolve()


def main(argv: List[str]) -> int:
    if len(argv) >= 2 and argv[1] in ("-h", "--help", "help"):
        return _run_main(argv)
    try:
        lease = buildlock.BuildTreeLock(requested_build_dir())
    except ValueError as error:
        print(f"{RED}Error{NC}: {error}")
        return 1

    print(f"{BLUE}==>{NC} build-tree lease: {lease.build_dir}")
    try:
        with timed_phase("build-tree wait"):
            if not lease.acquire():
                owner = lease.owner_text()
                print(f"{RED}BUILD TREE BUSY{NC}: {lease.build_dir}")
                if owner:
                    print(f"    owner: {owner}")
                return 1
        return _run_main(argv)
    finally:
        lease.release()


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
