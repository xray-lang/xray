#!/usr/bin/env python3
"""Run the tests/regression corpus, plus the cross-backend differential net.

Cases with top-level ``@test`` declarations run through ``xray test``;
standalone executable cases run through ``xray run``. Output is captured
through a real file rather than a pipe buffer: a crashing case writes its
report between the last flush and the abort, which is exactly when the output
matters most.

The VM/AOT differential net is an additional gate with its own result, so a
backend divergence fails the run without changing source-case tallies. Opt
out with XRAY_SKIP_BACKEND_DIFF=1 when no AOT host toolchain is available.

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
import base64
import hashlib
import json
import os
import re
import platform as host_platform
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, regression_report  # noqa: E402

PROJECT_ROOT = Path(__file__).resolve().parent.parent
TEST_DIR = PROJECT_ROOT / "tests" / "regression"
BACKEND_DIFF = PROJECT_ROOT / "tests" / "diff" / "run_backend_diff.py"

# Directories holding fixtures and importable modules rather than standalone
# cases; `_`-prefixed files are reserved the same way.
EXCLUDED_PARTS = ("fixtures", "modules", "reexport_test")

TEST_ANNOTATION = re.compile(rb"(?m)^[ \t]*@test(?:[ \t\r\n(]|$)")

USE_COLOR = sys.stdout.isatty() and not os.environ.get("NO_COLOR")
GREEN = "\033[0;32m" if USE_COLOR else ""
RED = "\033[0;31m" if USE_COLOR else ""
YELLOW = "\033[1;33m" if USE_COLOR else ""
BLUE = "\033[0;34m" if USE_COLOR else ""
CYAN = "\033[0;36m" if USE_COLOR else ""
NC = "\033[0m" if USE_COLOR else ""

PASS, FAIL, CRASH, TIMEOUT, SKIP = "PASS", "FAIL", "CRASH", "TIMEOUT", "SKIP"


@dataclass
class CaseResult:
    case_id: str
    verdict: str
    returncode: int
    seconds: float
    stdout: bytes
    stderr: bytes

    @property
    def output(self) -> bytes:
        return self.stdout + self.stderr

    def as_json(self) -> dict:
        return {
            "case_id": self.case_id, "outcome": self.verdict,
            "returncode": self.returncode,
            "seconds": self.seconds,
            "stdout_base64": base64.b64encode(self.stdout).decode("ascii"),
            "stderr_base64": base64.b64encode(self.stderr).decode("ascii"),
        }


def case_id(case: Path) -> str:
    return case.relative_to(PROJECT_ROOT).as_posix()


def file_digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def provenance(xray: Path, build_dir: Path) -> dict:
    """Bind results to the actual binary, configuration and complete source set."""
    inventory = proc.run(["git", "ls-files", "-z", "--cached", "--others",
                          "--exclude-standard"], cwd=PROJECT_ROOT, check=True)
    files = sorted(set(os.fsdecode(name) for name in inventory.stdout.split(b"\0") if name))
    rows = {name: file_digest(PROJECT_ROOT / name) for name in files
            if (PROJECT_ROOT / name).is_file()}
    digest = lambda values: hashlib.sha256(json.dumps(
        values, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    head = proc.run(["git", "rev-parse", "HEAD"], cwd=PROJECT_ROOT, check=True)
    cache = build_dir / "CMakeCache.txt"
    return {
        "head": head.stdout_text().strip(),
        "source_files_sha256": digest(rows),
        "source_file_count": len(rows),
        "stdlib_sha256": digest({k: v for k, v in rows.items() if k.startswith("stdlib/")}),
        "binary": str(xray.resolve()), "binary_sha256": file_digest(xray),
        "cmake_cache_sha256": file_digest(cache) if cache.is_file() else None,
        "platform": host_platform.platform(),
    }


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


def is_test_module(case: Path) -> bool:
    """Return whether the source declares a top-level test annotation."""
    return TEST_ANNOTATION.search(case.read_bytes()) is not None


def run_one(xray: Path, case: Path, timeout: float) -> CaseResult:
    test_module = is_test_module(case)
    verb = "test" if test_module else "run"
    started = time.perf_counter()
    result = proc.run([xray, verb, case], timeout=timeout)
    if result.timed_out:
        verdict = TIMEOUT
    elif result.returncode < 0 or result.returncode >= 0xC0000000:
        verdict = CRASH
    else:
        verdict = PASS if result.ok else FAIL
    return CaseResult(case_id(case), verdict, result.returncode,
                      time.perf_counter() - started, result.stdout, result.stderr)


def collect_cases() -> list[Path]:
    cases: list[Path] = []
    for path in TEST_DIR.rglob("*.xr"):
        if not path.is_file() or path.name.startswith("_"):
            continue
        if any(part in EXCLUDED_PARTS for part in path.relative_to(TEST_DIR).parts):
            continue
        cases.append(path)
    return sorted(cases)


def case_manifest(cases: list[Path]) -> list[dict]:
    return [{"case_id": case_id(case), "source_sha256": file_digest(case),
             "entry": "test" if is_test_module(case) else "run"} for case in cases]


def main(argv: list[str]) -> int:
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
    if not cases:
        print(f"{RED}错误: 没有发现回归用例{NC}")
        return 1
    print(f"{CYAN}运行 {len(cases)} 个测试 ({jobs} 并行)...{NC}")
    print("")

    manifest = case_manifest(cases)
    identity = provenance(xray, build_dir)
    started = time.time()
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(lambda c: run_one(xray, c, timeout), cases))
    results.sort(key=lambda r: r.case_id)
    counts = {outcome: sum(item.verdict == outcome for item in results)
              for outcome in regression_report.OUTCOMES}
    passed, skipped = counts[PASS], counts[SKIP]
    failed_results = [item for item in results
                      if item.verdict in regression_report.FAILED_OUTCOMES]
    failed = len(failed_results)
    backend_diff = None

    # The differential suite is an auxiliary gate, not a regression source
    # case. Keep its outcome out of the source manifest and source tallies.
    if not skip_diff:
        print(f"{CYAN}运行跨后端差分网 (VM/AOT){NC}")
        diff = proc.run([sys.executable, BACKEND_DIFF, xray])
        backend_diff = {
            "outcome": PASS if diff.ok else FAIL,
            "returncode": diff.returncode,
            "stdout_base64": base64.b64encode(diff.stdout).decode("ascii"),
            "stderr_base64": base64.b64encode(diff.stderr).decode("ascii"),
        }
        print("")

    identity_after = provenance(xray, build_dir)
    stable_manifest = case_manifest(collect_cases()) == manifest
    stable_inputs = identity_after == identity and stable_manifest
    elapsed = int(time.time() - started)

    print(f"{BLUE}======================================{NC}")
    print(f"{BLUE}测试摘要{NC}")
    print(f"{BLUE}======================================{NC}")
    print(f"总文件数: {len(cases)}")
    print(f"{GREEN}通过: {passed}{NC}")
    if skipped:
        print(f"{CYAN}跳过: {skipped}{NC}")
    print(f"{RED}失败: {failed} (错误 {counts[FAIL]}, 崩溃 {counts[CRASH]}, "
          f"超时 {counts[TIMEOUT]}){NC}")
    print(f"耗时: {elapsed} 秒")
    print("")

    if args.json:
        args.json.write_text(json.dumps({
            "schema_version": regression_report.SCHEMA_VERSION,
            "manifest": manifest,
            "results": [item.as_json() for item in results],
            "counts": counts,
            "provenance": identity,
            "provenance_after": identity_after,
            "stable_inputs": stable_inputs,
            "stable_manifest": stable_manifest,
            "jobs": jobs, "timeout_seconds": timeout,
            "backend_diff": backend_diff,
            "total_files": len(cases),
            "passed": passed,
            "skipped": skipped,
            "failed": failed,
            "elapsed_seconds": elapsed,
        }, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    auxiliary_failed = backend_diff is not None and backend_diff["outcome"] != PASS
    if not stable_inputs:
        print(f"{RED}运行期间源码、二进制或构建配置发生变化；本次结果不能用于资格判断{NC}")
    if not failed and not skipped and not counts["NOT_RUN"] and not auxiliary_failed and stable_inputs:
        print(f"{GREEN}所有测试通过！{NC}")
        print("")
        return 0

    if dump_failed:
        print(f"{YELLOW}--- begin per-test failure output "
              f"(XRAY_TEST_DUMP_FAILED=1) ---{NC}")
        for item in failed_results:
            print(f"{RED}>>> {item.case_id} ({item.verdict}) >>>{NC}")
            sys.stdout.write(item.output.decode("utf-8", "replace"))
            print(f"{RED}<<< {item.case_id} <<<{NC}")
            print("")
        if auxiliary_failed:
            sys.stdout.write(base64.b64decode(backend_diff["stdout_base64"]).decode("utf-8", "replace"))
            sys.stdout.write(base64.b64decode(backend_diff["stderr_base64"]).decode("utf-8", "replace"))
        print(f"{YELLOW}--- end per-test failure output ---{NC}")
        print("")

    print(f"{RED}失败的测试:{NC}")
    for item in failed_results:
        print(f"  - {item.case_id} ({item.verdict})")
    if auxiliary_failed:
        print("  - backend differential suite (FAIL)")
    print("")
    print(f"{YELLOW}提示: 使用 VERBOSE=1 运行单个失败测试查看详细输出{NC}")
    print(f"  {xray} test <test_file>")
    print("")
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
