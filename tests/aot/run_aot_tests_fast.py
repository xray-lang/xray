#!/usr/bin/env python3
"""Fast AOT VM-vs-native correctness runner.

The shell entrypoint remains the public interface. This runner keeps the same
observable contract while avoiding the old hot path of several shell processes
and temp files per case.
"""

from __future__ import annotations

import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent

NEGATIVE_RE = re.compile(
    r"unsupported .*coroutine Xi op|"
    r"unsupported AOT sync call to suspendable function|"
    r"unsupported AOT indirect call|"
    r"exceptions inside AOT coroutine are unsupported|"
    r"unsupported Xi op ERR_|"
    r"semantic analysis failed|"
    r": error: "
)


def is_uint(value: str) -> bool:
    return value.isdigit()


def configure_jobs(requested: str) -> int:
    if requested in ("", "auto"):
        jobs = os.cpu_count() or 1
        max_auto = os.environ.get("XRAY_AOT_MAX_AUTO_JOBS", "16")
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


def toolchain_key(xray_bin: Path) -> str:
    bin_dir = xray_bin.parent
    material = (
        f"xray {file_key(xray_bin)}\n"
        f"libxray_aot_core.a {file_key(bin_dir / 'libxray_aot_core.a')}\n"
        f"libxray_rt_coro.a {file_key(bin_dir / 'libxray_rt_coro.a')}\n"
        f"libxray_core.a {file_key(bin_dir / 'libxray_core.a')}\n"
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


def cache_root() -> Path:
    return Path(os.environ.get("XRAY_TEST_CACHE_ROOT", str(PROJECT_DIR / "build" / ".xray-test-cache")))


def stable_cache_dir(suite: str, xray_bin: Path) -> Path:
    return cache_root() / suite / toolchain_key(xray_bin)


def shared_cache_dir(suite: str) -> Path:
    return cache_root() / suite


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


def rel_case_name(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(SCRIPT_DIR))
    except ValueError:
        return path.name


def safe_name(rel: str) -> str:
    stem = rel[:-3] if rel.endswith(".xr") else rel
    return "".join(ch if ch.isalnum() or ch in "_.-" else "_" for ch in stem)


def positive_args(path: Path) -> list[str]:
    if path.stem.startswith("process_args"):
        return ["100000", "abc"]
    return []


def head_text(data: bytes, lines: int = 5) -> str:
    return "|".join(data.decode("utf-8", "replace").splitlines()[:lines])


@dataclass
class CaseResult:
    order: int
    status: str
    output: str


@dataclass
class RunnerConfig:
    xray: Path
    jobs: int
    aot_opt: str
    aot_cache: Path
    aot_bin_cache: Path
    negative_cache: Path
    disable_run_cache: bool


def build_aot_binary(config: RunnerConfig, case: Path, rel: str, key: str) -> tuple[Path | None, bytes]:
    name = safe_name(rel)
    bin_dir = config.aot_bin_cache / f"{name}-{key}"
    binary = bin_dir / "aot"
    if binary.is_file() and os.access(binary, os.X_OK):
        return binary, b""

    tmp = bin_dir / f"aot.{os.getpid()}.{threading.get_ident()}"
    lock = bin_dir.with_name(bin_dir.name + ".lock")
    bin_dir.mkdir(parents=True, exist_ok=True)
    if not lock_dir(lock):
        return None, f"cannot lock binary cache: {lock}\n".encode()
    try:
        if binary.is_file() and os.access(binary, os.X_OK):
            return binary, b""
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
        ok = False
        buildlog = b""
        for _attempt in range(3):
            proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
            buildlog = proc.stdout
            if proc.returncode == 0:
                tmp.replace(binary)
                ok = True
                break
            try:
                tmp.unlink()
            except OSError:
                pass
        if not ok:
            return None, buildlog
        return binary, buildlog
    finally:
        unlock_dir(lock)


def run_positive(config: RunnerConfig, order: int, case: Path) -> CaseResult:
    rel = rel_case_name(case)
    test_name = rel[:-3] if rel.endswith(".xr") else rel
    prefix = f"  {test_name:<42}"
    key = case_dir_key(case)
    name = safe_name(rel)
    bin_dir = config.aot_bin_cache / f"{name}-{key}"
    run_key_material = "args:"
    args = positive_args(case)
    if args:
        run_key_material += "\n" + "\n".join(args)
    run_key_material += (
        f"\nXRAY_CORO_DETERMINISTIC={os.environ.get('XRAY_CORO_DETERMINISTIC', '')}"
        f"\nXRAY_CORO_SEED={os.environ.get('XRAY_CORO_SEED', '')}"
    )
    run_dir = bin_dir / f"run-{string_key(run_key_material)}"

    binary, buildlog = build_aot_binary(config, case, rel, key)
    if binary is None:
        return CaseResult(order, "fail", prefix + "FAIL (native build failed after retries)")

    if not config.disable_run_cache and (run_dir / "pass").is_file():
        return CaseResult(order, "pass", prefix + "PASS (cached)")

    vm_cmd = [str(config.xray), "run", str(case)]
    if args:
        vm_cmd.extend(["--", *args])
    aot_cmd = [str(binary), *args]
    vm = subprocess.run(vm_cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    aot = subprocess.run(aot_cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    if vm.returncode != aot.returncode:
        return CaseResult(
            order,
            "fail",
            prefix + f"FAIL (exit code: VM={vm.returncode} AOT={aot.returncode})",
        )
    if vm.stdout != aot.stdout:
        return CaseResult(
            order,
            "fail",
            prefix
            + "FAIL (output mismatch)\n"
            + f"    VM:  {head_text(vm.stdout)}\n"
            + f"    AOT: {head_text(aot.stdout)}",
        )

    if not config.disable_run_cache:
        lock = run_dir.with_name(run_dir.name + ".lock")
        if lock_dir(lock):
            try:
                run_dir.mkdir(parents=True, exist_ok=True)
                (run_dir / "pass").touch()
            finally:
                unlock_dir(lock)
    return CaseResult(order, "pass", prefix + "PASS")


def run_negative(config: RunnerConfig, order: int, case: Path) -> CaseResult:
    rel = rel_case_name(case)
    test_name = rel[:-3] if rel.endswith(".xr") else rel
    prefix = f"  {test_name:<42}"
    key = case_dir_key(case)
    name = safe_name(rel)
    neg_dir = config.negative_cache / f"{name}-{key}"
    cached = neg_dir / "pass"

    if not config.disable_run_cache and cached.is_file():
        return CaseResult(order, "pass", prefix + "PASS (cached rejection)")

    neg_dir.mkdir(parents=True, exist_ok=True)
    lock = neg_dir.with_name(neg_dir.name + ".lock")
    if not lock_dir(lock):
        return CaseResult(order, "fail", prefix + "FAIL (cannot lock negative cache)")
    try:
        if not config.disable_run_cache and cached.is_file():
            return CaseResult(order, "pass", prefix + "PASS (cached rejection)")

        out = neg_dir / f"aot.{os.getpid()}.{threading.get_ident()}"
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
            str(out),
        ]
        env = os.environ.copy()
        env.setdefault("XRAY_AOT_FAST_TEST_BUILD", "1")
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
        try:
            out.unlink()
        except OSError:
            pass
        if proc.returncode == 0:
            return CaseResult(order, "fail", prefix + "FAIL (unexpected AOT success)")
        log = proc.stdout.decode("utf-8", "replace")
        if NEGATIVE_RE.search(log):
            if not config.disable_run_cache:
                cached.touch()
            return CaseResult(order, "pass", prefix + "PASS (rejected)")
        return CaseResult(order, "fail", prefix + "FAIL (wrong rejection)\n    " + head_text(proc.stdout))
    finally:
        unlock_dir(lock)


def collect_cases(shard_total: int, shard_index: int) -> tuple[list[tuple[int, str, Path]], int]:
    cases: list[tuple[int, str, Path]] = []
    all_cases: list[tuple[str, Path]] = []
    for subdir in ("basic", "modules", "coro"):
        directory = SCRIPT_DIR / subdir
        if directory.is_dir():
            for case in sorted(directory.glob("*.xr")):
                all_cases.append(("positive", case))
    neg_dir = SCRIPT_DIR / "negative"
    if neg_dir.is_dir():
        for case in sorted(neg_dir.glob("*.xr")):
            all_cases.append(("negative", case))

    for idx, (kind, case) in enumerate(all_cases):
        if idx % shard_total == shard_index:
            cases.append((idx, kind, case))
    return cases, len(all_cases)


def caches_are_hot(config: RunnerConfig, selected: list[tuple[int, str, Path]]) -> bool:
    for _order, kind, case in selected:
        rel = rel_case_name(case)
        key = case_dir_key(case)
        name = safe_name(rel)
        if kind == "negative":
            if not (config.negative_cache / f"{name}-{key}" / "pass").is_file():
                return False
        elif not (config.aot_bin_cache / f"{name}-{key}" / "aot").is_file():
            return False
    return True


def validate_shard(total: str, index: str) -> tuple[int, int]:
    if not is_uint(total) or not is_uint(index):
        raise ValueError(f"shard config must be numeric: total={total} index={index}")
    total_i = int(total)
    index_i = int(index)
    if total_i < 1:
        raise ValueError("XRAY_AOT_SHARD_TOTAL must be >= 1")
    if index_i >= total_i:
        raise ValueError(f"XRAY_AOT_SHARD_INDEX must be in [0,total): index={index_i} total={total_i}")
    return total_i, index_i


def main(argv: list[str]) -> int:
    xray_raw = argv[1] if len(argv) > 1 else "./build/xray"
    xray = Path(xray_raw)
    requested_jobs = os.environ.get("XRAY_AOT_JOBS", os.environ.get("XRAY_TEST_JOBS", "auto"))
    auto_jobs = requested_jobs in ("", "auto")
    jobs = configure_jobs(requested_jobs)
    aot_opt = os.environ.get("XRAY_AOT_TEST_OPT", "0")
    aot_cache = Path(os.environ.get("XRAY_AOT_CACHE_DIR", str(shared_cache_dir("aot-objects"))))
    aot_bin_cache = Path(
        os.environ.get("XRAY_AOT_BIN_CACHE_DIR", str(stable_cache_dir("aot-bin", xray) / f"O{aot_opt}"))
    )
    negative_cache = Path(
        os.environ.get(
            "XRAY_AOT_NEGATIVE_CACHE_DIR",
            str(stable_cache_dir("aot-negative", xray) / f"O{aot_opt}"),
        )
    )
    disable_run_cache = os.environ.get("XRAY_TEST_DISABLE_RUN_CACHE", "0") == "1"
    try:
        shard_total, shard_index = validate_shard(
            os.environ.get("XRAY_AOT_SHARD_TOTAL", "1"),
            os.environ.get("XRAY_AOT_SHARD_INDEX", "0"),
        )
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if not (xray.is_file() and os.access(xray, os.X_OK)) and shutil.which(str(xray)) is None:
        print(f"FAIL: xray binary not executable: {xray_raw}", file=sys.stderr)
        return 1

    config = RunnerConfig(
        xray=xray,
        jobs=jobs,
        aot_opt=aot_opt,
        aot_cache=aot_cache,
        aot_bin_cache=aot_bin_cache,
        negative_cache=negative_cache,
        disable_run_cache=disable_run_cache,
    )
    selected, total = collect_cases(shard_total, shard_index)
    cache_state = "hot" if not disable_run_cache and caches_are_hot(config, selected) else "cold"
    if auto_jobs and cache_state == "hot":
        jobs = configure_hot_jobs(jobs, "XRAY_AOT_HOT_MAX_AUTO_JOBS", 8)
        config.jobs = jobs

    print("=== AOT VM-AOT Diff Tests ===")
    print(f"Binary: {xray_raw}")
    print(f"Jobs:   {jobs}")
    print(f"CacheState: {cache_state}")
    print(f"Cache:  {aot_cache}")
    print(f"BinCache: {aot_bin_cache}")
    print(f"NegCache: {negative_cache}")
    print(f"AOT opt: -O{aot_opt}")
    if shard_total > 1:
        print(f"Shard: {shard_index} / {shard_total}")
    print("")

    if shard_total > 1:
        print(f"Cases: {len(selected)} / {total}")
        print("")

    results: list[CaseResult] = []
    if jobs <= 1:
        for order, kind, case in selected:
            results.append(run_negative(config, order, case) if kind == "negative" else run_positive(config, order, case))
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
            future_map = {
                executor.submit(
                    run_negative if kind == "negative" else run_positive,
                    config,
                    order,
                    case,
                ): order
                for order, kind, case in selected
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
