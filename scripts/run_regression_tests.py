#!/usr/bin/env python3
"""Run the tests/regression corpus, plus the cross-backend differential net.

Every case is a `.xr` with @test functions, run through `xray test` under a
per-case timeout. Output is captured through a real file rather than a pipe
buffer: a crashing case writes its report between the last flush and the abort,
which is exactly when the output matters most.

The VM/AOT differential net is folded into the same summary, so a backend
divergence fails the run rather than being reported somewhere else. Opt out
with XRAY_SKIP_BACKEND_DIFF=1 when no AOT host toolchain is available.

`--json PATH` writes the same result as structured data. Callers that need the
failure list should read that instead of scraping the console summary.

Environment:
    XRAY_BUILD_DIR          build directory (default: build, then build-release)
    XRAY_PATH               xray binary, overriding the build directory
    XRAY_TEST_TIMEOUT       per-case seconds (default: 10)
    XRAY_TEST_JOBS          parallelism (default: CPU count)
    XRAY_SKIP_BUILD=1       do not rebuild first
    XRAY_SKIP_BACKEND_DIFF=1  skip the VM/AOT differential net
    XRAY_TEST_DUMP_FAILED=1 dump each failing case's output before the summary

Usage: run_regression_tests.py [--json PATH]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc  # noqa: E402

PROJECT_ROOT = Path(__file__).resolve().parent.parent
TEST_DIR = PROJECT_ROOT / "tests" / "regression"
BACKEND_DIFF = PROJECT_ROOT / "tests" / "diff" / "run_backend_diff.py"

# Directories holding fixtures and importable modules rather than standalone
# cases; `_`-prefixed files are reserved the same way.
EXCLUDED_PARTS = ("fixtures", "modules", "reexport_test")

_ANSI = re.compile(r"\x1b\[[0-9;]*m")
COUNT_PASSED = re.compile(r"(\d+) passed")
COUNT_FAILED = re.compile(r"(\d+) failed")

USE_COLOR = sys.stdout.isatty() and not os.environ.get("NO_COLOR")
GREEN = "\033[0;32m" if USE_COLOR else ""
RED = "\033[0;31m" if USE_COLOR else ""
YELLOW = "\033[1;33m" if USE_COLOR else ""
BLUE = "\033[0;34m" if USE_COLOR else ""
CYAN = "\033[0;36m" if USE_COLOR else ""
NC = "\033[0m" if USE_COLOR else ""

PASS, FAIL, TIMEOUT, SKIP = "PASS", "FAIL", "TIMEOUT", "SKIP"


@dataclass
class CaseResult:
    name: str
    verdict: str
    executed: int
    output: bytes = b""


def find_build_dir() -> Path:
    override = os.environ.get("XRAY_BUILD_DIR")
    if override:
        return Path(override)
    for name in ("build", "build-release"):
        if (PROJECT_ROOT / name / platform.exe_name("xray")).is_file():
            return PROJECT_ROOT / name
    return PROJECT_ROOT / "build"


def find_xray(build_dir: Path) -> Path:
    override = os.environ.get("XRAY_PATH")
    if override and Path(override).is_file():
        return Path(override)
    return build_dir / platform.exe_name("xray")


def executed_count(text: str) -> int:
    """`N passed` + `N failed` from the case's own summary line."""
    plain = _ANSI.sub("", text)
    passed = COUNT_PASSED.search(plain)
    failed = COUNT_FAILED.search(plain)
    return (int(passed.group(1)) if passed else 0) + \
           (int(failed.group(1)) if failed else 0)


def run_one(xray: Path, case: Path, timeout: float) -> CaseResult:
    result = proc.run([xray, "test", case], timeout=timeout)
    output = result.stdout + result.stderr
    count = executed_count(output.decode("utf-8", "replace"))
    if result.timed_out:
        return CaseResult(case.name, TIMEOUT, 0, output)
    if result.ok:
        return CaseResult(case.name, PASS, count)
    return CaseResult(case.name, FAIL, count, output)


def collect_cases() -> List[Path]:
    cases: List[Path] = []
    for path in TEST_DIR.rglob("*.xr"):
        if not path.is_file() or path.name.startswith("_"):
            continue
        if any(part in EXCLUDED_PARTS for part in path.relative_to(TEST_DIR).parts):
            continue
        cases.append(path)
    return sorted(cases)


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description="Run the regression corpus.")
    parser.add_argument("--json", type=Path, default=None,
                        help="also write the result as structured JSON")
    args = parser.parse_args(argv[1:])

    build_dir = find_build_dir()
    xray = find_xray(build_dir)
    timeout = float(platform.env_int("XRAY_TEST_TIMEOUT", 10))
    jobs = platform.env_int("XRAY_TEST_JOBS", platform.cpu_count())
    dump_failed = platform.env_flag("XRAY_TEST_DUMP_FAILED")
    skip_diff = platform.env_flag("XRAY_SKIP_BACKEND_DIFF")

    if not platform.env_flag("XRAY_SKIP_BUILD"):
        print(f"{BLUE}正在构建 {build_dir}...{NC}")
        if not proc.run(["cmake", "--build", build_dir, "-j8"]).ok:
            print(f"{RED}构建失败{NC}")
            return 1
        print(f"{GREEN}构建完成{NC}")

    if not xray.is_file():
        print(f"{RED}错误: 找不到 xray 可执行文件: {xray}{NC}")
        return 1
    if not TEST_DIR.is_dir():
        print(f"{RED}错误: 找不到测试目录 {TEST_DIR}{NC}")
        return 1

    print(f"{BLUE}======================================{NC}")
    print(f"{BLUE}Xray 回归测试运行器{NC}")
    print(f"{BLUE}======================================{NC}")
    print("")
    print(f"测试目录: {TEST_DIR}")
    print(f"并行度: {jobs}  超时: {int(timeout)}s")
    print("")

    cases = collect_cases()
    print(f"{CYAN}运行 {len(cases)} 个测试 ({jobs} 并行)...{NC}")
    print("")

    started = time.time()
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(lambda c: run_one(xray, c, timeout), cases))
    # Sorted by name so the report and the tallies are byte-for-byte
    # deterministic regardless of completion order.
    results.sort(key=lambda r: r.name)

    passed = failed = skipped = executed = 0
    failed_list: List[str] = []
    outputs: Dict[str, bytes] = {}
    for item in results:
        executed += item.executed
        if item.verdict == PASS:
            passed += 1
        elif item.verdict == SKIP:
            skipped += 1
        else:
            failed += 1
            label = (f"{item.name} (timeout)" if item.verdict == TIMEOUT
                     else item.name)
            failed_list.append(label)
            outputs[item.name] = item.output

    # Cross-backend differential net: the same .xr through VM and AOT must
    # produce byte-identical observable output. Folded in here so a divergence
    # fails this run rather than being reported somewhere nobody reads.
    if not skip_diff and BACKEND_DIFF.is_file():
        print(f"{CYAN}运行跨后端差分网 (VM/AOT){NC}")
        diff = proc.run([sys.executable, BACKEND_DIFF, xray])
        if diff.ok:
            passed += 1
        else:
            failed += 1
            failed_list.append("backend_diff")
            outputs["backend_diff"] = diff.stdout + diff.stderr
        print("")

    elapsed = int(time.time() - started)

    print(f"{BLUE}======================================{NC}")
    print(f"{BLUE}测试摘要{NC}")
    print(f"{BLUE}======================================{NC}")
    print(f"总文件数: {len(cases)}")
    print(f"执行测试: {executed}")
    print(f"{GREEN}通过: {passed}{NC}")
    if skipped:
        print(f"{CYAN}跳过: {skipped}{NC}")
    print(f"{RED}失败: {failed}{NC}")
    print(f"耗时: {elapsed} 秒")
    print("")

    if args.json:
        platform.write_text_lf(args.json, json.dumps({
            "total_files": len(cases),
            "executed": executed,
            "passed": passed,
            "skipped": skipped,
            "failed": failed,
            "elapsed_seconds": elapsed,
            "failed_tests": failed_list,
        }, indent=2, ensure_ascii=False) + "\n")

    if not failed:
        print(f"{GREEN}所有测试通过！{NC}")
        print("")
        return 0

    if dump_failed:
        print(f"{YELLOW}--- begin per-test failure output "
              f"(XRAY_TEST_DUMP_FAILED=1) ---{NC}")
        for label in failed_list:
            name = label[:-len(" (timeout)")] if label.endswith(" (timeout)") else label
            blob = outputs.get(name)
            if blob:
                print(f"{RED}>>> {name} >>>{NC}")
                sys.stdout.write(blob.decode("utf-8", "replace"))
                print(f"{RED}<<< {name} <<<{NC}")
                print("")
        print(f"{YELLOW}--- end per-test failure output ---{NC}")
        print("")

    print(f"{RED}失败的测试:{NC}")
    for label in failed_list:
        print(f"  - {label}")
    print("")
    print(f"{YELLOW}提示: 使用 VERBOSE=1 运行单个失败测试查看详细输出{NC}")
    print(f"  {xray} test <test_file>")
    print("")
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
