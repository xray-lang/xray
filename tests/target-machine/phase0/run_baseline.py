#!/usr/bin/env python3
"""Generate and verify reproducible target-machine baseline evidence."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import math
import os
import platform as host_platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[2] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform  # noqa: E402


if sys.version_info < (3, 11):
    raise SystemExit("run_baseline.py requires Python 3.11 or newer")


EVIDENCE_SCHEMA = 2
POLICY_SCHEMA = 1
RUNNER_VERSION = "target-machine-baseline/3"
DEFAULT_POLICY = Path("contracts/target-machine/baseline-manifest.json")


@dataclass(frozen=True)
class Lane:
    name: str
    ctest_args: tuple[str, ...]
    timeout_seconds: int
    repeat: bool


def run_capture(command: list[str], cwd: Path, timeout: float,
                env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
        check=False,
        env={**(env or os.environ), "NO_COLOR": "1"},
    )


def captured_text(value: str | bytes | None) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value or ""


def command_text(command: list[str], root: Path, output: Path | None = None) -> list[str]:
    replacements = [(str(root), "${SOURCE_ROOT}")]
    if output:
        replacements.insert(0, (str(output), "${EVIDENCE_ROOT}"))
    rendered = []
    for part in command:
        for source, replacement in replacements:
            part = part.replace(source, replacement)
        rendered.append(part)
    return rendered


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def git(root: Path, *args: str) -> str:
    result = run_capture(["git", *args], root, 30)
    if result.returncode != 0:
        raise RuntimeError(result.stdout.strip() or "git command failed")
    return result.stdout.strip()


def tool_version(command: list[str], root: Path) -> str:
    try:
        result = run_capture(command, root, 30)
    except (OSError, subprocess.SubprocessError):
        return "unavailable"
    lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    return lines[0] if lines else "unavailable"


def compiler_binary_path(build: Path) -> Path:
    return build / platform.exe_name("xray")


def resolve_policy_path(root: Path, value: str | Path) -> Path:
    path = Path(value)
    return path.resolve() if path.is_absolute() else (root / path).resolve()


def load_policy(root: Path, path: Path) -> dict[str, Any]:
    try:
        policy = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot load baseline policy {path}: {error}") from error
    errors = validate_policy(root, policy)
    if errors:
        raise RuntimeError("invalid baseline policy: " + "; ".join(errors))
    return policy


def validate_policy(root: Path, policy: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if policy.get("schema") != POLICY_SCHEMA:
        errors.append(f"policy schema must be {POLICY_SCHEMA}")
    if policy.get("runner") != RUNNER_VERSION:
        errors.append(f"policy runner must be {RUNNER_VERSION}")
    if policy.get("status_values") != ["passed", "failed"]:
        errors.append("policy status values must be exactly passed/failed")

    qualification = policy.get("qualification", {})
    if qualification.get("result") not in ("passed", "failed"):
        errors.append("qualification result must be passed or failed")
    if qualification.get("result") == "passed":
        if not isinstance(qualification.get("evidence"), str):
            errors.append("a passed qualification requires an evidence path")
        if not re.fullmatch(r"[0-9a-f]{64}", str(qualification.get("evidence_sha256", ""))):
            errors.append("a passed qualification requires an evidence sha256")
    elif qualification.get("evidence") is not None or qualification.get("evidence_sha256") is not None:
        errors.append("a failed qualification must not cite qualifying evidence")

    identity = policy.get("identity", {})
    required_identity = (
        "require_clean_worktree",
        "require_binary_commit_match",
        "require_binary_dirty_match",
        "require_cmake_home_match",
        "require_native_toolchain_probe",
    )
    for key in required_identity:
        if identity.get(key) is not True:
            errors.append(f"identity.{key} must be true")
    if identity.get("generator") != "Ninja" or identity.get("build_type") != "Release":
        errors.append("baseline build identity must require Ninja Release")
    for key in ("configure_preset", "build_preset", "build_directory"):
        if not isinstance(identity.get(key), str) or not identity.get(key):
            errors.append(f"identity.{key} must be a non-empty string")
    for key in ("configure_timeout_seconds", "build_timeout_seconds", "build_parallelism"):
        if not isinstance(identity.get(key), int) or identity.get(key, 0) < 1:
            errors.append(f"identity.{key} must be a positive integer")

    residue = policy.get("residue", {}).get("source_root_globs")
    if not isinstance(residue, list) or not residue or not all(isinstance(v, str) for v in residue):
        errors.append("residue source_root_globs must be a non-empty string list")

    correctness = policy.get("correctness", {})
    minimum_repeat = correctness.get("minimum_repeat")
    if not isinstance(minimum_repeat, int) or minimum_repeat < 3:
        errors.append("correctness minimum_repeat must be at least 3")
    lane_names: set[str] = set()
    for row in correctness.get("lanes", []):
        name = row.get("name")
        if not isinstance(name, str) or not name or name in lane_names:
            errors.append(f"invalid or duplicate correctness lane {name!r}")
        else:
            lane_names.add(name)
        if not isinstance(row.get("ctest_args"), list) or not row.get("ctest_args"):
            errors.append(f"correctness lane {name!r} has no ctest_args")
        scopes = row.get("scopes")
        if not isinstance(scopes, list) or not scopes or any(v not in ("core", "full") for v in scopes):
            errors.append(f"correctness lane {name!r} has invalid scopes")
        if not isinstance(row.get("timeout_seconds"), int) or row.get("timeout_seconds", 0) < 1:
            errors.append(f"correctness lane {name!r} has invalid timeout")

    performance = policy.get("performance", {})
    logical_cpu = performance.get("logical_cpu")
    if not isinstance(logical_cpu, int) or isinstance(logical_cpu, bool) or logical_cpu < 0:
        errors.append("performance logical_cpu must be a non-negative integer")
    if not isinstance(performance.get("sample_count"), int) or performance.get("sample_count", 0) < 5:
        errors.append("performance sample_count must be at least 5")
    max_cv = performance.get("max_coefficient_of_variation")
    if not isinstance(max_cv, (int, float)) or not 0 < max_cv <= 1:
        errors.append("performance max_coefficient_of_variation must be in (0, 1]")
    interval = performance.get("rss_sample_interval_seconds")
    if not isinstance(interval, (int, float)) or not 0 < interval <= 1:
        errors.append("performance RSS interval must be in (0, 1]")
    if not isinstance(performance.get("timeout_seconds"), int) or performance.get("timeout_seconds", 0) < 1:
        errors.append("performance timeout_seconds must be positive")
    power_policy = performance.get("power_policy", {})
    if os.name == "nt":
        guid = power_policy.get("windows_active_scheme_guid")
        if not isinstance(guid, str) or re.fullmatch(
            r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}", guid
        ) is None:
            errors.append("performance power policy must govern a Windows scheme GUID")
    elif sys.platform.startswith("linux"):
        governor = power_policy.get("linux_scaling_governor")
        if not isinstance(governor, str) or not governor:
            errors.append("performance power policy must govern a Linux scaling governor")

    modes: set[str] = set()
    performance_names: set[str] = set()
    for row in performance.get("lanes", []):
        name = row.get("name")
        mode = row.get("mode")
        if not isinstance(name, str) or not name or name in performance_names:
            errors.append(f"invalid or duplicate performance lane {name!r}")
        else:
            performance_names.add(name)
        if mode not in ("cold", "warm", "edit") or mode in modes:
            errors.append(f"invalid or duplicate performance mode {mode!r}")
        else:
            modes.add(mode)
        if not isinstance(row.get("warmups"), int) or row.get("warmups", -1) < 0:
            errors.append(f"performance lane {name!r} has invalid warmups")
    if modes != {"cold", "warm", "edit"}:
        errors.append("performance lanes must cover cold, warm, and edit exactly once")

    fixture = performance.get("fixture", {})
    directory_value = fixture.get("directory")
    if not isinstance(directory_value, str):
        errors.append("performance fixture directory is missing")
        return errors
    directory = (root / directory_value).resolve()
    if not directory.is_relative_to(root):
        errors.append("performance fixture escapes the source root")
    files = fixture.get("files")
    if not isinstance(files, list) or not files:
        errors.append("performance fixture file inventory is missing")
    else:
        for row in files:
            relative = row.get("path")
            expected = row.get("sha256")
            path = directory / relative if isinstance(relative, str) else directory
            if not isinstance(relative, str) or not isinstance(expected, str):
                errors.append("performance fixture inventory row is malformed")
            elif not path.is_file():
                errors.append(f"performance fixture is missing: {path}")
            elif sha256(path) != expected:
                errors.append(f"performance fixture digest drift: {relative}")
    for key in ("entry", "edit_file", "edited_source", "base_stdout", "edited_stdout"):
        if not isinstance(fixture.get(key), str):
            errors.append(f"performance fixture {key} is missing")
    return errors


def source_root_residue(root: Path, policy: dict[str, Any]) -> list[str]:
    found: set[str] = set()
    for pattern in policy["residue"]["source_root_globs"]:
        for path in root.glob(pattern):
            if path.is_file():
                found.add(path.relative_to(root).as_posix())
    return sorted(found)


def clean_source_snapshot(root: Path, policy: dict[str, Any]) -> dict[str, Any]:
    residue = source_root_residue(root, policy)
    status = git(root, "status", "--porcelain=v1", "--untracked-files=all")
    errors = []
    if residue:
        errors.append("source-root temporary residue: " + ", ".join(residue))
    if status:
        errors.append("worktree is dirty")
    if errors:
        raise RuntimeError("; ".join(errors))
    return {
        "git_commit": git(root, "rev-parse", "HEAD"),
        "git_tree": git(root, "rev-parse", "HEAD^{tree}"),
        "git_dirty": False,
    }


def run_preflight_step(command: list[str], root: Path, output: Path, name: str,
                       timeout_seconds: int, policy: dict[str, Any]) -> dict[str, Any]:
    started = time.perf_counter()
    timed_out = False
    try:
        result = run_capture(command, root, timeout_seconds)
        returncode = result.returncode
        content = result.stdout
    except subprocess.TimeoutExpired as error:
        timed_out = True
        returncode = 124
        content = captured_text(error.stdout) + "\nTIMEOUT\n"
    log = output / "preflight" / f"{name}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(content, encoding="utf-8")
    residue = source_root_residue(root, policy)
    status = "passed" if returncode == 0 and not timed_out and not residue else "failed"
    return {
        "name": name,
        "status": status,
        "returncode": returncode,
        "timed_out": timed_out,
        "duration_seconds": round(time.perf_counter() - started, 6),
        "source_root_residue": residue,
        "command": command_text(command, root, output),
        "log": log.relative_to(output).as_posix(),
        "log_sha256": sha256(log),
    }


def run_freshness_preflight(root: Path, build: Path, output: Path,
                            policy: dict[str, Any]) -> list[dict[str, Any]]:
    identity = policy["identity"]
    expected_build = (root / identity["build_directory"]).resolve()
    if build != expected_build:
        raise RuntimeError(f"baseline build directory {build} != governed {expected_build}")
    steps = [
        run_preflight_step(
            ["cmake", "--preset", identity["configure_preset"]], root, output, "configure",
            identity["configure_timeout_seconds"], policy,
        )
    ]
    if steps[-1]["status"] == "passed":
        steps.append(run_preflight_step(
            ["cmake", "--build", "--preset", identity["build_preset"], "--parallel",
             str(identity["build_parallelism"])], root, output, "build",
            identity["build_timeout_seconds"], policy,
        ))
    return steps


def compiler_identity(root: Path, build: Path, policy: dict[str, Any]) -> dict[str, Any]:
    binary = compiler_binary_path(build)
    if not binary.is_file():
        raise RuntimeError(f"compiler binary missing: {binary}")
    snapshot = clean_source_snapshot(root, policy)
    status = ""
    head = snapshot["git_commit"]
    tree = snapshot["git_tree"]
    result = run_capture([str(binary), "--version", "--json"], root, 30)
    if result.returncode != 0:
        raise RuntimeError(f"compiler version query failed: {result.stdout.strip()}")
    try:
        version = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"compiler version is not JSON: {error}") from error
    errors = []
    if version.get("commit") != head:
        errors.append(f"binary commit {version.get('commit')} != HEAD {head}")
    if bool(version.get("dirty")) != bool(status):
        errors.append("binary dirty flag does not match Git status")
    if status:
        errors.append("worktree is dirty")
    if errors:
        raise RuntimeError("; ".join(errors))
    return {
        "git_commit": head,
        "git_tree": tree,
        "git_dirty": False,
        "version": version,
        "binary": file_identity(binary),
    }


def cache_values(build: Path) -> dict[str, str]:
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        raise RuntimeError(f"CMake cache missing: {cache}")
    values: dict[str, str] = {}
    for line in cache.read_text(encoding="utf-8", errors="strict").splitlines():
        if not line or line.startswith(("#", "//")) or ":" not in line or "=" not in line:
            continue
        name, rest = line.split(":", 1)
        _, value = rest.split("=", 1)
        values[name] = value.strip()
    return values


def cmake_identity_values(build: Path) -> dict[str, str]:
    values = cache_values(build)
    candidates = sorted((build / "CMakeFiles").glob("*/CMakeCCompiler.cmake"))
    if not candidates:
        candidates = sorted((build / "CMakeFiles").glob("*/CMakeSystem.cmake"))
    for candidate in candidates:
        text = candidate.read_text(encoding="utf-8", errors="strict")
        for name, value in re.findall(r'^set\((CMAKE_[A-Z0-9_]+) "([^"]*)"\)', text, re.M):
            values.setdefault(name, value)
    return values


def file_identity(path: Path) -> dict[str, Any]:
    resolved = path.resolve()
    if not resolved.is_file():
        raise RuntimeError(f"identity file missing: {resolved}")
    return {
        "path": str(resolved),
        "sha256": sha256(resolved),
        "size_bytes": resolved.stat().st_size,
    }


def optional_tool_identity(value: str) -> dict[str, Any]:
    path = Path(value) if value else Path()
    if value and not path.is_absolute():
        found = shutil.which(value)
        path = Path(found) if found else path
    if value and path.is_file():
        return file_identity(path)
    return {"path": value or "unknown", "sha256": None, "size_bytes": None}


def total_memory_bytes() -> int | None:
    if os.name == "nt":
        class MemoryStatus(ctypes.Structure):
            _fields_ = [
                ("length", ctypes.c_ulong),
                ("memory_load", ctypes.c_ulong),
                ("total_phys", ctypes.c_ulonglong),
                ("avail_phys", ctypes.c_ulonglong),
                ("total_page_file", ctypes.c_ulonglong),
                ("avail_page_file", ctypes.c_ulonglong),
                ("total_virtual", ctypes.c_ulonglong),
                ("avail_virtual", ctypes.c_ulonglong),
                ("avail_extended_virtual", ctypes.c_ulonglong),
            ]

        status = MemoryStatus()
        status.length = ctypes.sizeof(status)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            return int(status.total_phys)
        return None
    try:
        return int(os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES"))
    except (AttributeError, OSError, ValueError):
        return None


def affinity_from_windows_handle(handle: int | ctypes.c_void_p) -> list[int] | str:
    process_mask = ctypes.c_size_t()
    system_mask = ctypes.c_size_t()
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetProcessAffinityMask.argtypes = (
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)
    )
    kernel32.GetProcessAffinityMask.restype = ctypes.c_int
    if kernel32.GetProcessAffinityMask(
        ctypes.c_void_p(handle), ctypes.byref(process_mask), ctypes.byref(system_mask)
    ):
        return [index for index in range(process_mask.value.bit_length())
                if process_mask.value & (1 << index)]
    return "unavailable"


def process_affinity() -> list[int] | str:
    if hasattr(os, "sched_getaffinity"):
        return sorted(os.sched_getaffinity(0))
    if os.name == "nt":
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GetCurrentProcess.restype = ctypes.c_void_p
        return affinity_from_windows_handle(kernel32.GetCurrentProcess())
    return "unavailable"


def set_process_affinity(cpus: list[int]) -> None:
    if not cpus:
        raise RuntimeError("measurement affinity cannot be empty")
    if hasattr(os, "sched_setaffinity"):
        os.sched_setaffinity(0, set(cpus))
    elif os.name == "nt":
        mask = sum(1 << cpu for cpu in cpus)
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GetCurrentProcess.restype = ctypes.c_void_p
        kernel32.SetProcessAffinityMask.argtypes = (ctypes.c_void_p, ctypes.c_size_t)
        kernel32.SetProcessAffinityMask.restype = ctypes.c_int
        handle = kernel32.GetCurrentProcess()
        if not kernel32.SetProcessAffinityMask(handle, ctypes.c_size_t(mask)):
            raise ctypes.WinError()
    else:
        raise RuntimeError("process affinity control is unavailable on this host")
    if process_affinity() != sorted(cpus):
        raise RuntimeError("process affinity did not match the requested logical CPUs")


@contextmanager
def fixed_affinity(logical_cpu: int):
    previous = process_affinity()
    if not isinstance(previous, list) or logical_cpu not in previous:
        raise RuntimeError(
            f"logical CPU {logical_cpu} is not available in process affinity {previous!r}"
        )
    set_process_affinity([logical_cpu])
    try:
        yield
    finally:
        set_process_affinity(previous)


def active_power_policy(root: Path, policy: dict[str, Any],
                        require_performance: bool) -> dict[str, Any]:
    if not require_performance:
        return {"required": False}
    governed = policy["performance"]["power_policy"]
    if os.name == "nt":
        expected = governed["windows_active_scheme_guid"]
        result = run_capture(["powercfg", "/getactivescheme"], root, 30)
        output = result.stdout.strip()
        match = re.search(
            r"(?i)([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})",
            output,
        )
        if result.returncode != 0 or not match:
            raise RuntimeError("cannot query the active Windows power scheme: " + output)
        active = match.group(1).lower()
        if active != expected.lower():
            raise RuntimeError(f"active Windows power scheme {active} != expected {expected}")
        return {
            "required": True,
            "kind": "windows-power-scheme-guid",
            "expected": expected.lower(),
            "active": active,
            "query_sha256": sha256_bytes((output + "\n").encode("utf-8")),
        }
    if sys.platform.startswith("linux"):
        expected = governed["linux_scaling_governor"]
        files = sorted(Path("/sys/devices/system/cpu").glob("cpu[0-9]*/cpufreq/scaling_governor"))
        governors = sorted({path.read_text(encoding="ascii").strip() for path in files})
        if not governors:
            raise RuntimeError("cannot query Linux CPU scaling governors")
        if governors != [expected]:
            raise RuntimeError(f"active Linux CPU governors {governors!r} != expected {expected!r}")
        return {
            "required": True,
            "kind": "linux-scaling-governor",
            "expected": expected,
            "active": expected,
            "query_sha256": sha256_bytes("\n".join(governors).encode("utf-8")),
        }
    raise RuntimeError("verified power-policy capture is unavailable on this host")


def host_and_toolchain_info(root: Path, build: Path, binary: Path,
                            require_performance: bool,
                            policy: dict[str, Any]) -> dict[str, Any]:
    values = cmake_identity_values(build)
    expected = policy["identity"]
    errors = []
    home = values.get("CMAKE_HOME_DIRECTORY", "")
    if not home or Path(home).resolve() != root:
        errors.append(f"CMake home {home!r} does not match {root}")
    if values.get("CMAKE_GENERATOR") != expected["generator"]:
        errors.append(f"CMake generator is {values.get('CMAKE_GENERATOR')!r}")
    if values.get("CMAKE_BUILD_TYPE") != expected["build_type"]:
        errors.append(f"CMake build type is {values.get('CMAKE_BUILD_TYPE')!r}")
    compiler_value = values.get("CMAKE_C_COMPILER", "")
    if not compiler_value:
        errors.append("CMAKE_C_COMPILER is missing")
    allowed_affinity = process_affinity()
    logical_cpu = policy["performance"]["logical_cpu"]
    if require_performance and (
        not isinstance(allowed_affinity, list) or logical_cpu not in allowed_affinity
    ):
        errors.append(f"logical CPU {logical_cpu} is unavailable in affinity {allowed_affinity!r}")
    if errors:
        raise RuntimeError("; ".join(errors))

    probe_home = build / "target-machine" / "toolchain-identity"
    probe = run_capture(
        [str(binary), "toolchain", "probe", "--json", "--no-run"], root, 120,
        controlled_environment(probe_home),
    )
    if probe.returncode != 0:
        raise RuntimeError("native toolchain probe failed: " + probe.stdout.strip())
    try:
        probe_json = json.loads(probe.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"native toolchain probe is not JSON: {error}") from error
    probe_canonical = json.dumps(probe_json, sort_keys=True, separators=(",", ":")).encode("utf-8")
    power = active_power_policy(root, policy, require_performance)

    cpu = host_platform.processor() or host_platform.machine()
    if sys.platform == "darwin":
        result = run_capture(["sysctl", "-n", "machdep.cpu.brand_string"], root, 10)
        if result.returncode == 0 and result.stdout.strip():
            cpu = result.stdout.strip()
    return {
        "host": {
            "os": host_platform.platform(),
            "system": host_platform.system(),
            "release": host_platform.release(),
            "machine": host_platform.machine(),
            "cpu": cpu,
            "logical_cpus": os.cpu_count(),
            "affinity": allowed_affinity,
            "measurement_affinity": [logical_cpu] if require_performance else [],
            "ram_bytes": total_memory_bytes(),
            "power_policy": power,
        },
        "build": {
            "cmake_home": str(Path(home).resolve()),
            "generator": values.get("CMAKE_GENERATOR"),
            "build_type": values.get("CMAKE_BUILD_TYPE"),
            "c_compiler_id": values.get("CMAKE_C_COMPILER_ID"),
            "c_compiler_version": values.get("CMAKE_C_COMPILER_VERSION"),
            "c_compiler": optional_tool_identity(compiler_value),
            "linker": optional_tool_identity(values.get("CMAKE_LINKER", "")),
            "xray_python": values.get("XRAY_PYTHON"),
        },
        "tools": {
            "python": {
                "version": host_platform.python_version(),
                **file_identity(Path(sys.executable)),
            },
            "cmake": tool_version(["cmake", "--version"], root),
            "ninja": tool_version(["ninja", "--version"], root),
        },
        "native_toolchain_probe": probe_json,
        "native_toolchain_probe_sha256": sha256_bytes(probe_canonical),
        "inherited_toolchain_environment_sha256": sha256_bytes(json.dumps(
            {name: os.environ.get(name) for name in ("PATH", "INCLUDE", "LIB", "LIBPATH", "SDKROOT")},
            sort_keys=True, separators=(",", ":"),
        ).encode("utf-8")),
    }


def parse_ctest_summary(output: str) -> dict[str, int | None]:
    match = re.search(r"([0-9]+)% tests passed, ([0-9]+) tests failed out of ([0-9]+)", output)
    if not match:
        return {"percent_passed": None, "failed": None, "total": None}
    return {
        "percent_passed": int(match.group(1)),
        "failed": int(match.group(2)),
        "total": int(match.group(3)),
    }


def percentile(values: list[float], quantile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    position = (len(ordered) - 1) * quantile
    low = math.floor(position)
    high = math.ceil(position)
    if low == high:
        return ordered[low]
    fraction = position - low
    return ordered[low] * (1.0 - fraction) + ordered[high] * fraction


def distribution(values: list[float], unit: str) -> dict[str, Any]:
    mean = statistics.fmean(values) if values else 0.0
    deviation = statistics.pstdev(values) if len(values) > 1 else 0.0
    return {
        "samples": len(values),
        "unit": unit,
        "p50": round(percentile(values, 0.50), 6),
        "p95": round(percentile(values, 0.95), 6),
        "mean": round(mean, 6),
        "coefficient_of_variation": round(deviation / mean, 6) if mean else 0.0,
    }


def correctness_lanes(policy: dict[str, Any], scope: str) -> list[Lane]:
    rows = []
    for row in policy["correctness"]["lanes"]:
        if scope in row["scopes"]:
            rows.append(Lane(row["name"], tuple(row["ctest_args"]), row["timeout_seconds"],
                             bool(row["repeat"])))
    return rows


def execute_lane(root: Path, build: Path, output: Path, lane: Lane, iteration: int,
                 policy: dict[str, Any]) -> dict[str, Any]:
    command = ["ctest", "--test-dir", str(build), "--output-on-failure", *lane.ctest_args]
    started = time.perf_counter()
    timed_out = False
    try:
        result = run_capture(command, root, lane.timeout_seconds)
        returncode = result.returncode
        content = result.stdout
    except subprocess.TimeoutExpired as error:
        timed_out = True
        returncode = 124
        content = captured_text(error.stdout) + "\nTIMEOUT\n"
    duration = time.perf_counter() - started
    log = output / "correctness" / f"{lane.name}.run-{iteration}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(content, encoding="utf-8")
    summary = parse_ctest_summary(content)
    residue = source_root_residue(root, policy)
    status = (
        "passed"
        if returncode == 0 and not timed_out and summary["failed"] == 0 and not residue
        else "failed"
    )
    return {
        "iteration": iteration,
        "status": status,
        "returncode": returncode,
        "timed_out": timed_out,
        "duration_seconds": round(duration, 6),
        "ctest": summary,
        "source_root_residue": residue,
        "log": log.relative_to(output).as_posix(),
        "log_sha256": sha256(log),
        "command": command_text(command, root, output),
    }


def run_correctness(root: Path, build: Path, output: Path, repeat: int, scope: str,
                    policy: dict[str, Any]) -> list[dict[str, Any]]:
    grouped = []
    for lane in correctness_lanes(policy, scope):
        runs = repeat if lane.repeat else 1
        results = []
        for iteration in range(1, runs + 1):
            print(f"[{lane.name}] run {iteration}/{runs}", flush=True)
            result = execute_lane(root, build, output, lane, iteration, policy)
            results.append(result)
            print(f"[{lane.name}] {result['status']} in {result['duration_seconds']:.3f}s", flush=True)
        grouped.append({
            "name": lane.name,
            "status": "passed" if all(row["status"] == "passed" for row in results) else "failed",
            "repeat_policy": "three-or-more" if lane.repeat else "single-heavy-gate",
            "timeout_seconds": lane.timeout_seconds,
            "runs": results,
            "duration": distribution([float(row["duration_seconds"]) for row in results], "seconds"),
        })
    return grouped


def process_rss_bytes(pid: int) -> int:
    if os.name == "nt":
        class ProcessMemoryCounters(ctypes.Structure):
            _fields_ = [
                ("cb", ctypes.c_ulong),
                ("page_fault_count", ctypes.c_ulong),
                ("peak_working_set_size", ctypes.c_size_t),
                ("working_set_size", ctypes.c_size_t),
                ("quota_peak_paged_pool_usage", ctypes.c_size_t),
                ("quota_paged_pool_usage", ctypes.c_size_t),
                ("quota_peak_non_paged_pool_usage", ctypes.c_size_t),
                ("quota_non_paged_pool_usage", ctypes.c_size_t),
                ("pagefile_usage", ctypes.c_size_t),
                ("peak_pagefile_usage", ctypes.c_size_t),
            ]

        handle = ctypes.windll.kernel32.OpenProcess(0x1000 | 0x0010, False, pid)
        if not handle:
            return 0
        try:
            counters = ProcessMemoryCounters()
            counters.cb = ctypes.sizeof(counters)
            if ctypes.windll.psapi.GetProcessMemoryInfo(
                handle, ctypes.byref(counters), counters.cb
            ):
                return int(counters.working_set_size)
        finally:
            ctypes.windll.kernel32.CloseHandle(handle)
        return 0
    status = Path(f"/proc/{pid}/status")
    if status.is_file():
        try:
            match = re.search(r"(?m)^VmRSS:\s+(\d+)\s+kB$", status.read_text(encoding="ascii"))
            return int(match.group(1)) * 1024 if match else 0
        except (OSError, ValueError):
            return 0
    try:
        result = subprocess.run(
            ["ps", "-o", "rss=", "-p", str(pid)], text=True, encoding="ascii",
            errors="strict", stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=2,
            check=False,
        )
        return int(result.stdout.strip()) * 1024 if result.returncode == 0 else 0
    except (OSError, ValueError, subprocess.SubprocessError):
        return 0


def execute_process_sample(command: list[str], cwd: Path, output: Path, log_stem: str,
                           timeout_seconds: int, interval_seconds: float,
                           expected_stdout: bytes, env: dict[str, str]) -> dict[str, Any]:
    stdout_log = output / f"{log_stem}.stdout.log"
    stderr_log = output / f"{log_stem}.stderr.log"
    stdout_log.parent.mkdir(parents=True, exist_ok=True)
    started_ns = time.perf_counter_ns()
    started = time.perf_counter()
    peak_rss = 0
    timed_out = False
    with stdout_log.open("wb") as stdout_stream, stderr_log.open("wb") as stderr_stream:
        process = subprocess.Popen(
            command,
            cwd=cwd,
            stdout=stdout_stream,
            stderr=stderr_stream,
            env=env,
        )
        observed_affinity: list[int] | str = "unavailable"
        if os.name == "nt":
            child_handle = getattr(process, "_handle", None)
            if child_handle:
                observed_affinity = affinity_from_windows_handle(child_handle)
        elif hasattr(os, "sched_getaffinity"):
            try:
                observed_affinity = sorted(os.sched_getaffinity(process.pid))
            except OSError:
                observed_affinity = "unavailable"
        if isinstance(observed_affinity, list):
            affinity = observed_affinity
            affinity_evidence = "observed-child"
        else:
            affinity = process_affinity()
            affinity_evidence = "inherited-parent"
        deadline = started + timeout_seconds
        while True:
            peak_rss = max(peak_rss, process_rss_bytes(process.pid))
            returncode = process.poll()
            if returncode is not None:
                break
            if time.perf_counter() >= deadline:
                timed_out = True
                process.kill()
                returncode = process.wait()
                break
            time.sleep(interval_seconds)
    duration_ns = time.perf_counter_ns() - started_ns
    stdout = stdout_log.read_bytes()
    stderr = stderr_log.read_bytes()
    output_matches = stdout.replace(b"\r\n", b"\n") == expected_stdout.replace(
        b"\r\n", b"\n"
    )
    status = (
        "passed"
        if returncode == 0 and not timed_out and output_matches and peak_rss > 0
        else "failed"
    )
    return {
        "status": status,
        "returncode": returncode,
        "timed_out": timed_out,
        "stdout_matches": output_matches,
        "duration_ns": duration_ns,
        "peak_rss_bytes": peak_rss,
        "process_affinity": affinity,
        "process_affinity_evidence": affinity_evidence,
        "stdout_log": stdout_log.name,
        "stdout_sha256": sha256_bytes(stdout),
        "stderr_log": stderr_log.name,
        "stderr_sha256": sha256_bytes(stderr),
    }


def fixture_digest(directory: Path, policy: dict[str, Any],
                   replacements: dict[str, bytes] | None = None) -> str:
    digest = hashlib.sha256()
    for row in policy["performance"]["fixture"]["files"]:
        relative = row["path"]
        if replacements is not None and relative in replacements:
            data = replacements[relative]
        else:
            data = (directory / relative).read_bytes()
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(len(data).to_bytes(8, "big"))
        digest.update(data)
    return digest.hexdigest()


def expected_fixture_digest(root: Path, policy: dict[str, Any], variant: str) -> str:
    fixture = policy["performance"]["fixture"]
    directory = (root / fixture["directory"]).resolve()
    if variant == "base":
        return fixture_digest(directory, policy)
    if variant == "edited":
        return fixture_digest(directory, policy, {
            fixture["edit_file"]: (directory / fixture["edited_source"]).read_bytes(),
        })
    raise RuntimeError(f"unknown fixture variant {variant!r}")


def governed_inputs(root: Path, policy: dict[str, Any], policy_path: Path) -> dict[str, str]:
    return {
        "baseline_manifest_sha256": sha256(policy_path),
        "runner_sha256": sha256(Path(__file__).resolve()),
        "fixture_sha256": expected_fixture_digest(root, policy, "base"),
    }


def controlled_environment(directory: Path) -> dict[str, str]:
    temp = directory / "tmp"
    cache = directory / "cache"
    home = directory / "home"
    appdata = home / "AppData" / "Roaming"
    localappdata = home / "AppData" / "Local"
    temp.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    appdata.mkdir(parents=True, exist_ok=True)
    localappdata.mkdir(parents=True, exist_ok=True)
    return {
        **os.environ,
        "NO_COLOR": "1",
        "TMP": str(temp),
        "TEMP": str(temp),
        "TMPDIR": str(temp),
        "XDG_CACHE_HOME": str(cache / "xdg"),
        "XRAY_CACHE_DIR": str(cache / "xray"),
        "HOME": str(home),
        "USERPROFILE": str(home),
        "APPDATA": str(appdata),
        "LOCALAPPDATA": str(localappdata),
    }


def copy_fixture(source: Path, parent: Path, prefix: str) -> Path:
    work = Path(tempfile.mkdtemp(prefix=prefix, dir=parent))
    shutil.copytree(source, work, dirs_exist_ok=True)
    return work


def run_performance(root: Path, binary: Path, output: Path,
                    policy: dict[str, Any]) -> list[dict[str, Any]]:
    performance = policy["performance"]
    fixture_spec = performance["fixture"]
    fixture_source = (root / fixture_spec["directory"]).resolve()
    sample_count = performance["sample_count"]
    max_cv = float(performance["max_coefficient_of_variation"])
    timeout = performance["timeout_seconds"]
    interval = float(performance["rss_sample_interval_seconds"])
    grouped = []
    for lane in performance["lanes"]:
        lane_output = output / "performance" / lane["name"]
        lane_output.mkdir(parents=True, exist_ok=True)
        persistent = None if lane["mode"] == "cold" else copy_fixture(
            fixture_source, lane_output, "work."
        )
        warmups = []
        if persistent:
            for index in range(1, lane["warmups"] + 1):
                command = [str(binary), "run", fixture_spec["entry"]]
                warmups.append(execute_process_sample(
                    command, persistent, lane_output, f"warmup-{index}", timeout, interval,
                    fixture_spec["base_stdout"].encode("utf-8"),
                    controlled_environment(persistent),
                ))
        samples = []
        for index in range(1, sample_count + 1):
            work = persistent or copy_fixture(fixture_source, lane_output, f"sample-{index}.")
            expected = fixture_spec["base_stdout"].encode("utf-8")
            variant = "base"
            if lane["mode"] == "edit":
                destination = work / fixture_spec["edit_file"]
                replacement = destination.with_name(destination.name + ".next")
                replacement.write_bytes((fixture_source / fixture_spec["edit_file"]).read_bytes())
                os.replace(replacement, destination)
                pre_command = [str(binary), "run", fixture_spec["entry"]]
                precondition = execute_process_sample(
                    pre_command, work, lane_output, f"pre-edit-{index}", timeout, interval,
                    fixture_spec["base_stdout"].encode("utf-8"), controlled_environment(work),
                )
                precondition["kind"] = "per-sample-base-warmup"
                warmups.append(precondition)
                replacement.write_bytes((work / fixture_spec["edited_source"]).read_bytes())
                os.replace(replacement, destination)
                expected = fixture_spec["edited_stdout"].encode("utf-8")
                variant = "edited"
            command = [str(binary), "run", fixture_spec["entry"]]
            row = execute_process_sample(
                command, work, lane_output, f"sample-{index}", timeout, interval, expected,
                controlled_environment(work),
            )
            row.update({
                "iteration": index,
                "variant": variant,
                "fixture_sha256": fixture_digest(work, policy),
                "command": command_text(command, root, output),
            })
            samples.append(row)
            print(
                f"[{lane['name']}] sample {index}/{sample_count}: {row['status']} "
                f"{row['duration_ns'] / 1_000_000_000:.3f}s rss={row['peak_rss_bytes']}",
                flush=True,
            )
        duration = distribution([float(row["duration_ns"]) for row in samples], "nanoseconds")
        rss = distribution([float(row["peak_rss_bytes"]) for row in samples], "bytes")
        variance_passed = duration["coefficient_of_variation"] <= max_cv
        status = (
            "passed"
            if samples and all(row["status"] == "passed" for row in warmups + samples)
            and variance_passed
            else "failed"
        )
        grouped.append({
            "name": lane["name"],
            "mode": lane["mode"],
            "status": status,
            "warmups": warmups,
            "samples": samples,
            "duration": duration,
            "peak_rss": rss,
            "variance": {
                "maximum_coefficient_of_variation": max_cv,
                "status": "passed" if variance_passed else "failed",
            },
        })
    return grouped


def aggregate_result(correctness: list[dict[str, Any]], performance: list[dict[str, Any]],
                     errors: list[str] | None = None) -> str:
    lanes = [*correctness, *performance]
    if errors or not lanes or any(row.get("status") != "passed" for row in lanes):
        return "failed"
    return "passed"


def render_manifest(root: Path, build: Path, output: Path, repeat: int, scope: str,
                    policy: dict[str, Any],
                    policy_path: Path) -> dict[str, Any]:
    source_snapshot = clean_source_snapshot(root, policy)
    preflight = run_freshness_preflight(root, build, output, policy)
    errors = [
        f"freshness preflight {row['name']} failed" for row in preflight
        if row["status"] != "passed"
    ]
    run_performance_lanes = scope in ("full", "performance")
    identity: dict[str, Any] = source_snapshot
    environment: dict[str, Any] = {}
    correctness: list[dict[str, Any]] = []
    performance: list[dict[str, Any]] = []
    if not errors:
        identity = compiler_identity(root, build, policy)
        environment = host_and_toolchain_info(
            root, build, compiler_binary_path(build), run_performance_lanes, policy
        )
        correctness = [] if scope == "performance" else run_correctness(
            root, build, output, repeat, scope, policy
        )
        if run_performance_lanes:
            with fixed_affinity(policy["performance"]["logical_cpu"]):
                performance = run_performance(
                    root, compiler_binary_path(build), output, policy
                )
    residue = source_root_residue(root, policy)
    if residue:
        errors.append("source-root temporary residue: " + ", ".join(residue))
    final_source: dict[str, Any]
    try:
        final_identity = compiler_identity(root, build, policy)
        if final_identity != identity:
            errors.append("source or compiler identity changed during baseline capture")
            final_source = {"status": "failed", "identity": final_identity}
        else:
            final_source = {"status": "passed", "identity": final_identity}
    except (OSError, RuntimeError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        errors.append(f"final source identity failed: {error}")
        final_source = {"status": "failed", "error": str(error)}
    manifest = {
        "schema": EVIDENCE_SCHEMA,
        "runner": RUNNER_VERSION,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "scope": scope,
        "policy": {
            "path": policy_path.relative_to(root).as_posix()
            if policy_path.is_relative_to(root) else str(policy_path),
            "sha256": sha256(policy_path),
        },
        "governed_inputs": governed_inputs(root, policy, policy_path),
        "source": identity,
        "final_source": final_source,
        "environment": environment,
        "freshness_preflight": preflight,
        "measurement_policy": {
            "correctness_repeat_count": repeat,
            "performance_sample_count": policy["performance"]["sample_count"],
            "percentile_method": "linear interpolation at (n - 1) * quantile",
            "known_failure_allowlist": False,
            "failed_to_skip_reclassification": False,
            "logs_retained_out_of_tree": True,
        },
        "correctness_lanes": correctness,
        "performance_lanes": performance,
        "source_root_residue": residue,
        "errors": errors,
    }
    manifest["result"] = aggregate_result(correctness, performance, errors)
    return manifest


def valid_sha256(value: Any) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def derived_process_status(row: dict[str, Any]) -> str:
    return "passed" if (
        row.get("returncode") == 0
        and row.get("timed_out") is False
        and row.get("stdout_matches") is True
        and isinstance(row.get("duration_ns"), int)
        and row.get("duration_ns", 0) > 0
        and isinstance(row.get("peak_rss_bytes"), int)
        and row.get("peak_rss_bytes", 0) > 0
    ) else "failed"


def validate_evidence(manifest: dict[str, Any], policy: dict[str, Any], policy_path: Path,
                      require_qualifying: bool = False) -> list[str]:
    errors: list[str] = []
    strict = manifest.get("result") == "passed" or require_qualifying
    if manifest.get("schema") != EVIDENCE_SCHEMA:
        errors.append(f"evidence schema must be {EVIDENCE_SCHEMA}")
    if manifest.get("runner") != RUNNER_VERSION:
        errors.append(f"evidence runner must be {RUNNER_VERSION}")
    if manifest.get("policy", {}).get("sha256") != sha256(policy_path):
        errors.append("evidence policy digest does not match")
    root = policy_path.parents[2].resolve()
    expected_inputs = governed_inputs(root, policy, policy_path)
    actual_inputs = manifest.get("governed_inputs")
    if not isinstance(actual_inputs, dict):
        errors.append("evidence governed input hashes are missing")
    else:
        for key, expected in expected_inputs.items():
            if actual_inputs.get(key) != expected:
                errors.append(f"evidence {key} does not match governed inputs")

    source = manifest.get("source")
    if strict and not isinstance(source, dict):
        errors.append("passed evidence lacks source identity")
        source = {}
    if isinstance(source, dict) and source:
        commit = source.get("git_commit")
        tree = source.get("git_tree")
        version = source.get("version", {})
        binary = source.get("binary", {})
        if not isinstance(commit, str) or re.fullmatch(r"[0-9a-f]{40}", commit) is None:
            errors.append("evidence source commit is invalid")
        if not isinstance(tree, str) or re.fullmatch(r"[0-9a-f]{40}", tree) is None:
            errors.append("evidence source tree is invalid")
        if source.get("git_dirty") is not False:
            errors.append("evidence source is not clean")
        if version.get("schema") != 1 or version.get("product") != "xray-lang":
            errors.append("evidence compiler version identity is invalid")
        if version.get("commit") != commit or version.get("dirty") is not False:
            errors.append("evidence compiler commit/dirty identity does not match source")
        if version.get("buildProfile") != policy["identity"]["build_type"]:
            errors.append("evidence compiler build profile does not match policy")
        if not isinstance(version.get("target"), str) or not version.get("target"):
            errors.append("evidence compiler target identity is missing")
        if not valid_sha256(binary.get("sha256")) or not isinstance(
            binary.get("size_bytes"), int
        ) or binary.get("size_bytes", 0) <= 0:
            errors.append("evidence binary identity is incomplete")

    final_source = manifest.get("final_source", {})
    if strict:
        if final_source.get("status") != "passed":
            errors.append("passed evidence lacks a clean final source identity")
        elif final_source.get("identity") != source:
            errors.append("final source identity differs from initial identity")

    preflight = manifest.get("freshness_preflight", [])
    if strict and [row.get("name") for row in preflight] != ["configure", "build"]:
        errors.append("passed evidence lacks configure/build freshness preflight")
    for row in preflight:
        expected_status = "passed" if (
            row.get("returncode") == 0
            and row.get("timed_out") is False
            and not row.get("source_root_residue")
        ) else "failed"
        if row.get("status") != expected_status:
            errors.append(f"freshness preflight {row.get('name')} status is not derived")

    environment = manifest.get("environment")
    if strict and not isinstance(environment, dict):
        errors.append("passed evidence lacks environment identity")
        environment = {}
    if isinstance(environment, dict) and environment:
        build = environment.get("build", {})
        if build.get("generator") != policy["identity"]["generator"]:
            errors.append("evidence build generator does not match policy")
        if build.get("build_type") != policy["identity"]["build_type"]:
            errors.append("evidence build type does not match policy")
        expected_root = policy_path.parents[2].resolve()
        if not build.get("cmake_home") or Path(build["cmake_home"]).resolve() != expected_root:
            errors.append("evidence CMake source identity does not match policy root")
        for key in ("c_compiler", "linker"):
            tool = build.get(key, {})
            if not valid_sha256(tool.get("sha256")) or not isinstance(
                tool.get("size_bytes"), int
            ) or tool.get("size_bytes", 0) <= 0:
                errors.append(f"evidence {key} file identity is incomplete")
        tools = environment.get("tools", {})
        if not valid_sha256(tools.get("python", {}).get("sha256")):
            errors.append("evidence Python identity is incomplete")
        for key in ("cmake", "ninja"):
            if not isinstance(tools.get(key), str) or tools.get(key) in ("", "unavailable"):
                errors.append(f"evidence {key} version identity is incomplete")
        probe = environment.get("native_toolchain_probe")
        if not isinstance(probe, dict) or probe.get("schema") != 1:
            errors.append("evidence native toolchain probe is missing or invalid")
        else:
            selection = probe.get("selection", {})
            for key in ("provider", "version", "targetAbi", "runtimeArtifact"):
                if not isinstance(selection.get(key), str) or not selection.get(key):
                    errors.append(f"evidence native toolchain selection lacks {key}")
            if selection.get("ready") is not True:
                errors.append("evidence native toolchain selection is not ready")
            canonical = json.dumps(probe, sort_keys=True, separators=(",", ":")).encode("utf-8")
            if environment.get("native_toolchain_probe_sha256") != sha256_bytes(canonical):
                errors.append("evidence native toolchain probe digest does not match")
        if not valid_sha256(environment.get("inherited_toolchain_environment_sha256")):
            errors.append("evidence inherited toolchain environment identity is incomplete")

    correctness = manifest.get("correctness_lanes", [])
    performance = manifest.get("performance_lanes", [])
    if not isinstance(correctness, list) or not isinstance(performance, list):
        errors.append("evidence lane collections must be lists")
        correctness = []
        performance = []
    repeat = manifest.get("measurement_policy", {}).get("correctness_repeat_count")
    if strict and (not isinstance(repeat, int) or repeat < policy["correctness"]["minimum_repeat"]):
        errors.append("evidence correctness repeat policy is invalid")
    correctness_policy = {row["name"]: row for row in policy["correctness"]["lanes"]}
    for lane in correctness:
        name = lane.get("name")
        spec = correctness_policy.get(name)
        runs = lane.get("runs", [])
        if spec is None:
            errors.append(f"unknown correctness lane {name!r}")
            continue
        expected_runs = repeat if spec["repeat"] else 1
        if not isinstance(expected_runs, int) or len(runs) != expected_runs:
            errors.append(f"correctness lane {name} run count drift")
        for row in runs:
            summary = row.get("ctest", {})
            expected_status = "passed" if (
                row.get("returncode") == 0
                and row.get("timed_out") is False
                and summary.get("percent_passed") == 100
                and summary.get("failed") == 0
                and isinstance(summary.get("total"), int)
                and summary.get("total", 0) > 0
                and not row.get("source_root_residue")
            ) else "failed"
            if row.get("status") != expected_status:
                errors.append(f"correctness lane {name} run status is not derived")
        expected_lane_status = "passed" if runs and all(
            row.get("status") == "passed" for row in runs
        ) else "failed"
        if lane.get("status") != expected_lane_status:
            errors.append(f"correctness lane {name} aggregate status is not derived")
        durations = [float(row.get("duration_seconds", 0)) for row in runs]
        if lane.get("duration") != distribution(durations, "seconds"):
            errors.append(f"correctness lane {name} duration statistics do not match raw runs")

    max_cv = float(policy["performance"]["max_coefficient_of_variation"])
    samples_required = policy["performance"]["sample_count"]
    logical_cpu = policy["performance"]["logical_cpu"]
    performance_policy = {row["name"]: row for row in policy["performance"]["lanes"]}
    for lane in performance:
        name = lane.get("name")
        spec = performance_policy.get(name)
        if spec is None or lane.get("mode") != spec.get("mode"):
            errors.append(f"unknown or mismatched performance lane {name!r}")
            continue
        samples = lane.get("samples", [])
        warmups = lane.get("warmups", [])
        expected_warmups = spec["warmups"] + (samples_required if spec["mode"] == "edit" else 0)
        if len(samples) != samples_required:
            errors.append(f"performance lane {name} has {len(samples)} samples")
        if len(warmups) != expected_warmups:
            errors.append(f"performance lane {name} warmup count drift")
        for row in [*warmups, *samples]:
            if row.get("status") != derived_process_status(row):
                errors.append(f"performance lane {name} process status is not derived")
            if row.get("process_affinity") != [logical_cpu]:
                errors.append(f"performance lane {name} was not pinned to logical CPU {logical_cpu}")
            if row.get("process_affinity_evidence") not in (
                "observed-child", "inherited-parent"
            ):
                errors.append(f"performance lane {name} affinity evidence is missing")
        variants = {row.get("variant") for row in samples}
        expected_variant = "edited" if spec["mode"] == "edit" else "base"
        if variants != {expected_variant}:
            errors.append(f"performance lane {name} sample variant drift")
        for row in samples:
            try:
                expected_fixture = expected_fixture_digest(root, policy, row.get("variant"))
            except (OSError, RuntimeError):
                errors.append(f"performance lane {name} fixture variant is invalid")
                continue
            if row.get("fixture_sha256") != expected_fixture:
                errors.append(f"performance lane {name} fixture digest does not match raw fixture")
        durations = [float(row.get("duration_ns", 0)) for row in samples]
        rss_values = [float(row.get("peak_rss_bytes", 0)) for row in samples]
        duration = distribution(durations, "nanoseconds")
        rss = distribution(rss_values, "bytes")
        if lane.get("duration") != duration:
            errors.append(f"performance lane {name} duration statistics do not match raw samples")
        if lane.get("peak_rss") != rss:
            errors.append(f"performance lane {name} RSS statistics do not match raw samples")
        variance_status = (
            "passed" if duration["coefficient_of_variation"] <= max_cv else "failed"
        )
        if lane.get("variance") != {
            "maximum_coefficient_of_variation": max_cv,
            "status": variance_status,
        }:
            errors.append(f"performance lane {name} variance status is not derived")
        expected_lane_status = "passed" if (
            samples
            and all(row.get("status") == "passed" for row in [*warmups, *samples])
            and variance_status == "passed"
        ) else "failed"
        if lane.get("status") != expected_lane_status:
            errors.append(f"performance lane {name} aggregate status is not derived")

    lane_statuses = [row.get("status") for row in [*correctness, *performance]]
    if any(status not in ("passed", "failed") for status in lane_statuses):
        errors.append("evidence contains a status other than passed/failed")
    expected_result = aggregate_result(correctness, performance, manifest.get("errors", []))
    if manifest.get("result") != expected_result:
        errors.append(
            f"evidence aggregate result {manifest.get('result')!r} must be {expected_result!r}"
        )
    if manifest.get("source_root_residue"):
        errors.append("evidence records source-root temporary residue")

    scope = manifest.get("scope")
    if strict and scope in ("core", "full"):
        expected_correctness = {
            row["name"] for row in policy["correctness"]["lanes"] if scope in row["scopes"]
        }
        if {row.get("name") for row in correctness} != expected_correctness:
            errors.append(f"{scope} evidence does not contain every correctness lane")
    if strict and scope in ("full", "performance"):
        if {row.get("name") for row in performance} != set(performance_policy):
            errors.append(f"{scope} evidence does not contain every performance lane")
        host = environment.get("host", {}) if isinstance(environment, dict) else {}
        if host.get("measurement_affinity") != [logical_cpu]:
            errors.append("evidence measurement affinity does not match policy")
        power = host.get("power_policy", {})
        governed_power = policy["performance"]["power_policy"]
        expected_power = (
            governed_power.get("windows_active_scheme_guid")
            if os.name == "nt" else governed_power.get("linux_scaling_governor")
        )
        if (power.get("required") is not True or power.get("expected") != expected_power
                or power.get("active") != expected_power):
            errors.append("evidence active power policy is not verified")
        if not valid_sha256(power.get("query_sha256")):
            errors.append("evidence power-policy query digest is missing")
    if require_qualifying:
        if scope != policy["governance"]["qualifying_scope"]:
            errors.append("evidence scope is not qualifying")
        if manifest.get("result") != "passed":
            errors.append("failed evidence cannot qualify the baseline")
    return errors


def validate_log_digests(manifest: dict[str, Any], base: Path) -> list[str]:
    errors: list[str] = []

    def checked_path(parent: Path, value: Any) -> Path | None:
        if not isinstance(value, str) or not value:
            return None
        candidate = (parent / value).resolve()
        return candidate if candidate.is_relative_to(base.resolve()) else None

    for row in manifest.get("freshness_preflight", []):
        path = checked_path(base, row.get("log"))
        if path is None or not path.is_file() or sha256(path) != row.get("log_sha256"):
            errors.append(f"freshness preflight log digest mismatch: {path}")
    for lane in manifest.get("correctness_lanes", []):
        for row in lane.get("runs", []):
            path = checked_path(base, row.get("log"))
            if path is None or not path.is_file() or sha256(path) != row.get("log_sha256"):
                errors.append(f"correctness log digest mismatch: {path}")
    for lane in manifest.get("performance_lanes", []):
        lane_base = base / "performance" / lane.get("name", "")
        for row in [*lane.get("warmups", []), *lane.get("samples", [])]:
            for key, digest_key in (
                ("stdout_log", "stdout_sha256"), ("stderr_log", "stderr_sha256")
            ):
                path = checked_path(lane_base, row.get(key))
                if path is None or not path.is_file() or sha256(path) != row.get(digest_key):
                    errors.append(f"performance log digest mismatch: {path}")
    return errors


def verify_evidence(manifest: dict[str, Any], policy: dict[str, Any], policy_path: Path,
                    evidence_root: Path, require_qualifying: bool = False) -> list[str]:
    errors = validate_evidence(manifest, policy, policy_path, require_qualifying)
    errors.extend(validate_log_digests(manifest, evidence_root))
    return errors


def validate_policy_qualification(root: Path, policy: dict[str, Any],
                                  policy_path: Path) -> list[str]:
    qualification = policy["qualification"]
    if qualification["result"] == "failed":
        return []
    evidence_path = resolve_policy_path(root, qualification["evidence"])
    if not evidence_path.is_relative_to(root):
        return ["qualifying evidence path escapes the source root"]
    if not evidence_path.is_file():
        return [f"qualifying evidence is missing: {evidence_path}"]
    if sha256(evidence_path) != qualification["evidence_sha256"]:
        return ["qualifying evidence digest does not match policy"]
    try:
        manifest = json.loads(evidence_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return [f"cannot load qualifying evidence: {error}"]
    return verify_evidence(manifest, policy, policy_path, evidence_path.parent, True)


def failed_manifest(scope: str, policy_path: Path, error: Exception) -> dict[str, Any]:
    return {
        "schema": EVIDENCE_SCHEMA,
        "runner": RUNNER_VERSION,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "scope": scope,
        "policy": {
            "path": str(policy_path),
            "sha256": sha256(policy_path) if policy_path.is_file() else None,
        },
        "correctness_lanes": [],
        "performance_lanes": [],
        "source_root_residue": [],
        "errors": [str(error)],
        "result": "failed",
    }


def write_evidence(path: Path, manifest: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def self_check(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError("self-test failed: " + message)


def self_test(root: Path, policy_path: Path) -> int:
    policy = load_policy(root, policy_path)
    self_check(captured_text(b"timeout\xff") == "timeout�", "bytes decoding")
    parsed = parse_ctest_summary("100% tests passed, 0 tests failed out of 8")
    self_check(parsed == {"percent_passed": 100, "failed": 0, "total": 8}, "CTest parsing")
    self_check(percentile([1.0, 2.0, 3.0], 0.5) == 2.0, "p50")
    self_check(round(percentile([1.0, 2.0, 3.0], 0.95), 3) == 2.9, "p95")
    stats = distribution([1.0, 1.0], "seconds")
    self_check(stats["coefficient_of_variation"] == 0.0, "variance")
    expected_binary = "xray.exe" if os.name == "nt" else "xray"
    self_check(compiler_binary_path(Path("build")).name == expected_binary, "binary name")
    self_check(
        aggregate_result([{"status": "passed"}], [{"status": "failed"}]) == "failed",
        "result aggregation",
    )

    with tempfile.TemporaryDirectory(prefix="xray-target-baseline-self-test.") as directory:
        temp = Path(directory)
        (temp / ".gitignore").write_text("*.obj\n", encoding="utf-8")
        residue = temp / "leaked.fast-test.obj"
        residue.write_bytes(b"not-an-object")
        self_check(source_root_residue(temp, policy) == [residue.name], "residue ignores .gitignore")
        residue.unlink()

        sample_output = temp / "sample"
        sample_output.mkdir()
        command = [
            sys.executable,
            "-c",
            "import time; data=bytearray(1024*1024); print('ok'); time.sleep(0.05)",
        ]
        sample = execute_process_sample(
            command, temp, sample_output, "rss", 5, 0.005, b"ok\n",
            controlled_environment(temp),
        )
        self_check(sample["status"] == "passed", "RSS sample")
        self_check(sample["peak_rss_bytes"] > 0, "RSS capture")
        self_check(sample["process_affinity"] == process_affinity(), "sample affinity")
        self_check(
            sample["process_affinity_evidence"] in ("observed-child", "inherited-parent"),
            "sample affinity provenance",
        )
        environment = controlled_environment(temp)
        self_check(environment["HOME"] == environment["USERPROFILE"], "controlled home")
        self_check(Path(environment["XRAY_CACHE_DIR"]).is_relative_to(temp), "controlled cache")

        false_pass = {
            "schema": EVIDENCE_SCHEMA,
            "runner": RUNNER_VERSION,
            "policy": {"sha256": sha256(policy_path)},
            "result": "passed",
            "errors": [],
            "source_root_residue": [],
            "correctness_lanes": [{"name": "seeded-red", "status": "failed"}],
            "performance_lanes": [],
        }
        evidence_errors = validate_evidence(false_pass, policy, policy_path)
        self_check(any("aggregate result" in value for value in evidence_errors), "false pass result")
        self_check(any("source identity" in value for value in evidence_errors), "false pass identity")
        self_check(
            any("governed input hashes" in value for value in evidence_errors),
            "false pass governed inputs",
        )
        expected_inputs = governed_inputs(root, policy, policy_path)
        for key in expected_inputs:
            hash_tampered = {**false_pass, "governed_inputs": dict(expected_inputs)}
            hash_tampered["governed_inputs"][key] = "0" * 64
            hash_errors = validate_evidence(hash_tampered, policy, policy_path)
            self_check(
                any(key in value for value in hash_errors),
                f"governed {key} mismatch",
            )

        tampered_stats = {
            "schema": EVIDENCE_SCHEMA,
            "runner": RUNNER_VERSION,
            "policy": {"sha256": sha256(policy_path)},
            "scope": "performance",
            "result": "failed",
            "errors": ["seeded failure"],
            "source_root_residue": [],
            "correctness_lanes": [],
            "performance_lanes": [{
                "name": policy["performance"]["lanes"][0]["name"],
                "mode": "cold",
                "status": "failed",
                "warmups": [],
                "samples": [],
                "duration": distribution([1.0], "nanoseconds"),
                "peak_rss": distribution([1.0], "bytes"),
                "variance": {
                    "maximum_coefficient_of_variation": policy["performance"][
                        "max_coefficient_of_variation"
                    ],
                    "status": "passed",
                },
            }],
        }
        tampered_errors = validate_evidence(tampered_stats, policy, policy_path)
        self_check(any("duration statistics" in value for value in tampered_errors), "duration recompute")
        self_check(any("RSS statistics" in value for value in tampered_errors), "RSS recompute")

        missing_log = {
            "freshness_preflight": [{"log": "missing.log", "log_sha256": "0" * 64}],
            "correctness_lanes": [],
            "performance_lanes": [],
        }
        log_errors = validate_log_digests(missing_log, temp)
        self_check(bool(log_errors), "missing logs fail closed")

        malformed_policy = json.loads(json.dumps(policy))
        malformed_policy["performance"]["fixture"]["files"][0]["sha256"] = "0" * 64
        self_check(bool(validate_policy(root, malformed_policy)), "fixture inventory drift")
    print("target-machine baseline runner self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--output-dir", default="build/target-machine/phase0")
    parser.add_argument("--policy", default=str(DEFAULT_POLICY))
    parser.add_argument("--manifest")
    parser.add_argument("--repeat", type=int)
    parser.add_argument("--scope", choices=("core", "full", "performance"), default="core")
    parser.add_argument("--verify-policy", action="store_true")
    parser.add_argument("--verify-manifest")
    parser.add_argument("--require-qualifying", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    policy_path = resolve_policy_path(root, args.policy)
    if args.self_test:
        return self_test(root, policy_path)
    try:
        policy = load_policy(root, policy_path)
    except RuntimeError as error:
        print(f"target-machine baseline policy: FAILED: {error}", file=sys.stderr)
        return 1
    if args.verify_policy:
        qualification_errors = validate_policy_qualification(root, policy, policy_path)
        if qualification_errors:
            print("target-machine baseline policy: FAILED", file=sys.stderr)
            for error in qualification_errors:
                print(f"  - {error}", file=sys.stderr)
            return 1
        print(f"target-machine baseline policy: PASS ({policy_path})")
        return 0
    if args.verify_manifest:
        evidence_path = Path(args.verify_manifest).resolve()
        try:
            manifest = json.loads(evidence_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            print(f"target-machine baseline evidence: FAILED: {error}", file=sys.stderr)
            return 1
        errors = verify_evidence(
            manifest, policy, policy_path, evidence_path.parent, args.require_qualifying
        )
        if errors:
            print("target-machine baseline evidence: FAILED", file=sys.stderr)
            for error in errors:
                print(f"  - {error}", file=sys.stderr)
            return 1
        print(f"target-machine baseline evidence: PASS ({evidence_path})")
        return 0

    repeat = args.repeat if args.repeat is not None else policy["correctness"]["minimum_repeat"]
    if repeat < policy["correctness"]["minimum_repeat"]:
        parser.error(
            f"--repeat must be at least {policy['correctness']['minimum_repeat']}"
        )
    build_value = Path(args.build_dir)
    build = build_value.resolve() if build_value.is_absolute() else (root / build_value).resolve()
    output_value = Path(args.output_dir)
    output = output_value.resolve() if output_value.is_absolute() else (root / output_value).resolve()
    output.mkdir(parents=True, exist_ok=True)
    evidence_path = output / "baseline-evidence.json"
    try:
        manifest = render_manifest(root, build, output, repeat, args.scope, policy, policy_path)
        validation_errors = verify_evidence(manifest, policy, policy_path, output)
        if validation_errors:
            manifest["errors"].extend(validation_errors)
            manifest["result"] = "failed"
    except (OSError, RuntimeError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        manifest = failed_manifest(args.scope, policy_path, error)
    write_evidence(evidence_path, manifest)
    if args.manifest:
        destination_value = Path(args.manifest)
        destination = (
            destination_value.resolve()
            if destination_value.is_absolute()
            else (root / destination_value).resolve()
        )
        if destination == policy_path:
            print("target-machine baseline: FAILED: evidence cannot overwrite policy", file=sys.stderr)
            return 1
        write_evidence(destination, manifest)
    print(f"target-machine baseline: {manifest['result'].upper()} ({evidence_path})")
    return 0 if manifest["result"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
