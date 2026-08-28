#!/usr/bin/env python3
"""Differential runner: the same .xr through several execution forms, byte for byte.

This is the strongest correctness asset in the repo. A divergence between two
forms of the same program is a bug in one of them, so the net gates every case
except a written baseline of known divergences.

Three forms are available, selected with XRAY_DIFF_BACKENDS:

  vm     `xray run` -- compile in-process, execute on the VM.
  aot    `xray build --native` -- the Xi IR native pipeline, then run the binary.
  embed  `xray build` (no --native) -- the default build: bytecode is serialized
         through the constant pool into a C byte array, linked against the
         runtime, and deserialized by the VM inside the produced binary.

`embed` shares the VM with `vm` and differs from it only by a constant-pool
serialize/deserialize round trip, so a vm/embed divergence localizes to that
round trip or to the embedded entry path. That form used to have no differential
gate at all, and two defects reached the tree through the gap: a dropped BigInt
literal tag, and host-heap pointers written into the container.

Observable contract = stdout + exit code, compared byte for byte (normalized
stderr only when a lane enables that channel). Backend build logs are not
program output and never enter the comparison. This is the contract frozen in
contracts/differential-protocol.md.

Infrastructure -- subprocess handling, cache keys, directory locks, parallelism,
the ratchet -- comes from the shared xraytest runtime, so this file holds only
what is specific to backend differencing: how a case is run on each backend, how
their outputs are compared, and the per-case sidecars (.args, .stdin,
.xr.expected, `// diff-backends:`, `// anchor:`) that shape a case.

Replaces run_backend_diff.sh and the earlier run_backend_diff_fast.py. There is
no shell runner: Python is a hard build requirement, so a second implementation
would be dead weight.
"""

from __future__ import annotations

import os
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path, PurePath
from typing import Optional


def _bootstrap() -> None:
    """Put tests/lib on sys.path so `import xraytest` works without an install."""
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import cache, platform, proc, progress, ratchet, scheduler  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent
CASE_DIR = SCRIPT_DIR / "cases"

# Every form this runner knows, in the order a lane evaluates them. The first
# enabled one is the reference the others are compared against.
BACKEND_ORDER = ("vm", "aot", "embed")

# Forms that produce a standalone binary and therefore use the binary cache.
BINARY_BACKENDS = ("aot", "embed")

# A case directory's identity includes the .xr.expected oracle: a changed
# expected file must change the key, or a stale AOT binary would be diffed
# against the new oracle and pass by accident.
CASE_DIR_GLOBS = ("*.xr", "*.args", "*.xr.expected", "*.toml")

# stdout/stderr previews in a failure line: enough to see the divergence.
_PREVIEW_LINES = 6
_HEAD_LINES = 3


def is_uint(value: str) -> bool:
    return value.isdigit()


def env_int(name: str, default: int) -> int:
    raw = os.environ.get(name, str(default))
    return int(raw) if is_uint(raw) and int(raw) > 0 else default


def configure_jobs(requested: str) -> tuple[int, bool]:
    """Worker count and whether cache-state caps may still adjust it."""
    if requested in ("", "auto"):
        jobs = min(cache_cpu_count(), env_int("XRAY_DIFF_MAX_AUTO_JOBS", 16))
        return max(1, jobs), True
    if is_uint(requested) and int(requested) > 0:
        return int(requested), False
    return 1, False


def cap_auto_jobs_for_cache_state(jobs: int, auto_jobs: bool, cache_state: str) -> int:
    """Apply the registered cold or hot cap without overriding explicit jobs."""
    if not auto_jobs:
        return jobs
    cap_name = (
        "XRAY_DIFF_HOT_MAX_AUTO_JOBS"
        if cache_state == "hot"
        else "XRAY_DIFF_COLD_MAX_AUTO_JOBS"
    )
    return max(1, min(jobs, env_int(cap_name, jobs)))


def cache_cpu_count() -> int:
    return os.cpu_count() or 1


def canonical_path_text(path: PurePath) -> str:
    """Return the one path spelling accepted by ratchet manifests.

    Baselines are repository artifacts shared by Windows and POSIX runners, so
    native separators are not semantic.  Keep the conversion in one helper:
    result names, baseline membership, and diagnostics must all see the same
    forward-slash spelling.
    """
    return path.as_posix()


def rel_path(path: Path) -> str:
    try:
        return canonical_path_text(path.resolve().relative_to(PROJECT_DIR))
    except ValueError:
        return canonical_path_text(path)


def safe_name(rel: str) -> str:
    stem = rel[:-3] if rel.endswith(".xr") else rel
    return "".join(ch if ch.isalnum() or ch in "_.-" else "_" for ch in stem)


def case_cache_name(rel: str, case_key: str) -> str:
    """Readable, bounded cache component for one differential case."""
    import hashlib

    stem = safe_name(Path(rel).stem)[:32] or "case"
    digest = hashlib.sha256(f"{rel}\0{case_key}".encode()).hexdigest()[:24]
    return f"{stem}-{digest}"


# --- sidecar readers --------------------------------------------------------


def read_first_directive(path: Path, prefix: str, max_lines: int) -> str:
    try:
        with path.open("r", encoding="utf-8") as fh:
            for idx, line in enumerate(fh, start=1):
                if idx > max_lines:
                    break
                if line.startswith(prefix):
                    return line[len(prefix):].strip()
    except OSError:
        pass
    return ""


def read_args(path: Path) -> list[str]:
    argfile = path.with_suffix(".args")
    if not argfile.is_file():
        return []
    try:
        first = argfile.read_text(encoding="utf-8").splitlines()[0]
    except (OSError, IndexError):
        return []
    return first.split()


def read_stdin(path: Path) -> bytes:
    """Bytes fed to both backends' stdin. Absent sidecar means empty, closed
    stdin so a stdin-reading case gets EOF instead of hanging on the harness's."""
    sidecar = path.with_suffix(".stdin")
    if not sidecar.is_file():
        return b""
    try:
        return sidecar.read_bytes()
    except OSError:
        return b""


def read_expected_stdout(path: Path) -> bytes | None:
    """Optional exact stdout oracle stored as <case>.xr.expected."""
    sidecar = Path(str(path) + ".expected")
    if not sidecar.is_file():
        return None
    try:
        return sidecar.read_bytes()
    except OSError:
        return None


def head_text(data: bytes, lines: int = _HEAD_LINES) -> str:
    return "|".join(data.decode("utf-8", "replace").splitlines()[:lines])


def decode_build_log(data: bytes) -> str:
    """Preserve invalid diagnostic bytes without emitting unencodable U+FFFD."""
    return data.decode("utf-8", "backslashreplace")


def console_safe_text(text: str, encoding: str | None = None) -> str:
    """Escape code points unavailable in the active console encoding."""
    codec = encoding or sys.stdout.encoding or "utf-8"
    return text.encode(codec, "backslashreplace").decode(codec, "strict")


# --- backend execution ------------------------------------------------------


@dataclass
class BackendResult:
    rc: int
    stdout: bytes
    stderr: bytes
    buildlog: bytes = b""


@dataclass
class CaseResult:
    order: int
    # "pass" | "fail" | "refused" | "skip"
    #
    # "refused" is a backend that would not build the case at all. It is not
    # "fail": this net asks whether two backends agree about the language, and a
    # case only one of them can run produced no answer to disagree about.
    # Counting a build refusal as a divergence is what let 494 unfinished
    # authorities bury the handful of real disagreements this net exists to
    # catch. Refusals ratchet separately, in not_comparable.txt.
    status: str
    output: str
    name: str = ""
    # Full native build evidence is exposed only for a refusal.  Consumers such
    # as the live refusal manifest must not scrape the console preview (which is
    # deliberately truncated to twenty lines) or launch a second build with a
    # subtly different differential policy.
    refusal_build_logs: dict[str, bytes] = field(default_factory=dict)


@dataclass
class RunnerConfig:
    xray: Path
    backends: list[str]
    jobs: int
    aot_opt: str
    aot_cache: Path
    aot_bin_cache: Path
    # The embedded-bytecode form gets its own binary cache: the two forms build
    # the same case into different binaries, so one cache would answer for both.
    embed_bin_cache: Path
    diff_stderr: bool
    # Optimizer policy handed to both lanes, or "" for each pipeline's default.
    #
    # The two lanes do not run the same passes by default: the VM pipeline is
    # pinned to the light level and the native backend to the full level, so an
    # ordinary run of this net compares two differently optimized programs and a
    # divergence cannot say which side is wrong. Setting XRAY_DIFF_XI_OPT puts
    # both lanes under one named policy, which is the only way this net asserts
    # that the two sides were configured alike -- the same string reaches
    # `xray run` and `xray build`, and a spec either applies to both or is
    # refused by both, because the compiler rejects a malformed one.
    xi_opt: str
    # Per-subprocess wall-clock ceiling. The shell net had none, so one
    # deadlocked case hung the whole lane until the outer 900s ctest timeout.
    # A generous default turns that into a single red case; the whole child
    # tree is killed on expiry (proc.run uses killpg on POSIX). Tunable via
    # XRAY_TEST_CASE_TIMEOUT; 0 disables (the historical behavior).
    case_timeout: float | None


def binary_cache_dir(config: RunnerConfig, kind: str) -> Path:
    return config.aot_bin_cache if kind == "aot" else config.embed_bin_cache


def build_case_binary(
    config: RunnerConfig, kind: str, case: Path, rel: str, case_key: str
) -> tuple[Path | None, bytes]:
    """Build (or reuse) one case's binary for a binary-producing form, under a lock.

    The lock is the shared DirLock: exactly one racer builds while the rest wait,
    then everyone reuses the finished binary. The tmp-then-rename keeps a partial
    build from ever being seen as a cache hit.

    `aot` and `embed` differ only in the build command and the cache root: the
    surrounding locking, reuse, and log capture are identical, so they share this
    body rather than drifting apart in two copies.
    """
    bin_dir = binary_cache_dir(config, kind) / case_cache_name(rel, case_key)
    binary = bin_dir / kind
    if binary.is_file() and os.access(binary, os.X_OK):
        return binary, f"cached: {binary}\n".encode()

    import threading

    tmp = bin_dir / f"{kind}.{os.getpid()}.{threading.get_ident()}"
    lock = cache.DirLock(bin_dir.with_name(bin_dir.name + ".lock"))
    bin_dir.mkdir(parents=True, exist_ok=True)
    if not lock.acquire():
        return None, f"cannot lock binary cache: {lock}\n".encode()
    try:
        if binary.is_file() and os.access(binary, os.X_OK):
            return binary, f"cached: {binary}\n".encode()
        try:
            tmp.unlink()
        except OSError:
            pass
        env = os.environ.copy()
        env.setdefault("XRAY_AOT_FAST_TEST_BUILD", "1")
        if kind == "aot":
            cmd = [
                config.xray, "build", "--native", "-O", config.aot_opt,
                "--cache-dir", config.aot_cache, case, "-o", tmp,
            ]
        else:
            # The default build. -O reaches only the host C compile of the
            # generated shim, so it is held at the lane's level for build speed;
            # what is under test is the serialized bytecode, which -O cannot
            # reach. No --cache-dir: the bytecode path has no object cache.
            cmd = [config.xray, "build", "-O", config.aot_opt, case, "-o", tmp]
        if config.xi_opt:
            cmd[2:2] = ["--xi-opt", config.xi_opt]
        result = proc.run(cmd, env=env, timeout=config.case_timeout)
        # build stdout+stderr merged into one log, matching the shell runner's
        # STDOUT=STDERR capture used for failure excerpts.
        buildlog = result.stdout + result.stderr
        if result.timed_out:
            buildlog += f"\n[timed out after {config.case_timeout}s]\n".encode()
        if not result.ok:
            try:
                tmp.unlink()
            except OSError:
                pass
            return None, buildlog
        tmp.replace(binary)
        return binary, buildlog
    finally:
        lock.release()


def run_backend(
    config: RunnerConfig, kind: str, case: Path, args: list[str], stdin_bytes: bytes
) -> BackendResult:
    if kind == "vm":
        cmd = [config.xray, "run", case]
        if config.xi_opt:
            cmd[2:2] = ["--xi-opt", config.xi_opt]
        if args:
            cmd.extend(["--", *args])
        r = proc.run(cmd, stdin=stdin_bytes, timeout=config.case_timeout)
        # A timed-out backend gets a sentinel exit so the diff reports a
        # mismatch instead of comparing truncated output as if it were real.
        rc = 124 if r.timed_out else r.returncode
        return BackendResult(rc, r.stdout, r.stderr)

    if kind in BINARY_BACKENDS:
        rel = rel_path(case)
        key = cache.dir_key(case.parent, CASE_DIR_GLOBS)
        binary, buildlog = build_case_binary(config, kind, case, rel, key)
        if binary is None:
            return BackendResult(200, b"BUILDFAIL\n", b"", buildlog)
        r = proc.run([binary, *args], stdin=stdin_bytes, timeout=config.case_timeout)
        rc = 124 if r.timed_out else r.returncode
        return BackendResult(rc, r.stdout, r.stderr, buildlog)

    return BackendResult(201, b"BADBACKEND\n", f"unknown backend: {kind}".encode())


def run_case(config: RunnerConfig, order: int, case: Path) -> CaseResult:
    name = rel_path(case)
    anchor = read_first_directive(case, "// anchor: ", 1)
    case_backends_raw = read_first_directive(case, "// diff-backends: ", 5)
    aot_reject = read_first_directive(case, "// diff-aot-reject: ", 5)
    case_backends = [b.strip() for b in case_backends_raw.split(",") if b.strip()]

    enabled: list[str] = []
    excluded: list[str] = []
    for backend in BACKEND_ORDER:
        if backend not in config.backends:
            continue
        if case_backends and backend not in case_backends:
            excluded.append(backend)
            continue
        enabled.append(backend)

    # A name past the column width must still be separated from its verdict:
    # left-justify padding adds nothing once the field overflows, which ran the
    # longest case paths straight into the word "FAIL" with no space between.
    prefix = f"  {name:<84} "
    if len(enabled) < 2:
        # The rejection contract is an assertion about the native backend, so it
        # is only owed by a lane that runs it. A vm/embed lane must not re-assert
        # it: the AOT lane already does, and doing it here would spend a native
        # build per case on a lane that has no native side.
        if enabled == ["vm"] and aot_reject and "aot" in config.backends:
            args = read_args(case)
            stdin_bytes = read_stdin(case)
            vm = run_backend(config, "vm", case, args, stdin_bytes)
            expected_stdout = read_expected_stdout(case)
            if vm.rc != 0 or (expected_stdout is not None and vm.stdout != expected_stdout):
                return CaseResult(
                    order, "fail",
                    prefix + "FAIL (VM half of rejection contract)\n"
                    f"      vm: rc={vm.rc} stdout: {head_text(vm.stdout)}",
                    name,
                )
            with tempfile.TemporaryDirectory(prefix="xray-diff-reject-") as temp_dir:
                out_c = Path(temp_dir) / "rejected.c"
                rejected = proc.run(
                    [config.xray, "build", "--native", "-c", "-o", out_c, case],
                    timeout=config.case_timeout,
                )
            diagnostic = rejected.stdout + rejected.stderr
            if rejected.timed_out or rejected.returncode == 0 or aot_reject.encode() not in diagnostic:
                return CaseResult(
                    order, "fail",
                    prefix + "FAIL (AOT rejection contract)\n"
                    f"      aot: rc={124 if rejected.timed_out else rejected.returncode} "
                    f"diagnostic: {head_text(diagnostic)}",
                    name,
                )
            return CaseResult(order, "pass", prefix + "PASS (VM run + AOT rejection)", name)
        return CaseResult(
            order, "skip",
            prefix + f"SKIP (need >=2 backends; case={case_backends_raw or 'all'} "
            f"global={','.join(config.backends)})", name,
        )

    args = read_args(case)
    stdin_bytes = read_stdin(case)
    results: dict[str, BackendResult] = {}
    for backend in enabled:
        results[backend] = run_backend(config, backend, case, args, stdin_bytes)

    ref = enabled[0]
    ref_result = results[ref]

    # A backend that refused to build answers nothing about agreement. Report
    # which one refused and let the coverage ratchet own it.
    refusers = [b for b in enabled if results[b].rc == 200]
    if refusers:
        lines = [prefix + f"REFUSED ({', '.join(refusers)} did not build)"]
        for backend in refusers:
            log = decode_build_log(results[backend].buildlog)
            lines.extend("      " + line for line in log.splitlines()[:20])
        return CaseResult(
            order,
            "refused",
            "\n".join(lines),
            name,
            refusal_build_logs={backend: results[backend].buildlog for backend in refusers},
        )

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

    expected_stdout = read_expected_stdout(case)
    if not mismatch and expected_stdout is not None:
        if ref_result.rc != 0:
            mismatch = f"exit code ({ref}={ref_result.rc} expected=0)"
        elif ref_result.stdout != expected_stdout:
            mismatch = f"stdout ({ref} vs expected)"
            other = "expected"

    if not mismatch:
        suffix = f"PASS (excl:{' '.join(excluded)})" if excluded else "PASS"
        return CaseResult(order, "pass", prefix + suffix, name)

    lines = [prefix + f"FAIL ({mismatch})" + (f"  [anchor: {anchor}]" if anchor else "")]
    for backend in enabled:
        res = results[backend]
        lines.append(f"      {backend}: rc={res.rc}  stdout: {head_text(res.stdout)}")
        if res.rc == 200 and res.buildlog:
            lines.extend(
                "      " + line
                for line in decode_build_log(res.buildlog).splitlines()[:20]
            )
    if other == "expected":
        lines.append(f"      {ref}: {head_text(ref_result.stdout, _PREVIEW_LINES)}")
        lines.append(f"      expected: {head_text(expected_stdout or b'', _PREVIEW_LINES)}")
    elif other:
        lhs = ref_result.stdout if mismatch.startswith("stdout") else ref_result.stderr
        rhs = results[other].stdout if mismatch.startswith("stdout") else results[other].stderr
        lines.append(f"      {ref}: {head_text(lhs, _PREVIEW_LINES)}")
        lines.append(f"      {other}: {head_text(rhs, _PREVIEW_LINES)}")
    return CaseResult(order, "fail", "\n".join(lines), name)


# --- case collection --------------------------------------------------------


def append_case_manifest(manifest: str) -> Optional[list[Path]]:
    if not manifest:
        return []
    manifest_path = Path(manifest)
    if not manifest_path.is_absolute():
        manifest_path = PROJECT_DIR / manifest
    if not manifest_path.is_file():
        return None
    cases: list[Path] = []
    for raw in manifest_path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        path = Path(line)
        if not path.is_absolute():
            path = PROJECT_DIR / line
        cases.append(path)
    return cases


def collect_cases(base_cases_file: str, extra_cases_file: str) -> list[Path]:
    if base_cases_file:
        base = append_case_manifest(base_cases_file)
        if base is None:
            raise FileNotFoundError(f"base case manifest not found: {base_cases_file}")
        cases = sorted(base)
    else:
        # cases/liveness/ is run_liveness_diff.py's, which enforces a wall-clock
        # budget and honors .live sidecars. This net has no per-case timeout and
        # compares terminating output, so a by-design non-terminating liveness
        # case would hang it. An explicit base manifest still wins if it names them.
        liveness_dir = CASE_DIR / "liveness"
        cases = sorted(p for p in CASE_DIR.rglob("*.xr") if liveness_dir not in p.parents)

    if extra_cases_file:
        extra = append_case_manifest(extra_cases_file)
        if extra:
            cases.extend(extra)
    return cases


def binary_cache_hot(config: RunnerConfig, selected: list[tuple[int, Path]]) -> bool:
    """True when every binary this lane needs is already built and executable."""
    kinds = [k for k in BINARY_BACKENDS if k in config.backends]
    if not kinds:
        return True
    for _order, case in selected:
        case_backends_raw = read_first_directive(case, "// diff-backends: ", 5)
        case_backends = [b.strip() for b in case_backends_raw.split(",") if b.strip()]
        rel = rel_path(case)
        key = cache.dir_key(case.parent, CASE_DIR_GLOBS)
        for kind in kinds:
            if case_backends and kind not in case_backends:
                continue
            binary = binary_cache_dir(config, kind) / case_cache_name(rel, key) / kind
            if not (binary.is_file() and os.access(binary, os.X_OK)):
                return False
    return True


def validate_shard(total: str, index: str) -> tuple[int, int]:
    if not is_uint(total) or not is_uint(index):
        raise ValueError(f"shard config must be numeric: total={total} index={index}")
    total_i, index_i = int(total), int(index)
    if total_i < 1:
        raise ValueError("XRAY_DIFF_SHARD_TOTAL must be >= 1")
    if index_i >= total_i:
        raise ValueError(
            f"XRAY_DIFF_SHARD_INDEX must be in [0,total): index={index_i} total={total_i}"
        )
    return total_i, index_i


# --- driver -----------------------------------------------------------------


def diff_stable_cache_dir(suite: str, xray: Path) -> Path:
    return cache.stable_cache_dir(PROJECT_DIR, suite, xray)


def main(argv: list[str]) -> int:
    xray_raw = argv[1] if len(argv) > 1 else os.environ.get("XRAY_BIN", "")
    if not xray_raw:
        build_dir = os.environ.get("XRAY_BUILD_DIR")
        xray_raw = str(Path(build_dir) / "xray") if build_dir else str(PROJECT_DIR / "build" / "xray")
    xray = Path(xray_raw)

    backends = [b.strip() for b in os.environ.get("XRAY_DIFF_BACKENDS", "vm,aot").split(",") if b.strip()]
    unknown = [b for b in backends if b not in BACKEND_ORDER]
    if unknown:
        print(f"error: unknown backend(s) in XRAY_DIFF_BACKENDS: {','.join(unknown)}", file=sys.stderr)
        print(f"known backends: {','.join(BACKEND_ORDER)}", file=sys.stderr)
        return 2
    requested_jobs = os.environ.get("XRAY_DIFF_JOBS", os.environ.get("XRAY_TEST_JOBS", "auto"))
    jobs, auto_jobs = configure_jobs(requested_jobs)
    aot_opt = os.environ.get("XRAY_AOT_TEST_OPT", "0")
    xi_opt = os.environ.get("XRAY_DIFF_XI_OPT", "").strip()
    # The policy names which passes ran, so it belongs in the cache path: a
    # binary built under one policy must never be reused to answer for another.
    cache_tag = f"O{aot_opt}" if not xi_opt else f"O{aot_opt}-{safe_name(xi_opt)}"
    aot_cache = Path(
        os.environ.get("XRAY_DIFF_CACHE_DIR", str(diff_stable_cache_dir("aot-objects", xray) / cache_tag))
    )
    aot_bin_cache = Path(
        os.environ.get("XRAY_DIFF_BIN_CACHE_DIR", str(diff_stable_cache_dir("backend-diff-bin", xray) / cache_tag))
    )
    embed_bin_cache = Path(
        os.environ.get(
            "XRAY_DIFF_EMBED_BIN_CACHE_DIR",
            str(diff_stable_cache_dir("backend-diff-embed-bin", xray) / cache_tag),
        )
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

    # 180s per subprocess: generous enough for a cold AOT build of the heaviest
    # case, tight enough that a deadlock becomes one red case, not a hung lane.
    # XRAY_TEST_CASE_TIMEOUT=0 restores the historical no-timeout behavior.
    case_timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 180)

    config = RunnerConfig(
        xray=xray, backends=backends, jobs=jobs, aot_opt=aot_opt,
        aot_cache=aot_cache, aot_bin_cache=aot_bin_cache, embed_bin_cache=embed_bin_cache,
        diff_stderr=diff_stderr, case_timeout=case_timeout, xi_opt=xi_opt,
    )

    if single_case:
        result = run_case(config, int(single_id_raw) if is_uint(single_id_raw) else 0, Path(single_case))
        print(result.output)
        # A refusal is not a divergence, but debugging one case still wants a
        # nonzero exit: nothing was compared.
        return 0 if result.status in ("pass", "skip") else 1

    if not (xray.is_file() and os.access(xray, os.X_OK)):
        import shutil

        if shutil.which(str(xray)) is None:
            print(f"FAIL: xray binary not executable: {xray_raw}")
            print("=== Results: 0 passed, 1 failed, 0 skipped ===")
            return 1
    if not CASE_DIR.is_dir():
        print(f"FAIL: governed backend-diff case directory is missing: {CASE_DIR}")
        print("=== Results: 0 passed, 1 failed, 0 skipped ===")
        return 1

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
    if case_index == 0:
        print("FAIL: backend-diff discovered no runnable cases")
        print("=== Results: 0 passed, 1 failed, 0 skipped ===")
        return 1
    if not selected:
        print(
            "FAIL: selected backend-diff shard has no runnable cases: "
            f"index={shard_index} total={shard_total} discovered={case_index}"
        )
        print("=== Results: 0 passed, 1 failed, 0 skipped ===")
        return 1

    cache_state = "hot" if binary_cache_hot(config, selected) else "cold"
    jobs = cap_auto_jobs_for_cache_state(jobs, auto_jobs, cache_state)
    config.jobs = jobs

    print(f"=== Backend Differential ({' / '.join(b.upper() for b in backends)}) ===")
    print(f"Binary:   {xray_raw}")
    print(f"Backends: {','.join(backends)}")
    print(f"Jobs:     {jobs}")
    print(f"CacheState: {cache_state}")
    print(f"AOT opt:  -O{aot_opt}")
    # -O reaches only the host C compiler. Which Xi passes ran is the line
    # below. The built-in default is one shared light pre-plan set for both lanes.
    print(f"Xi opt:   {xi_opt if xi_opt else 'shared light pre-plan default'}")
    print(f"Cache:    {aot_cache}")
    print(f"BinCache: {aot_bin_cache}")
    if "embed" in backends:
        print(f"EmbedBinCache: {embed_bin_cache}")
    print("RunCache: disabled")
    if shard_total > 1:
        print(f"Shard:    {shard_index} / {shard_total}")
    print("")
    if shard_total > 1:
        print(f"Cases:    {len(selected)} / {case_index}")
        print("")

    # Case-level parallelism. Each case mixes an AOT build and both backends'
    # runs, so it is one CPU-tagged unit; the scheduler caps concurrency at the
    # configured job count.
    results: list[CaseResult] = []
    reporter = progress.ProgressReporter(len(selected))
    if jobs <= 1:
        for order, case in selected:
            result = run_case(config, order, case)
            results.append(result)
            reporter.tick(result.name)
    else:
        sched = scheduler.Scheduler({scheduler.CPU: jobs})
        tasks = [
            scheduler.Task(
                key=str(order),
                fn=(lambda o=order, c=case: run_case(config, o, c)),
                tag=scheduler.CPU,
            )
            for order, case in selected
        ]
        by_key = sched.run(
            tasks,
            on_done=lambda k, r: reporter.tick(getattr(r, "name", "")),
        )
        for value in by_key.values():
            # run_case is written not to raise; a raised exception is a runner
            # bug, so surface it rather than mis-counting it as a case verdict.
            if isinstance(value, BaseException):
                raise value
            results.append(value)
    reporter.finish()

    passed = failed = refused = skipped = 0
    for result in sorted(results, key=lambda item: item.order):
        print(console_safe_text(result.output))
        if result.status == "pass":
            passed += 1
        elif result.status == "skip":
            skipped += 1
        elif result.status == "refused":
            refused += 1
        else:
            failed += 1

    print("")
    print(
        f"=== Results: {passed} passed, {failed} failed, "
        f"{refused} refused, {skipped} skipped ==="
    )

    # Ratchet. New-failure detection is always valid. The stale-entry check is
    # only valid on a full run: a case-subset run never executes most baselined
    # cases, so their absence from the failure set proves nothing.
    baseline_path = Path(os.environ.get("XRAY_DIFF_BASELINE", str(SCRIPT_DIR / "known_failures.txt")))
    baseline = ratchet.read_baseline(baseline_path)
    failed_names = {r.name for r in results if r.status == "fail" and r.name}
    refused_names = {r.name for r in results if r.status == "refused" and r.name}
    # A refusal produced no agreement verdict, so to the divergence ratchet it
    # reads exactly like a skip: it can neither be a new divergence nor prove a
    # baselined one is fixed.
    skipped_names = {r.name for r in results if r.status == "skip" and r.name} | refused_names

    full_run = not os.environ.get("XRAY_DIFF_CASES_FILE") and shard_total <= 1
    verdict = ratchet.evaluate(
        failed=failed_names,
        baseline=baseline,
        # On a partial run, suppress the now-passing check by treating every
        # unseen baseline entry as skipped -- it did not run, so it cannot be
        # declared fixed.
        skipped=skipped_names if full_run else (baseline - failed_names),
    )

    status = 0
    if verdict.new_failures:
        print("")
        print(f"=== New differential failures (not in {rel_path(baseline_path)}) ===")
        for name in verdict.new_failures:
            print(f"  {name}")
        print(f"A divergence between {' / '.join(backends)} is a correctness bug in one of them.")
        print("Fix it, or -- only with a written reason -- add it to the baseline.")
        status = 1

    if full_run:
        if verdict.now_passing:
            print("")
            print("=== Baselined cases now pass; delete these entries ===")
            for name in verdict.now_passing:
                print(f"  {name}")
            print(f"The baseline may only shrink. Remove the lines above from {rel_path(baseline_path)}.")
            status = 1
        print(f"Ratchet: {len(failed_names)} diverging, {len(baseline)} baselined.")

    # Coverage ratchet. Same policy, different question: which cases can be
    # compared at all. A case that stops building is a regression even though it
    # states nothing about agreement, and one that starts building must leave
    # the list so it cannot quietly stop again.
    # Derived from the divergence baseline rather than configured separately, so
    # every lane gets its own coverage list automatically: the embedded lane
    # compares a different pair of backends and therefore has its own set of
    # cases it can build at all.
    refused_path = baseline_path.with_name(baseline_path.stem + "_not_comparable.txt")
    refused_baseline = ratchet.read_baseline(refused_path)
    refused_verdict = ratchet.evaluate(
        failed=refused_names,
        baseline=refused_baseline,
        skipped={r.name for r in results if r.status == "skip" and r.name}
        if full_run
        else (refused_baseline - refused_names),
    )
    if refused_verdict.new_failures:
        print("")
        print(f"=== Cases that stopped building (not in {rel_path(refused_path)}) ===")
        for name in refused_verdict.new_failures:
            print(f"  {name}")
        print("A case that built before and does not now is a backend regression.")
        status = 1
    if full_run:
        if refused_verdict.now_passing:
            print("")
            print("=== Listed cases now build; delete these entries ===")
            for name in refused_verdict.now_passing:
                print(f"  {name}")
            print(f"The list may only shrink. Remove the lines above from {rel_path(refused_path)}.")
            status = 1
        print(
            f"Coverage: {len(refused_names)} not comparable, "
            f"{len(refused_baseline)} listed."
        )

    return status


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
