#!/usr/bin/env python3
"""Fast VM/AOT differential runner.

This keeps the public shell entrypoint intact while avoiding the old hot-path
cost of several temp files and shell processes per case.
"""

from __future__ import annotations

import concurrent.futures
import os
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent
CASE_DIR = SCRIPT_DIR / "cases"


def is_uint(value: str) -> bool:
    return value.isdigit()


def detect_cores() -> int:
    cores = os.cpu_count() or 1
    return max(1, cores)


def configure_jobs(requested: str) -> int:
    if requested in ("", "auto"):
        jobs = detect_cores()
        max_auto = os.environ.get("XRAY_DIFF_MAX_AUTO_JOBS", "16")
        if is_uint(max_auto) and int(max_auto) > 0:
            jobs = min(jobs, int(max_auto))
        return max(1, jobs)
    if is_uint(requested) and int(requested) > 0:
        return int(requested)
    return 1


def configure_hot_jobs(current: int, env_name: str, default_cap: int) -> int:
    cap_raw = os.environ.get(env_name, str(default_cap))
    if is_uint(cap_raw) and int(cap_raw) > 0:
        return max(1, min(current, int(cap_raw)))
    return current


def run_cksum(args: list[str], data: bytes | None = None) -> tuple[str, str]:
    proc = subprocess.run(["cksum", *args], input=data, stdout=subprocess.PIPE, check=True)
    parts = proc.stdout.decode("ascii", "replace").strip().split()
    if len(parts) < 2:
        return "0", "0"
    return parts[0], parts[1]


def file_key(path: Path) -> str:
    if path.is_file():
        checksum, size = run_cksum([str(path)])
        return f"{checksum}-{size}"
    return "missing"


def string_key(text: str) -> str:
    checksum, size = run_cksum([], text.encode())
    return f"{checksum}-{size}"


def tree_key(root: Path, suffixes: tuple[str, ...]) -> str:
    if not root.is_dir():
        return "missing"
    chunks: list[str] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file() or not path.name.endswith(suffixes):
            continue
        checksum, size = run_cksum([str(path)])
        chunks.append(f"{path.relative_to(root)} {checksum} {size}\n")
    return string_key("".join(chunks))


def toolchain_key(xray_bin: Path) -> str:
    bin_dir = xray_bin.parent
    material = (
        "xray-test-toolchain-cache-schema 2\n"
        f"xray {file_key(xray_bin)}\n"
        f"libxray_aot_core.a {file_key(bin_dir / 'libxray_aot_core.a')}\n"
        f"libxray_rt_coro.a {file_key(bin_dir / 'libxray_rt_coro.a')}\n"
        f"libxray_core.a {file_key(bin_dir / 'libxray_core.a')}\n"
        f"src/aot headers {tree_key(PROJECT_DIR / 'src' / 'aot', ('.h', '.inc.c'))}\n"
        f"src/shared headers {tree_key(PROJECT_DIR / 'src' / 'shared', ('.h', '.inc.c'))}\n"
        f"src/coro headers {tree_key(PROJECT_DIR / 'src' / 'coro', ('.h', '.inc.c'))}\n"
    )
    return string_key(material)


_dir_key_lock = threading.Lock()
_dir_key_cache: dict[Path, str] = {}


def case_dir_key(case_file: Path) -> str:
    directory = case_file.parent
    with _dir_key_lock:
        cached = _dir_key_cache.get(directory)
        if cached:
            return cached

    chunks: list[str] = []
    for pattern in ("*.xr", "*.args"):
        for file in sorted(directory.glob(pattern)):
            if not file.is_file():
                continue
            checksum, size = run_cksum([str(file)])
            chunks.append(f"{file.name} {checksum} {size} {file}\n")
    key = string_key("".join(chunks))
    with _dir_key_lock:
        _dir_key_cache[directory] = key
    return key


def stable_cache_dir(suite: str, xray_bin: Path) -> Path:
    root = Path(os.environ.get("XRAY_TEST_CACHE_ROOT", str(PROJECT_DIR / "build" / ".xray-test-cache")))
    return root / suite / toolchain_key(xray_bin)


def lock_dir(path: Path) -> bool:
    timeout_raw = os.environ.get("XRAY_TEST_LOCK_TIMEOUT", "3000")
    timeout = int(timeout_raw) if is_uint(timeout_raw) else 3000
    waited = 0
    path.parent.mkdir(parents=True, exist_ok=True)
    while True:
        try:
            path.mkdir()
            return True
        except FileExistsError:
            if waited >= timeout:
                return False
            waited += 1
            time.sleep(0.1)
        except OSError:
            return False


def unlock_dir(path: Path) -> None:
    try:
        path.rmdir()
    except OSError:
        pass


def rel_path(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(PROJECT_DIR))
    except ValueError:
        return str(path)


def safe_name(rel: str) -> str:
    stem = rel[:-3] if rel.endswith(".xr") else rel
    return "".join(ch if ch.isalnum() or ch in "_.-" else "_" for ch in stem)


def append_case_manifest(manifest: str) -> list[Path] | None:
    if not manifest:
        return []
    manifest_path = Path(manifest)
    if not manifest_path.is_absolute():
        manifest_path = PROJECT_DIR / manifest
    if not manifest_path.is_file():
        return None

    cases: list[Path] = []
    for raw in manifest_path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        path = Path(line)
        if not path.is_absolute():
            path = PROJECT_DIR / line
        cases.append(path)
    return cases


def read_first_directive(path: Path, prefix: str, max_lines: int) -> str:
    try:
        with path.open("r", encoding="utf-8") as fh:
            for idx, line in enumerate(fh, start=1):
                if idx > max_lines:
                    break
                if line.startswith(prefix):
                    return line[len(prefix) :].strip()
    except OSError:
        pass
    return ""


def read_args(path: Path) -> list[str]:
    argfile = path.with_suffix(".args")
    if not argfile.is_file():
        return []
    try:
        first = argfile.read_text().splitlines()[0]
    except (OSError, IndexError):
        return []
    return first.split()


def head_text(data: bytes, lines: int = 3) -> str:
    text = data.decode("utf-8", "replace")
    return "|".join(text.splitlines()[:lines])


@dataclass
class BackendResult:
    rc: int
    stdout: bytes
    stderr: bytes
    buildlog: bytes = b""


@dataclass
class CaseResult:
    order: int
    status: str
    output: str


@dataclass
class RunnerConfig:
    xray: Path
    backends: list[str]
    jobs: int
    aot_opt: str
    aot_cache: Path
    aot_bin_cache: Path
    diff_stderr: bool


def build_aot_binary(config: RunnerConfig, case: Path, rel: str, case_key: str) -> tuple[Path | None, bytes]:
    safe = safe_name(rel)
    bin_dir = config.aot_bin_cache / f"{safe}-{case_key}"
    binary = bin_dir / "aot"
    if binary.is_file() and os.access(binary, os.X_OK):
        return binary, f"cached: {binary}\n".encode()

    tmp = bin_dir / f"aot.{os.getpid()}.{threading.get_ident()}"
    lock = bin_dir.with_name(bin_dir.name + ".lock")
    bin_dir.mkdir(parents=True, exist_ok=True)
    if not lock_dir(lock):
        return None, f"cannot lock binary cache: {lock}\n".encode()
    try:
        if binary.is_file() and os.access(binary, os.X_OK):
            return binary, f"cached: {binary}\n".encode()
        try:
            tmp.unlink()
        except OSError:
            pass
        cmd = [
            str(config.xray),
            "build",
            "--native",
            "-O",
            config.aot_opt,
            "--cache-dir",
            str(config.aot_cache),
            str(case),
            "-o",
            str(tmp),
        ]
        env = os.environ.copy()
        env.setdefault("XRAY_AOT_FAST_TEST_BUILD", "1")
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
        if proc.returncode != 0:
            try:
                tmp.unlink()
            except OSError:
                pass
            return None, proc.stdout
        tmp.replace(binary)
        return binary, proc.stdout
    finally:
        unlock_dir(lock)


def run_backend(config: RunnerConfig, kind: str, case: Path, args: list[str]) -> BackendResult:
    if kind == "vm":
        cmd = [str(config.xray), "run", str(case)]
        if args:
            cmd.extend(["--", *args])
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return BackendResult(proc.returncode, proc.stdout, proc.stderr)

    if kind == "aot":
        rel = rel_path(case)
        key = case_dir_key(case)
        binary, buildlog = build_aot_binary(config, case, rel, key)
        if binary is None:
            return BackendResult(200, b"BUILDFAIL\n", b"", buildlog)
        proc = subprocess.run([str(binary), *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return BackendResult(proc.returncode, proc.stdout, proc.stderr, buildlog)

    return BackendResult(201, b"BADBACKEND\n", f"unknown backend: {kind}".encode())


def run_case(config: RunnerConfig, order: int, case: Path) -> CaseResult:
    name = rel_path(case)
    anchor = read_first_directive(case, "// anchor: ", 1)
    case_backends_raw = read_first_directive(case, "// diff-backends: ", 5)
    case_backends = [b.strip() for b in case_backends_raw.split(",") if b.strip()]

    enabled: list[str] = []
    excluded: list[str] = []
    for backend in ("vm", "aot"):
        if backend not in config.backends:
            continue
        if case_backends and backend not in case_backends:
            excluded.append(backend)
            continue
        enabled.append(backend)

    prefix = f"  {name:<84}"
    if len(enabled) < 2:
        return CaseResult(order, "skip", prefix + f"SKIP (need >=2 backends; case={case_backends_raw or 'all'} global={','.join(config.backends)})")

    args = read_args(case)
    results: dict[str, BackendResult] = {}
    for backend in enabled:
        results[backend] = run_backend(config, backend, case, args)

    ref = enabled[0]
    ref_result = results[ref]
    mismatch = ""
    other = ""
    for backend in enabled[1:]:
        cur = results[backend]
        if cur.rc != ref_result.rc:
            mismatch = f"exit code ({ref}={ref_result.rc} {backend}={cur.rc})"
            other = backend
            break
        if cur.stdout != ref_result.stdout:
            mismatch = f"stdout ({ref} vs {backend})"
            other = backend
            break
        if config.diff_stderr and cur.stderr != ref_result.stderr:
            mismatch = f"stderr ({ref} vs {backend})"
            other = backend
            break

    if not mismatch:
        suffix = f"PASS (excl:{' '.join(excluded)})" if excluded else "PASS"
        return CaseResult(order, "pass", prefix + suffix)

    lines = [prefix + f"FAIL ({mismatch})" + (f"  [anchor: {anchor}]" if anchor else "")]
    for backend in enabled:
        res = results[backend]
        lines.append(f"      {backend}: rc={res.rc}  stdout: {head_text(res.stdout)}")
        if res.rc == 200 and res.buildlog:
            lines.extend("      " + line for line in res.buildlog.decode("utf-8", "replace").splitlines()[:20])
    if other:
        lhs = ref_result.stdout if mismatch.startswith("stdout") else ref_result.stderr
        rhs = results[other].stdout if mismatch.startswith("stdout") else results[other].stderr
        lines.append(f"      {ref}: {head_text(lhs, 6)}")
        lines.append(f"      {other}: {head_text(rhs, 6)}")
    return CaseResult(order, "fail", "\n".join(lines))


def collect_cases(base_cases_file: str, extra_cases_file: str) -> list[Path]:
    if base_cases_file:
        base = append_case_manifest(base_cases_file)
        if base is None:
            raise FileNotFoundError(f"base case manifest not found: {base_cases_file}")
        cases = sorted(base)
    else:
        cases = sorted(CASE_DIR.rglob("*.xr"))

    if extra_cases_file:
        extra = append_case_manifest(extra_cases_file)
        if extra:
            cases.extend(extra)
    return cases


def aot_binary_cache_hot(config: RunnerConfig, selected: list[tuple[int, Path]]) -> bool:
    if "aot" not in config.backends:
        return True
    for _order, case in selected:
        case_backends_raw = read_first_directive(case, "// diff-backends: ", 5)
        case_backends = [b.strip() for b in case_backends_raw.split(",") if b.strip()]
        if case_backends and "aot" not in case_backends:
            continue
        rel = rel_path(case)
        key = case_dir_key(case)
        binary = config.aot_bin_cache / f"{safe_name(rel)}-{key}" / "aot"
        if not (binary.is_file() and os.access(binary, os.X_OK)):
            return False
    return True


def validate_shard(total: str, index: str) -> tuple[int, int]:
    if not is_uint(total) or not is_uint(index):
        raise ValueError(f"shard config must be numeric: total={total} index={index}")
    total_i = int(total)
    index_i = int(index)
    if total_i < 1:
        raise ValueError("XRAY_DIFF_SHARD_TOTAL must be >= 1")
    if index_i >= total_i:
        raise ValueError(f"XRAY_DIFF_SHARD_INDEX must be in [0,total): index={index_i} total={total_i}")
    return total_i, index_i


def main(argv: list[str]) -> int:
    xray_raw = argv[1] if len(argv) > 1 else os.environ.get("XRAY_BIN", "")
    if not xray_raw:
        build_dir = os.environ.get("XRAY_BUILD_DIR")
        xray_raw = str(Path(build_dir) / "xray") if build_dir else str(PROJECT_DIR / "build" / "xray")
    xray = Path(xray_raw)

    backends = [b.strip() for b in os.environ.get("XRAY_DIFF_BACKENDS", "vm,aot").split(",") if b.strip()]
    requested_jobs = os.environ.get("XRAY_DIFF_JOBS", os.environ.get("XRAY_TEST_JOBS", "auto"))
    auto_jobs = requested_jobs in ("", "auto")
    jobs = configure_jobs(requested_jobs)
    aot_opt = os.environ.get("XRAY_AOT_TEST_OPT", "0")
    aot_cache = Path(
        os.environ.get("XRAY_DIFF_CACHE_DIR", str(stable_cache_dir("aot-objects", xray) / f"O{aot_opt}"))
    )
    aot_bin_cache = Path(
        os.environ.get("XRAY_DIFF_BIN_CACHE_DIR", str(stable_cache_dir("backend-diff-bin", xray) / f"O{aot_opt}"))
    )
    diff_stderr = os.environ.get("XRAY_DIFF_STDERR", "0") == "1"
    shard_total_raw = os.environ.get("XRAY_DIFF_SHARD_TOTAL", "1")
    shard_index_raw = os.environ.get("XRAY_DIFF_SHARD_INDEX", "0")
    single_case = os.environ.get("XRAY_DIFF_SINGLE_CASE", "")
    single_id_raw = os.environ.get("XRAY_DIFF_SINGLE_ID", "0")

    try:
        shard_total, shard_index = validate_shard(shard_total_raw, shard_index_raw)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    config = RunnerConfig(
        xray=xray,
        backends=backends,
        jobs=jobs,
        aot_opt=aot_opt,
        aot_cache=aot_cache,
        aot_bin_cache=aot_bin_cache,
        diff_stderr=diff_stderr,
    )

    if single_case:
        result = run_case(config, int(single_id_raw) if is_uint(single_id_raw) else 0, Path(single_case))
        print(result.output)
        return 0 if result.status != "fail" else 1

    if not (xray.is_file() and os.access(xray, os.X_OK)) and shutil.which(str(xray)) is None:
        print(f"SKIP: xray binary not found: {xray_raw}")
        print("=== Results: 0 passed, 0 failed, 0 skipped ===")
        return 0
    if not CASE_DIR.is_dir():
        print(f"SKIP: no case dir {CASE_DIR}")
        print("=== Results: 0 passed, 0 failed, 0 skipped ===")
        return 0

    base_cases_file = os.environ.get("XRAY_DIFF_CASES_FILE", "")
    if "XRAY_DIFF_EXTRA_CASES_FILE" in os.environ:
        extra_cases_file = os.environ["XRAY_DIFF_EXTRA_CASES_FILE"]
    elif base_cases_file:
        extra_cases_file = ""
    else:
        extra_cases_file = str(SCRIPT_DIR / "coro_regression_cases.txt")

    try:
        all_cases = collect_cases(base_cases_file, extra_cases_file)
    except FileNotFoundError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    selected: list[tuple[int, Path]] = []
    case_index = 0
    for case in all_cases:
        if not case.is_file() or case.name.startswith("_"):
            continue
        if case_index % shard_total == shard_index:
            selected.append((case_index, case))
        case_index += 1

    cache_state = "hot" if aot_binary_cache_hot(config, selected) else "cold"
    if auto_jobs and cache_state == "cold":
        jobs = configure_hot_jobs(jobs, "XRAY_DIFF_COLD_MAX_AUTO_JOBS", 4)
        config.jobs = jobs
    elif auto_jobs and cache_state == "hot":
        jobs = configure_hot_jobs(jobs, "XRAY_DIFF_HOT_MAX_AUTO_JOBS", 8)
        config.jobs = jobs

    print("=== Backend Differential (VM / AOT) ===")
    print(f"Binary:   {xray_raw}")
    print(f"Backends: {','.join(backends)}")
    print(f"Jobs:     {jobs}")
    print(f"CacheState: {cache_state}")
    print(f"AOT opt:  -O{aot_opt}")
    print(f"Cache:    {aot_cache}")
    print(f"BinCache: {aot_bin_cache}")
    print("RunCache: disabled")
    if shard_total > 1:
        print(f"Shard:    {shard_index} / {shard_total}")
    print("")

    if shard_total > 1:
        print(f"Cases:    {len(selected)} / {case_index}")
        print("")

    results: list[CaseResult] = []
    if jobs <= 1:
        for order, case in selected:
            results.append(run_case(config, order, case))
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
            future_map = {
                executor.submit(run_case, config, order, case): order
                for order, case in selected
            }
            for future in concurrent.futures.as_completed(future_map):
                results.append(future.result())

    passed = failed = skipped = 0
    for result in sorted(results, key=lambda item: item.order):
        print(result.output)
        if result.status == "pass":
            passed += 1
        elif result.status == "skip":
            skipped += 1
        else:
            failed += 1

    print("")
    print(f"=== Results: {passed} passed, {failed} failed, {skipped} skipped ===")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
