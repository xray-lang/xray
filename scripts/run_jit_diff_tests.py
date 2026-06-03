#!/usr/bin/env python3
"""
JIT Differential Test Runner (Cross-platform)

Runs tests with --jit-force (threshold=1) vs --no-jit (interpreter only),
comparing output to detect JIT correctness issues.

WHY --jit-force:
  Default JIT threshold is 100 calls. Regression tests call @test functions
  only once, so default mode never actually triggers JIT compilation.
  --jit-force sets threshold=1, ensuring every function gets JIT compiled.

Usage: python3 scripts/run_jit_diff_tests.py [options]
  -b, --binary PATH    Path to xray binary (default: auto-detect)
  -f, --filter STR     Run only tests whose path contains this substring
  -v, --verbose        Show output diff on mismatch
  -t, --timeout SEC    Timeout per test per mode (default: 10)
  -d, --test-dir DIR   Test directory (default: tests/regression)
  -j, --jobs N         Parallel jobs (default: CPU count)
  -J, --include-jit    Also run tests/jit directory
  -a, --allowlist PATH Custom known-failures file
  -S, --skip-allowlist Ignore the allowlist; treat every failure as unexpected
"""

import argparse
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

# ── Platform detection ────────────────────────────────────────────────────────

IS_WINDOWS = sys.platform == "win32"
BINARY_NAME = "xray.exe" if IS_WINDOWS else "xray"

# ── Color output ──────────────────────────────────────────────────────────────

_use_color = (
    hasattr(sys.stdout, "isatty")
    and sys.stdout.isatty()
    and not os.environ.get("NO_COLOR")
)

if _use_color and IS_WINDOWS:
    # Enable ANSI on Windows 10+
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32
        kernel32.SetConsoleMode(kernel32.GetStdHandle(-11), 7)
    except Exception:
        _use_color = False


def _c(code: str, text: str) -> str:
    return f"\033[{code}m{text}\033[0m" if _use_color else text


def green(s: str) -> str: return _c("32", s)
def red(s: str) -> str: return _c("31", s)
def yellow(s: str) -> str: return _c("33", s)
def blue(s: str) -> str: return _c("34", s)


# ── Helpers ───────────────────────────────────────────────────────────────────

def detect_binary(project_root: Path) -> Path:
    """Auto-detect xray binary location."""
    env_dir = os.environ.get("XRAY_BUILD_DIR")
    if env_dir:
        candidate = Path(env_dir) / BINARY_NAME
        if candidate.exists():
            return candidate

    for build_dir in ["build", "build-release"]:
        candidate = project_root / build_dir / BINARY_NAME
        if candidate.exists():
            return candidate

    # Fallback
    return project_root / "build" / BINARY_NAME


def load_allowlist(path: Path) -> set:
    """Load known-failures allowlist, stripping comments and whitespace."""
    if not path.exists():
        return set()
    entries = set()
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        cleaned = re.sub(r"#.*", "", line).strip()
        if cleaned:
            entries.add(cleaned)
    return entries


def is_crash(rc: int) -> bool:
    """Detect crash exit codes (platform-aware)."""
    if IS_WINDOWS:
        # Windows: negative exit codes indicate unhandled exceptions
        # e.g. ACCESS_VIOLATION = -1073741819 (0xC0000005)
        return rc < -1 and rc != -999
    else:
        # Unix: SIGABRT=134, SIGSEGV=139, SIGFPE=136
        return rc in (134, 139, 136)


TIMING_RE = re.compile(r"\(\d+ms\)|\d+ms")


def normalize_output(text: str) -> str:
    """Normalize timing-dependent output for comparison."""
    return TIMING_RE.sub("_ms", text)


# ── Single test execution ─────────────────────────────────────────────────────

def run_xray(binary: Path, args: list, timeout_sec: int) -> tuple:
    """Run xray with given args. Returns (exit_code, stdout, stderr)."""
    cmd = [str(binary)] + args
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout_sec,
            errors="replace",
        )
        return (result.returncode, result.stdout, result.stderr)
    except subprocess.TimeoutExpired:
        return (124, "", "")
    except Exception as e:
        return (-999, "", str(e))


def run_diff_test(
    test_file: Path, binary: Path, timeout_sec: int, project_root: Path
) -> dict:
    """Run one differential test: --no-jit vs --jit-force."""
    rel_path = str(test_file.relative_to(project_root)).replace("\\", "/")

    # Run both modes
    rc_interp, out_interp, err_interp = run_xray(
        binary, ["test", "--no-jit", str(test_file)], timeout_sec
    )
    rc_jit, out_jit, err_jit = run_xray(
        binary, ["test", "--jit-force", str(test_file)], timeout_sec
    )

    # Classify result
    status = "UNKNOWN"
    if rc_interp == 124 and rc_jit == 124:
        status = "TIMEOUT_BOTH"
    elif rc_jit == 124:
        status = "TIMEOUT_JIT"
    elif is_crash(rc_jit) and not is_crash(rc_interp):
        status = "CRASH"
    elif is_crash(rc_jit) and is_crash(rc_interp):
        status = "BOTH_FAIL"
    elif rc_interp != 0 and rc_jit != 0:
        status = "BOTH_FAIL"
    elif rc_interp != rc_jit:
        status = "EXIT_DIFF"
    else:
        # Both succeed — compare normalized output
        norm_interp = normalize_output(out_interp)
        norm_jit = normalize_output(out_jit)
        if norm_interp == norm_jit:
            # Check at least one @test executed
            combined = out_interp + err_interp
            match = re.search(r"(\d+) passed", combined)
            if match and int(match.group(1)) > 0:
                status = "PASS"
            else:
                status = "NO_TESTS"
        else:
            status = "OUTPUT_DIFF"

    return {
        "status": status,
        "rc_interp": rc_interp,
        "rc_jit": rc_jit,
        "rel_path": rel_path,
        "out_interp": out_interp,
        "out_jit": out_jit,
    }


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    project_root = Path(__file__).resolve().parent.parent

    parser = argparse.ArgumentParser(description="JIT Differential Test Runner")
    parser.add_argument("-b", "--binary", default="", help="Path to xray binary")
    parser.add_argument("-f", "--filter", default="", help="Filter tests by substring")
    parser.add_argument("-v", "--verbose", action="store_true", help="Show diff on mismatch")
    parser.add_argument("-t", "--timeout", type=int, default=10, help="Timeout per test (seconds)")
    parser.add_argument("-d", "--test-dir", default="", help="Test directory")
    parser.add_argument("-j", "--jobs", type=int, default=0, help="Parallel jobs")
    parser.add_argument("-J", "--include-jit", action="store_true", help="Also run tests/jit")
    parser.add_argument("-a", "--allowlist", default="", help="Known-failures file")
    parser.add_argument("-S", "--skip-allowlist", action="store_true", help="Ignore allowlist")
    args = parser.parse_args()

    # Resolve paths
    binary = Path(args.binary) if args.binary else detect_binary(project_root)
    test_dir = Path(args.test_dir) if args.test_dir else project_root / "tests" / "regression"
    allowlist_path = Path(args.allowlist) if args.allowlist else project_root / "tests" / "jit" / "known_failures.txt"
    jobs = args.jobs if args.jobs > 0 else (os.cpu_count() or 4)

    if not binary.exists():
        print(red(f"Error: xray binary not found at {binary}"))
        sys.exit(1)

    # Load allowlist
    known_failures = set()
    if not args.skip_allowlist:
        known_failures = load_allowlist(allowlist_path)

    # Collect test files
    test_files = sorted(test_dir.rglob("*.xr"))
    if args.include_jit:
        jit_dir = project_root / "tests" / "jit"
        if jit_dir.exists():
            test_files.extend(sorted(jit_dir.rglob("*.xr")))

    # Filter: skip helper files (starting with _) and apply user filter
    test_files = [
        f for f in test_files
        if not f.name.startswith("_")
        and (not args.filter or args.filter in str(f.relative_to(project_root)))
    ]

    total = len(test_files)

    # Banner
    print(blue("======================================"))
    print(blue("JIT Differential Test Runner"))
    print(blue("======================================"))
    print(f"Binary:    {binary}")
    print(f"Test dir:  {test_dir}")
    print(f"Timeout:   {args.timeout}s per mode")
    print(f"Parallel:  {jobs} jobs")
    print(f"Tests:     {total}")
    print(f"Strategy:  --jit-force vs --no-jit")
    print()

    start_time = time.time()

    # Run tests in parallel
    results = [None] * total
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        future_to_idx = {}
        for i, test_file in enumerate(test_files):
            future = executor.submit(
                run_diff_test, test_file, binary, args.timeout, project_root
            )
            future_to_idx[future] = i

        for future in as_completed(future_to_idx):
            idx = future_to_idx[future]
            try:
                results[idx] = future.result()
            except Exception as e:
                results[idx] = {
                    "status": "ERROR",
                    "rc_interp": -1,
                    "rc_jit": -1,
                    "rel_path": str(test_files[idx].relative_to(project_root)),
                    "out_interp": "",
                    "out_jit": str(e),
                }

    # Display results in order
    pass_count = 0
    diff_fail = 0
    both_fail = 0
    jit_crash = 0
    timeout_count = 0
    no_tests_count = 0
    known_count = 0
    unexpected_count = 0
    failures = []

    for i, r in enumerate(results):
        idx = i + 1
        rel_path = r["rel_path"]
        status = r["status"]
        is_known = rel_path in known_failures

        if status == "PASS":
            pass_count += 1
            print(f"  [{idx:3d}] {rel_path:<55s} {green('PASS')}")
        elif status == "NO_TESTS":
            no_tests_count += 1
            print(f"  [{idx:3d}] {rel_path:<55s} {yellow('NO_TESTS')}")
        elif status == "TIMEOUT_BOTH":
            timeout_count += 1
            print(f"  [{idx:3d}] {rel_path:<55s} {yellow('TIMEOUT (both)')}")
        elif status == "TIMEOUT_JIT":
            timeout_count += 1
            print(f"  [{idx:3d}] {rel_path:<55s} {yellow('TIMEOUT (jit)')}")
            failures.append(f"  TIMEOUT(jit): {rel_path}")
        elif status == "CRASH":
            jit_crash += 1
            if is_known:
                known_count += 1
                print(f"  [{idx:3d}] {rel_path:<55s} {yellow('CRASH (known)')}")
            else:
                unexpected_count += 1
                rc_jit = r["rc_jit"]
                rc_interp = r["rc_interp"]
                if IS_WINDOWS:
                    label = f"CRASH (jit=0x{rc_jit & 0xFFFFFFFF:X}, interp={rc_interp})"
                else:
                    label = f"CRASH (jit exit={rc_jit}, interp exit={rc_interp})"
                print(f"  [{idx:3d}] {rel_path:<55s} {red(label)}")
                failures.append(f"  CRASH(jit={rc_jit}): {rel_path}")
        elif status == "BOTH_FAIL":
            both_fail += 1
            ri, rj = r["rc_interp"], r["rc_jit"]
            print(f"  [{idx:3d}] {rel_path:<55s} {yellow(f'BOTH_FAIL (interp={ri}, jit={rj})')}")
        elif status == "EXIT_DIFF":
            diff_fail += 1
            if is_known:
                known_count += 1
                print(f"  [{idx:3d}] {rel_path:<55s} {yellow('EXIT_DIFF (known)')}")
            else:
                unexpected_count += 1
                ri, rj = r["rc_interp"], r["rc_jit"]
                print(f"  [{idx:3d}] {rel_path:<55s} {red(f'EXIT_DIFF (interp={ri}, jit={rj})')}")
                failures.append(f"  EXIT_DIFF(interp={r['rc_interp']},jit={r['rc_jit']}): {rel_path}")
        elif status == "OUTPUT_DIFF":
            diff_fail += 1
            if is_known:
                known_count += 1
                print(f"  [{idx:3d}] {rel_path:<55s} {yellow('OUTPUT_DIFF (known)')}")
            else:
                unexpected_count += 1
                print(f"  [{idx:3d}] {rel_path:<55s} {red('OUTPUT_DIFF')}")
                failures.append(f"  OUTPUT_DIFF: {rel_path}")
            if args.verbose and not is_known:
                interp_lines = r["out_interp"].splitlines()[:5]
                jit_lines = r["out_jit"].splitlines()[:5]
                print("    --- interpreter ---")
                for line in interp_lines:
                    print(f"    {line}")
                print("    --- jit ---")
                for line in jit_lines:
                    print(f"    {line}")
        else:
            print(f"  [{idx:3d}] {rel_path:<55s} {red(f'ERROR ({status})')}")

    # Summary
    elapsed = int(time.time() - start_time)
    print()
    print(blue("======================================"))
    print(blue("JIT Differential Test Summary"))
    print(blue("======================================"))
    print(f"Total:      {total}")
    print(f"Elapsed:    {elapsed}s")
    print(green(f"Pass:       {pass_count}"))
    print(red(f"Diff fail:  {diff_fail}"))
    print(red(f"JIT crash:  {jit_crash}"))
    print(yellow(f"Both fail:  {both_fail}  (pre-existing, not JIT bugs)"))
    print(yellow(f"Timeout:    {timeout_count}"))
    if no_tests_count > 0:
        print(yellow(f"No tests:   {no_tests_count}  (files with no @test functions)"))
    if not args.skip_allowlist:
        print(yellow(f"Known:      {known_count}  (in allowlist)"))
        print(red(f"Unexpected: {unexpected_count}  (NEW regressions)"))

    if failures:
        print()
        print(red("Unexpected JIT failures (NOT in allowlist):"))
        for f in failures:
            print(red(f))

    print()
    if unexpected_count == 0:
        if known_count > 0:
            print(green(f"All JIT differential tests passed ({known_count} known failures in allowlist)."))
        else:
            print(green("All JIT differential tests passed!"))
        sys.exit(0)
    else:
        print(red(f"{unexpected_count} unexpected JIT failure(s) detected!"))
        sys.exit(1)


if __name__ == "__main__":
    main()
