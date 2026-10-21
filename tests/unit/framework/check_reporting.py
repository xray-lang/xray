#!/usr/bin/env python3
"""Check actual framework output and exit status for intentional fixture failures."""

import subprocess
import sys


def main():
    executable, mode = sys.argv[1:]
    cases = {
        "success": (0, 2, 0),
        "direct_failure": (1, 1, 1),
        "helper_failure": (1, 1, 2),
    }
    expected_exit, passes, assertions = cases[mode]
    result = subprocess.run([executable, mode], capture_output=True,
                            text=True, encoding="utf-8", timeout=10)
    context = f"exit={result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    assert result.returncode == expected_exit, context
    assert result.stderr == "", context
    assert result.stdout.count("\033[32mPASS\033[0m\n") == passes, context
    assert result.stdout.count("\033[31mFAIL\033[0m\n") == assertions, context
    assert "later case executed\n" in result.stdout, context
    counters = f"COUNTERS run=2 passed={passes} assertions_failed={assertions}\n"
    assert counters in result.stdout, context
    if mode == "success":
        assert "All 2 tests passed" in result.stdout, context
        assert "tests failed" not in result.stdout, context
    else:
        assert "1/2 tests failed" in result.stdout, context
        assert "All " not in result.stdout, context
    if mode == "helper_failure":
        assert "helper body continued\n" in result.stdout, context
    print(f"PASS: {mode} preserves case counts, assertion diagnostics, continuation and exit")


if __name__ == "__main__":
    main()
