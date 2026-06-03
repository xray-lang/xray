#!/usr/bin/env python3
"""
JIT Test Suite Runner (Cross-platform)

Runs tests/jit/*.xr files using the // EXPECTED: output matching convention.
Optionally uses --diff mode: compare --no-jit vs --jit-force output (like
run_jit_diff_tests.py but scoped to tests/jit only).

Usage: python3 scripts/run_jit_tests.py [options]
  -b, --binary PATH    Path to xray binary (default: auto-detect)
  -f, --filter STR     Run only tests matching filter
  -v, --verbose        Show output on failure
  -t, --timeout SEC    Timeout per test (default: 10)
  -d, --test-dir DIR   Test directory (default: tests/jit)
  --diff               Use differential mode (--no-jit vs --jit-force)
  -a, --allowlist PATH Known-failures file
  -S, --skip-allowlist Ignore the allowlist
"""

import argparse
import os
import re
import subprocess
import sys
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


# ── Helpers ───────────────────────────────────────────────────────────────────

def detect_binary(project_root: Path) -> Path:
    env_dir = os.environ.get("XRAY_BUILD_DIR")
    if env_dir:
        candidate = Path(env_dir) / BINARY_NAME
        if candidate.exists():
            return candidate
    for d in ["build-release", "build"]:
        candidate = project_root / d / BINARY_NAME
        if candidate.exists():
            return candidate
    return project_root / "build" / BINARY_NAME


def load_allowlist(path: Path) -> set:
    if not path.exists():
        return set()
    entries = set()
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        cleaned = re.sub(r"#.*", "", line).strip()
        if cleaned:
            entries.add(cleaned)
    return entries


def is_crash(rc: int) -> bool:
    if IS_WINDOWS:
        return rc < -1 and rc != -999
    return rc in (134, 139, 136)


def run_xray(binary: Path, args: list, timeout_sec: int) -> tuple:
    cmd = [str(binary)] + args
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True,
            timeout=timeout_sec, errors="replace"
        )
        return (result.returncode, result.stdout, result.stderr)
    except subprocess.TimeoutExpired:
        return (124, "", "")
    except Exception as e:
        return (-999, "", str(e))


def get_expected(test_file: Path) -> tuple:
    """Parse // EXPECTED: lines from test file. Returns (skip: bool, expected_text: str)."""
    lines = []
    try:
        content = test_file.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return (False, "")

    for line in content.splitlines():
        m = re.match(r"^// EXPECTED: (.*)$", line)
        if m:
            lines.append(m.group(1))
            continue
        if re.match(r"^// SKIP", line):
            return (True, "")
        if not line.startswith("//"):
            break

    return (False, "\n".join(lines) if lines else "")


def normalize_jit_output(text: str) -> str:
    """Strip JIT dump sections (===== ... =====) from output."""
    result = []
    in_dump = False
    for line in text.splitlines():
        if re.match(r"^===== end .* =====$", line):
            in_dump = False
            continue
        if re.match(r"^===== .* =====$", line):
            in_dump = True
            continue
        if not in_dump:
            result.append(line)
    return "\n".join(result).rstrip()


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    project_root = Path(__file__).resolve().parent.parent

    parser = argparse.ArgumentParser(description="JIT Test Suite Runner")
    parser.add_argument("-b", "--binary", default="")
    parser.add_argument("-f", "--filter", default="")
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("-t", "--timeout", type=int, default=10)
    parser.add_argument("-d", "--test-dir", default="")
    parser.add_argument("--diff", action="store_true", help="Differential mode: --no-jit vs --jit-force")
    parser.add_argument("-a", "--allowlist", default="")
    parser.add_argument("-S", "--skip-allowlist", action="store_true")
    args = parser.parse_args()

    binary = Path(args.binary) if args.binary else detect_binary(project_root)
    test_dir = Path(args.test_dir) if args.test_dir else project_root / "tests" / "jit"
    allowlist_path = Path(args.allowlist) if args.allowlist else project_root / "tests" / "jit" / "known_failures.txt"

    if not binary.exists():
        print(red(f"Error: xray binary not found at {binary}"))
        print("Build with: cmake --build build-release --target xray")
        sys.exit(1)

    if not test_dir.exists():
        print(red(f"Error: test directory not found at {test_dir}"))
        sys.exit(1)

    known_failures = set()
    if not args.skip_allowlist:
        known_failures = load_allowlist(allowlist_path)

    # Collect test files (non-recursive for tests/jit, just *.xr in top level)
    test_files = sorted(test_dir.glob("*.xr"))
    if args.filter:
        test_files = [f for f in test_files if args.filter in f.stem]

    total = 0
    pass_count = 0
    fail_count = 0
    skip_count = 0
    timeout_count = 0
    crash_count = 0
    known_count = 0
    failures = []

    for test_file in test_files:
        total += 1
        name = test_file.stem
        rel_path = str(test_file.relative_to(project_root)).replace("\\", "/")
        is_known = rel_path in known_failures

        if args.diff:
            # Differential mode
            rc_interp, out_interp, _ = run_xray(binary, ["run", "--no-jit", str(test_file)], args.timeout)
            rc_jit, out_jit, _ = run_xray(binary, ["run", "--jit-force", str(test_file)], args.timeout)

            if rc_interp == 124 or rc_jit == 124:
                timeout_count += 1
                label = "TIMEOUT (known)" if is_known else "TIMEOUT"
                if is_known:
                    known_count += 1
                else:
                    fail_count += 1
                    failures.append(f"TIMEOUT: {rel_path}")
                print(f"  [{total:3d}] {name:<45s} ... {yellow(label)}")
                continue

            if is_crash(rc_jit):
                crash_count += 1
                if is_known:
                    known_count += 1
                    print(f"  [{total:3d}] {name:<45s} ... {yellow('CRASH (known)')}")
                else:
                    fail_count += 1
                    if IS_WINDOWS:
                        lbl = f"CRASH (jit=0x{rc_jit & 0xFFFFFFFF:X})"
                    else:
                        lbl = f"CRASH (exit={rc_jit})"
                    print(f"  [{total:3d}] {name:<45s} ... {red(lbl)}")
                    failures.append(f"CRASH({rc_jit}): {rel_path}")
                continue

            if rc_interp != rc_jit:
                if is_known:
                    known_count += 1
                    print(f"  [{total:3d}] {name:<45s} ... {yellow('EXIT_DIFF (known)')}")
                else:
                    fail_count += 1
                    print(f"  [{total:3d}] {name:<45s} ... {red(f'EXIT_DIFF (interp={rc_interp}, jit={rc_jit})')}")
                    failures.append(f"EXIT_DIFF(interp={rc_interp},jit={rc_jit}): {rel_path}")
                continue

            if rc_interp != 0:
                skip_count += 1
                print(f"  [{total:3d}] {name:<45s} ... {yellow(f'BOTH_FAIL (exit={rc_interp})')}")
                continue

            interp_norm = normalize_jit_output(out_interp)
            jit_norm = normalize_jit_output(out_jit)
            if interp_norm == jit_norm:
                pass_count += 1
                print(f"  [{total:3d}] {name:<45s} ... {green('PASS')}")
            else:
                if is_known:
                    known_count += 1
                    print(f"  [{total:3d}] {name:<45s} ... {yellow('OUTPUT_DIFF (known)')}")
                else:
                    fail_count += 1
                    print(f"  [{total:3d}] {name:<45s} ... {red('OUTPUT_DIFF')}")
                    failures.append(f"OUTPUT_DIFF: {rel_path}")
                if args.verbose and not is_known:
                    print("    Interpreter:")
                    for line in interp_norm.splitlines()[:8]:
                        print(f"      {line}")
                    print("    JIT:")
                    for line in jit_norm.splitlines()[:8]:
                        print(f"      {line}")
        else:
            # Expected-output mode
            skip, expected = get_expected(test_file)
            if skip:
                skip_count += 1
                print(f"  [{total:3d}] {name:<45s} ... {yellow('SKIP')}")
                continue
            if not expected:
                skip_count += 1
                print(f"  [{total:3d}] {name:<45s} ... {yellow('SKIP (no EXPECTED)')}")
                continue

            rc, stdout, stderr = run_xray(binary, [str(test_file)], args.timeout)

            if rc == 124:
                timeout_count += 1
                if is_known:
                    known_count += 1
                    print(f"  [{total:3d}] {name:<45s} ... {yellow('TIMEOUT (known)')}")
                else:
                    fail_count += 1
                    print(f"  [{total:3d}] {name:<45s} ... {yellow('TIMEOUT')}")
                    failures.append(f"TIMEOUT: {rel_path}")
                continue

            if is_crash(rc):
                crash_count += 1
                if is_known:
                    known_count += 1
                    print(f"  [{total:3d}] {name:<45s} ... {yellow('CRASH (known)')}")
                else:
                    fail_count += 1
                    if IS_WINDOWS:
                        lbl = f"CRASH (exit=0x{rc & 0xFFFFFFFF:X})"
                    else:
                        lbl = f"CRASH (exit={rc})"
                    print(f"  [{total:3d}] {name:<45s} ... {red(lbl)}")
                    failures.append(f"CRASH({rc}): {rel_path}")
                continue

            if rc != 0:
                if is_known:
                    known_count += 1
                    print(f"  [{total:3d}] {name:<45s} ... {yellow(f'FAIL (known, exit={rc})')}")
                else:
                    fail_count += 1
                    print(f"  [{total:3d}] {name:<45s} ... {red(f'FAIL (exit={rc})')}")
                    failures.append(f"FAIL({rc}): {rel_path}")
                if args.verbose:
                    print(f"    Output: {stdout.rstrip()}")
                continue

            actual = stdout.rstrip("\n").rstrip("\r\n")
            if actual == expected:
                pass_count += 1
                print(f"  [{total:3d}] {name:<45s} ... {green('PASS')}")
            else:
                if is_known:
                    known_count += 1
                    print(f"  [{total:3d}] {name:<45s} ... {yellow('FAIL (known)')}")
                else:
                    fail_count += 1
                    print(f"  [{total:3d}] {name:<45s} ... {red('FAIL')}")
                    failures.append(f"MISMATCH: {rel_path}")
                if args.verbose and not is_known:
                    print(f"    Expected: {expected}")
                    print(f"    Actual:   {actual}")

    # Summary
    print()
    print("======================================")
    print("JIT Test Summary")
    print("======================================")
    print(f"Total:   {total}")
    print(green(f"Pass:    {pass_count}"))
    print(red(f"Fail:    {fail_count}"))
    print(red(f"Crash:   {crash_count}"))
    print(yellow(f"Timeout: {timeout_count}"))
    print(yellow(f"Skip:    {skip_count}"))
    if not args.skip_allowlist:
        print(yellow(f"Known:   {known_count}"))

    if failures:
        print()
        print(red("Unexpected failures:"))
        for f in failures:
            print(red(f"  {f}"))

    print()
    if fail_count == 0:
        if known_count > 0:
            print(green(f"All JIT tests passed ({known_count} known failures in allowlist)."))
        else:
            print(green("All JIT tests passed!"))
        sys.exit(0)
    else:
        sys.exit(1)


if __name__ == "__main__":
    main()
