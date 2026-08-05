#!/usr/bin/env python3
"""Local 0.9 consolidation gate.

Deliberately not the final roadmap completion gate: it verifies the runnable
local baseline after consolidating the active code lanes, while keeping the
broad AOT expectation suites visible as known open boundaries rather than
hiding them.

The excluded suites are reported separately afterwards, and the report says
which way they went -- if they start passing, the exclusion has to go before
release, and this is where that gets noticed.

Usage: run_release_09_gate.py [options]
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path
from typing import List

PROJECT_DIR = Path(__file__).resolve().parent.parent

# aot_filetests left this list on 2026-07-30: it is ratchet-gated against
# tests/aot/filetests_known_failures.txt, so it blocks new failures while its
# tracked ones are worked down.
KNOWN_AOT_BOUNDARY_RE = r"^(aot_link_command_manifest)$"


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Local 0.9 consolidation gate.",
        epilog=f"The gate excludes only:\n  {KNOWN_AOT_BOUNDARY_RE}\n\n"
               "Those suites currently track long-running AOT file/link "
               "expectation work and are reported separately so a local 0.9 "
               "baseline is not blocked on roadmap completion work.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path,
                        default=Path(os.environ.get("XRAY_RELEASE09_BUILD_DIR",
                                                    str(PROJECT_DIR / "build"))),
                        help="CMake build directory (default: build)")
    parser.add_argument("--jobs", default=os.environ.get("XRAY_TEST_JOBS", "8"),
                        help="parallel jobs for build/ctest")
    parser.add_argument("--ctest", default=os.environ.get("CTEST", "ctest"),
                        help="ctest executable")
    parser.add_argument("--extra-exclude-regex",
                        default=os.environ.get("XRAY_RELEASE09_EXTRA_EXCLUDE_RE", ""),
                        help="extra CTest exclusion regex for platform-specific "
                             "CI debt")
    parser.add_argument("--no-build", action="store_true",
                        help="skip cmake --build")
    parser.add_argument("--skip-boundary-report", action="store_true",
                        help="do not run the known open AOT boundary suites")
    return parser.parse_args(argv[1:])


def main(argv: List[str]) -> int:
    args = parse_args(argv)

    if not str(args.jobs).isdigit():
        sys.stderr.write(f"invalid --jobs value: {args.jobs}\n")
        return 2
    if not args.build_dir.is_dir():
        sys.stderr.write(f"build directory not found: {args.build_dir}\n")
        sys.stderr.write("configure first, for example: "
                         f"cmake -S {PROJECT_DIR} -B {args.build_dir}\n")
        return 2

    print("=== Xray 0.9 local consolidation gate ===")
    print(f"Project:   {PROJECT_DIR}")
    print(f"Build dir: {args.build_dir}")
    print(f"Jobs:      {args.jobs}")
    print(f"Excluded known AOT boundary suites: {KNOWN_AOT_BOUNDARY_RE}")
    exclude = KNOWN_AOT_BOUNDARY_RE
    if args.extra_exclude_regex:
        exclude = f"{exclude}|{args.extra_exclude_regex}"
        print(f"Extra platform exclusions: {args.extra_exclude_regex}")
    print("")
    sys.stdout.flush()

    if not args.no_build:
        code = subprocess.call(["cmake", "--build", str(args.build_dir),
                                "-j", str(args.jobs)])
        if code != 0:
            return code
        print("")
        sys.stdout.flush()

    code = subprocess.call([args.ctest, "--test-dir", str(args.build_dir),
                            "--output-on-failure", "-j", str(args.jobs),
                            "-E", exclude])
    if code != 0:
        return code

    if not args.skip_boundary_report:
        print("")
        print("=== Known open AOT boundary report (non-blocking for 0.9) ===")
        sys.stdout.flush()
        boundary = subprocess.call([args.ctest, "--test-dir", str(args.build_dir),
                                    "--output-on-failure", "-R",
                                    KNOWN_AOT_BOUNDARY_RE])
        if boundary == 0:
            print("Known boundary suites now pass; remove the 0.9 exclusion "
                  "before release.")
        else:
            print("Known boundary suites still fail as expected for the current "
                  "roadmap boundary.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
