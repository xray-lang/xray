#!/usr/bin/env python3
"""AOT filetest runner: assert XAOT plan/dump and generated-C shape per case.

Each case is a .xr with a .expect file in the directive language of
filetest_expect. The runner builds the case with --dump-xaot-plan and captures
the generated C, then checks the .expect assertions and, when asked, that the C
compiles. A .toml turns the case into a NativePackagePlan project; a
.contract.toml runs `xray verify` instead of a build.

Infrastructure -- subprocess, workspaces, cache keys and locks, the ratchet,
parallelism, toolchain probes -- comes from the shared xraytest runtime. This
file holds the filetest-specific flow. It replaces run_aot_filetests.sh; there
is no shell runner, because Python is a hard build requirement.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from dataclasses import dataclass, field
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))
    here = Path(__file__).resolve().parent
    if str(here) not in sys.path:
        sys.path.insert(0, str(here))


_bootstrap()
from xraytest import cache, platform, proc, progress, ratchet, report, scheduler, toolchain, workspace  # noqa: E402
import filetest_expect as expectlib  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent
FILETEST_DIR = SCRIPT_DIR / "filetests"
ALL_MODES = ["rep", "layout", "abi", "boundary", "container", "link", "cgen"]
BASELINE_DEFAULT = SCRIPT_DIR / "filetests_known_failures.txt"

# Public ABI headers the runtime headers include by bare name (xrt_value.h ->
# "xray_value_abi.h") live in include/. Omitting it fails the C syntax check on
# nearly every case; the gap hid behind a warm cache until a source edit
# invalidated the keys.
SYNTAX_INCLUDE_DIRS = ("src/aot", "src", "include")

Status = report.Status


@dataclass
class Config:
    xray: Path
    mode: str
    selected_modes: list[str]
    verbose: bool
    keep_tmp: bool
    jobs: int
    cache_dir: Path
    sanitizer: bool
    disable_run_cache: bool
    baseline: Path
    # Per-subprocess wall-clock ceiling. A dump/verify/compile that deadlocks
    # becomes one red case, not a hung lane; the child tree is killed on expiry.
    # XRAY_TEST_CASE_TIMEOUT tunes it, 0 disables.
    case_timeout: float | None
    # Every case in a mode shares one fixture directory. Hash that directory
    # once before scheduling instead of once per parallel case: the latter is
    # quadratic in the corpus size and can exhaust the outer CTest budget.
    case_directory_keys: dict[Path, str] = field(default_factory=dict)


# --- helpers ----------------------------------------------------------------


def rel_path(path: Path) -> str:
    try:
        return str(Path(path).resolve().relative_to(PROJECT_DIR))
    except ValueError:
        return str(path)


def safe_case_name(rel: str) -> str:
    stem = rel[:-3] if rel.endswith(".xr") else rel
    return "".join(ch if ch.isalnum() or ch in "_.-" else "_" for ch in stem)


def is_collected(path: Path) -> bool:
    name = path.name
    return name.endswith(".xr") and not name.startswith("_")


def configure_jobs(requested: str) -> int:
    if requested in ("", "auto"):
        cap = os.environ.get("XRAY_AOT_FILETEST_MAX_AUTO_JOBS", "4")
        cap_n = int(cap) if cap.isdigit() and int(cap) > 0 else 4
        return max(1, min(platform.cpu_count(), cap_n))
    return int(requested) if requested.isdigit() and int(requested) > 0 else 1


# --- commands ---------------------------------------------------------------


def probe_dump_support(config: Config, ws: workspace.Workspace) -> tuple[bool, str]:
    """Does this xray support --dump-xaot-plan? A trivial build decides; failure
    skips the whole suite rather than reporting every case as a failure."""
    src = ws.write("probe.xr", "fn answer() -> int {\n    return 42\n}\nprint(answer())\n")
    out_c = ws.path("probe.c")
    result = proc.run(
        [config.xray, "build", "--native", "--dump-xaot-plan", "--dump-link-manifest",
         "-c", "-o", out_c, src],
        timeout=config.case_timeout,
    )
    return result.ok, result.combined_text()


def run_dump(config: Config, out_c: Path, xr_file: Path, extra_args: list[str]) -> proc.ProcResult:
    argv = [config.xray, "build", "--native", "--dump-xaot-plan", "--dump-link-manifest"]
    argv.extend(extra_args)
    argv.extend(["-c", "-o", out_c, xr_file])
    return proc.run(argv, timeout=config.case_timeout)


def run_verify(config: Config, project: Path, contract: Path) -> proc.ProcResult:
    return proc.run(
        [config.xray, "verify", "--contract", contract.name],
        cwd=project,
        timeout=config.case_timeout,
    )


def compile_c_syntax(
    config: Config, c_out: Path, extra_args: list[str], ws: workspace.Workspace, tag: str
) -> expectlib.CheckOutcome:
    """Check the generated C compiles. Cross-target C needs a toolchain shipping
    that target's headers (zig); host C uses -fsyntax-only."""
    triple = expectlib.target_triple(" ".join(extra_args))
    include_flags = [f"-I{PROJECT_DIR / d}" for d in SYNTAX_INCLUDE_DIRS]

    if triple:
        zig = os.environ.get("ZIG", "zig")
        if shutil.which(zig) is None:
            return expectlib.CheckOutcome(
                f"c_syntax needs a cross toolchain for {triple}; set ZIG", skip=True
            )
        obj = ws.path(f"{tag}.syntax.o")
        argv = [zig, "cc", "-target", triple, "-c", "-o", obj, *include_flags, c_out]
    else:
        cc = toolchain.find_c_compiler()
        if cc is None:
            return expectlib.CheckOutcome("C compiler not found for c_syntax")
        obj = ws.path(f"{tag}.syntax.o")
        argv = cc.syntax_check_argv(c_out, [PROJECT_DIR / d for d in SYNTAX_INCLUDE_DIRS], obj)

    result = proc.run(argv, timeout=config.case_timeout)
    if result.timed_out:
        return expectlib.CheckOutcome(f"generated C syntax check timed out after {config.case_timeout}s")
    if not result.ok:
        return expectlib.CheckOutcome("generated C syntax check failed")
    return expectlib.CheckOutcome()


# --- one case ---------------------------------------------------------------


def skip_for_sanitizer(config: Config, exp: expectlib.Expect) -> bool:
    """Sanitizer builds drop the freestanding -nostdlib/-ffreestanding link
    shape, so a case positively asserting those flags cannot hold there."""
    if not config.sanitizer:
        return False
    import re

    for d in exp.directives:
        if d.key in ("contains", "regex") and re.search(r"-nostdlib|-ffreestanding", d.value):
            return True
    return False


def assemble_project(mode: str, xr_file: Path, manifest: Path, contract: Path | None,
                     ws: workspace.Workspace) -> Path:
    """A .toml case builds as a project: copy the directory's .xr and .S assets,
    the manifest as xray.toml, and any contract, then build from there."""
    base = xr_file.stem
    project = ws.subdir(f"projects/{mode}_{base}")
    src_dir = xr_file.parent
    for asset in sorted(src_dir.glob("*.xr")):
        shutil.copy2(asset, project / asset.name)
    for asset in sorted(src_dir.glob("*.S")):
        shutil.copy2(asset, project / asset.name)
    shutil.copy2(manifest, project / "xray.toml")
    if contract and contract.is_file():
        shutil.copy2(contract, project / contract.name)
    return project / xr_file.name


@dataclass
class CaseVerdict:
    name: str
    mode: str
    status: str  # Status value
    detail: str = ""
    excerpt: str = ""


def _dump_and_c_paths(config: Config, mode: str, xr_file: Path, extra_args: list[str],
                      ws: workspace.Workspace) -> tuple[bytes, bytes, str | None]:
    """Return (dump_bytes, c_bytes, error). Uses the persistent dump/C cache
    unless disabled. error is a failure detail when the dump command itself
    failed on a status=pass case."""
    base = xr_file.stem
    build_source = xr_file
    manifest = xr_file.with_suffix(".toml")
    contract = xr_file.with_name(xr_file.stem + ".contract.toml")
    if manifest.is_file():
        build_source = assemble_project(mode, xr_file, manifest, contract, ws)

    if config.disable_run_cache:
        out_c = ws.path(f"{mode}_{base}.c")
        result = run_dump(config, out_c, build_source, extra_args)
        if result.timed_out:
            return result.stdout + result.stderr, b"", f"dump command timed out after {config.case_timeout}s"
        if not result.ok:
            return result.stdout + result.stderr, b"", "dump command failed"
        c_bytes = out_c.read_bytes() if out_c.is_file() else b""
        return result.stdout + result.stderr, c_bytes, None

    case_key = config.case_directory_keys.get(xr_file.parent)
    if case_key is None:
        return b"", b"", f"cache identity missing for case directory: {rel_path(xr_file.parent)}"
    args_key = cache.mix([" ".join(extra_args)])
    safe = safe_case_name(rel_path(xr_file))
    cache_dir = config.cache_dir / mode / f"{safe}-{case_key}-{args_key}"
    cached_dump = cache_dir / "dump"
    cached_c = cache_dir / "generated.c"

    if cached_dump.is_file() and cached_c.is_file():
        return cached_dump.read_bytes(), cached_c.read_bytes(), None

    cache_dir.mkdir(parents=True, exist_ok=True)
    lock = cache.DirLock(cache_dir.with_name(cache_dir.name + ".lock"))
    if not lock.acquire():
        return b"", b"", "cannot lock cache dir"
    try:
        if cached_dump.is_file() and cached_c.is_file():
            return cached_dump.read_bytes(), cached_c.read_bytes(), None
        out_c = ws.path(f"{mode}_{base}.c")
        result = run_dump(config, out_c, build_source, extra_args)
        dump_bytes = result.stdout + result.stderr
        if result.timed_out:
            return dump_bytes, b"", f"dump command timed out after {config.case_timeout}s"
        if not result.ok:
            return dump_bytes, b"", "dump command failed"
        c_bytes = out_c.read_bytes() if out_c.is_file() else b""
        cached_dump.write_bytes(dump_bytes)
        cached_c.write_bytes(c_bytes)
        return dump_bytes, c_bytes, None
    finally:
        lock.release()


def run_one_case(config: Config, mode: str, xr_file: Path, ws: workspace.Workspace) -> CaseVerdict:
    name = rel_path(xr_file)
    base = xr_file.stem
    expect_path = xr_file.with_suffix(".expect")
    exp = expectlib.parse(expect_path)
    extra_args = exp.args.split()
    contract = xr_file.with_name(xr_file.stem + ".contract.toml")

    if not expect_path.is_file():
        return CaseVerdict(name, mode, Status.SKIP.value, "missing expect")

    if skip_for_sanitizer(config, exp):
        return CaseVerdict(name, mode, Status.SKIP.value,
                           "freestanding link flags incompatible with sanitizer runtime")

    triple = expectlib.target_triple(" ".join(extra_args))
    if triple and shutil.which(os.environ.get("ZIG", "zig")) is None:
        return CaseVerdict(name, mode, Status.SKIP.value,
                           f"no cross toolchain for {triple}; install zig or set ZIG")

    if exp.status not in ("pass", "fail"):
        return CaseVerdict(name, mode, Status.FAIL.value, f"unknown expected status: {exp.status}")

    # status=fail: the dump/verify must NOT succeed, then assertions still run.
    if exp.status == "fail":
        if contract.is_file():
            manifest = xr_file.with_suffix(".toml")
            project = assemble_project(mode, xr_file, manifest, contract, ws) if manifest.is_file() else xr_file.parent
            project_dir = project.parent if manifest.is_file() else xr_file.parent
            vres = run_verify(config, project_dir, contract)
            if vres.timed_out:
                return CaseVerdict(name, mode, Status.FAIL.value,
                                   f"contract verification timed out after {config.case_timeout}s",
                                   vres.combined_text())
            if vres.ok:
                return CaseVerdict(name, mode, Status.FAIL.value,
                                   "contract verification unexpectedly succeeded",
                                   vres.combined_text())
            outcome = expectlib.check(exp, vres.combined_text(), "")
            return _verdict_from_outcome(name, mode, outcome, vres.combined_text())

        out_c = ws.path(f"{mode}_{base}.c")
        result = run_dump(config, out_c, _build_source(mode, xr_file, ws), extra_args)
        if result.timed_out:
            return CaseVerdict(name, mode, Status.FAIL.value,
                               f"dump command timed out after {config.case_timeout}s",
                               result.combined_text())
        if result.ok:
            return CaseVerdict(name, mode, Status.FAIL.value, "dump command unexpectedly succeeded",
                               result.combined_text())
        dump_text = (result.stdout + result.stderr).decode("utf-8", "replace")
        c_text = ""
        if mode == "link" and out_c.is_file():
            c_text = expectlib.normalize_link_c(out_c.read_text(encoding="utf-8"))
        outcome = expectlib.check(exp, dump_text, c_text)
        return _verdict_from_outcome(name, mode, outcome, dump_text)

    # status=pass with a contract: verify must succeed.
    if contract.is_file():
        manifest = xr_file.with_suffix(".toml")
        if manifest.is_file():
            build_source = assemble_project(mode, xr_file, manifest, contract, ws)
            project_dir = build_source.parent
        else:
            project_dir = xr_file.parent
        vres = run_verify(config, project_dir, contract)
        if vres.timed_out:
            return CaseVerdict(name, mode, Status.FAIL.value,
                               f"contract verification timed out after {config.case_timeout}s",
                               vres.combined_text())
        if not vres.ok:
            return CaseVerdict(name, mode, Status.FAIL.value, "contract verification failed",
                               vres.combined_text())

    dump_bytes, c_bytes, err = _dump_and_c_paths(config, mode, xr_file, extra_args, ws)
    if err:
        return CaseVerdict(name, mode, Status.FAIL.value, err,
                           dump_bytes.decode("utf-8", "replace"))

    dump_text = dump_bytes.decode("utf-8", "replace")
    c_text = c_bytes.decode("utf-8", "replace")
    c_for_expect = expectlib.normalize_link_c(c_text) if mode == "link" else c_text

    # C syntax check happens against the un-normalized C, before the assertions.
    if exp.wants_c_syntax:
        c_out = ws.path(f"{mode}_{base}.syntax.c")
        c_out.write_bytes(c_bytes)
        syn = compile_c_syntax(config, c_out, extra_args, ws, f"{mode}_{base}")
        if syn.skip:
            return CaseVerdict(name, mode, Status.SKIP.value, syn.reason)
        if not syn.ok:
            return CaseVerdict(name, mode, Status.FAIL.value, syn.reason, c_text[:2000])

    outcome = expectlib.check(exp, dump_text, c_for_expect)
    return _verdict_from_outcome(name, mode, outcome, dump_text if not outcome.reason or "generated C" not in outcome.reason else c_for_expect)


def _build_source(mode: str, xr_file: Path, ws: workspace.Workspace) -> Path:
    manifest = xr_file.with_suffix(".toml")
    if manifest.is_file():
        contract = xr_file.with_name(xr_file.stem + ".contract.toml")
        return assemble_project(mode, xr_file, manifest, contract if contract.is_file() else None, ws)
    return xr_file


def _verdict_from_outcome(name: str, mode: str, outcome: expectlib.CheckOutcome, excerpt: str) -> CaseVerdict:
    if outcome.skip:
        return CaseVerdict(name, mode, Status.SKIP.value, outcome.reason or "")
    if not outcome.ok:
        return CaseVerdict(name, mode, Status.FAIL.value, outcome.reason or "", excerpt)
    return CaseVerdict(name, mode, Status.PASS.value)


# --- collection & driver ----------------------------------------------------


def collect(selected_modes: list[str]) -> list[tuple[str, Path]]:
    out: list[tuple[str, Path]] = []
    for mode in selected_modes:
        directory = FILETEST_DIR / mode
        if not directory.is_dir():
            continue
        for xr in sorted(directory.glob("*.xr")):
            if is_collected(xr):
                out.append((mode, xr))
    return out


def prepare_case_directory_keys(config: Config, cases: list[tuple[str, Path]]) -> None:
    """Freeze each fixture-directory identity once before parallel execution.

    Cache keys still cover the same complete directory contents. Precomputing
    also prevents a source-tree edit during a run from giving sibling cases
    different identities; one runner invocation measures one fixture snapshot.
    """
    config.case_directory_keys.clear()
    if config.disable_run_cache:
        return
    directories = sorted({xr_file.parent for _, xr_file in cases})
    config.case_directory_keys.update(
        (directory, cache.dir_key(directory)) for directory in directories
    )


def format_line(v: CaseVerdict) -> str:
    prefix = f"  {v.mode:<9} {v.name:<48}"
    if v.status == Status.PASS.value:
        return prefix + "PASS"
    if v.status == Status.SKIP.value:
        return prefix + f"SKIP ({v.detail})"
    return prefix + f"FAIL ({v.detail})"


def parse_args(argv: list[str]) -> Config:
    ap = argparse.ArgumentParser(description="AOT filetest runner")
    ap.add_argument("--mode", default="all")
    ap.add_argument("--xray", default=None)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--keep-tmp", action="store_true")
    ap.add_argument("xray_positional", nargs="?", default=None)
    ns = ap.parse_args(argv[1:])

    mode = ns.mode
    if mode == "all":
        selected = list(ALL_MODES)
    elif mode in ALL_MODES:
        selected = [mode]
    else:
        ap.error(f"unsupported mode '{mode}' (expected {'/'.join(ALL_MODES)}/all)")

    xray_raw = ns.xray or ns.xray_positional or os.environ.get("XRAY_BIN")
    if not xray_raw:
        build_dir = os.environ.get("XRAY_BUILD_DIR")
        xray_raw = str(Path(build_dir) / "xray") if build_dir else str(PROJECT_DIR / "build" / "xray")
    xray = Path(xray_raw)
    if not xray.is_absolute() and xray.exists():
        xray = xray.resolve()

    requested_jobs = os.environ.get("XRAY_AOT_FILETEST_JOBS", os.environ.get("XRAY_TEST_JOBS", "auto"))
    cache_override = os.environ.get("XRAY_AOT_FILETEST_CACHE_DIR")
    cache_dir = Path(cache_override) if cache_override else cache.stable_cache_dir(
        PROJECT_DIR, "aot-filetest-dumps", xray
    )

    return Config(
        xray=xray,
        mode=mode,
        selected_modes=selected,
        verbose=ns.verbose,
        keep_tmp=ns.keep_tmp,
        jobs=configure_jobs(requested_jobs),
        cache_dir=cache_dir,
        sanitizer=platform.env_flag("XRAY_AOT_FILETEST_SANITIZER"),
        disable_run_cache=platform.env_flag("XRAY_TEST_DISABLE_RUN_CACHE"),
        baseline=Path(os.environ.get("XRAY_AOT_FILETEST_BASELINE", str(BASELINE_DEFAULT))),
        # 120s per subprocess: a case builds with -c (no link), so this is
        # generous. XRAY_TEST_CASE_TIMEOUT tunes it, 0 disables.
        case_timeout=platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 120),
    )


def main(argv: list[str]) -> int:
    config = parse_args(argv)
    cases = collect(config.selected_modes)

    print("=== AOT Filetests (XAOT plan scaffold) ===")
    print(f"Binary: {config.xray}")
    print(f"Mode:   {config.mode}")
    print(f"Jobs:   {config.jobs}")
    print(f"Cache:  {config.cache_dir}")
    print("")

    if shutil.which(str(config.xray)) is None and not (config.xray.is_file() and os.access(config.xray, os.X_OK)):
        return _fail_before_measurement(
            f"xray binary not found or not executable: {config.xray}", len(cases)
        )

    with workspace.Workspace("xray_aot_filetests", keep=config.keep_tmp) as ws:
        ok, probe_log = probe_dump_support(config, ws)
        if not ok:
            reason = next((ln for ln in probe_log.splitlines() if ln.strip()), "")
            return _fail_before_measurement(
                "xray build --native --dump-xaot-plan probe failed",
                len(cases),
                reason=reason,
            )

        if not cases:
            return _fail_before_measurement(
                f"no AOT filetests found for mode '{config.mode}'", 0
            )

        prepare_case_directory_keys(config, cases)
        verdicts = _run_cases(config, cases, ws)

    return _report_and_ratchet(config, verdicts)


def _fail_before_measurement(message: str, case_count: int, *, reason: str = "") -> int:
    """Fail closed when the runner cannot produce even one case verdict.

    Capability and setup failures are not skips: treating them as such lets a
    completely unmeasured suite appear green to CTest.  Keep the ordinary
    result grammar, but count the infrastructure failure explicitly and report
    how many cases never ran.
    """
    print(f"ERROR: {message}")
    if reason:
        print(f"Reason: {reason}")
    print(f"Cases not run: {case_count}")
    print("=== Results: 0 passed, 1 failed, 0 skipped ===")
    return 1


def _run_cases(config: Config, cases: list[tuple[str, Path]], ws: workspace.Workspace) -> list[CaseVerdict]:
    reporter = progress.ProgressReporter(len(cases))
    if config.jobs <= 1:
        results = []
        for mode, xr in cases:
            v = run_one_case(config, mode, xr, ws)
            results.append(v)
            reporter.tick(v.name)
        reporter.finish()
        return results

    sched = scheduler.Scheduler({scheduler.CPU: config.jobs})
    tasks = [
        scheduler.Task(
            key=f"{i}",
            fn=(lambda m=mode, x=xr: run_one_case(config, m, x, ws)),
            tag=scheduler.CPU,
        )
        for i, (mode, xr) in enumerate(cases)
    ]
    by_key = sched.run(tasks, on_done=lambda k, r: reporter.tick(getattr(r, "name", "")))
    reporter.finish()
    results = []
    for i in range(len(cases)):
        value = by_key.get(str(i))
        if isinstance(value, BaseException):
            raise value
        results.append(value)
    return results


def _report_and_ratchet(config: Config, verdicts: list[CaseVerdict]) -> int:
    passed = failed = skipped = 0
    for v in verdicts:
        print(format_line(v))
        if config.verbose and v.excerpt and v.status == Status.FAIL.value:
            for line in v.excerpt.splitlines()[:40]:
                print(f"        {line}")
        if v.status == Status.PASS.value:
            passed += 1
        elif v.status == Status.SKIP.value:
            skipped += 1
        else:
            failed += 1

    print("")
    print(f"=== Results: {passed} passed, {failed} failed, {skipped} skipped ===")

    if passed + failed == 0:
        print("")
        print("ERROR: AOT filetests produced no measured verdicts; skips cannot qualify the suite.")
        return 1

    baseline = ratchet.read_baseline(config.baseline)
    failed_names = {v.name for v in verdicts if v.status == Status.FAIL.value}
    skipped_names = {v.name for v in verdicts if v.status == Status.SKIP.value}
    full_run = config.mode == "all"

    verdict = ratchet.evaluate(
        failed=failed_names,
        baseline=baseline,
        skipped=skipped_names if full_run else (baseline - failed_names),
    )

    status = 0
    if verdict.new_failures:
        print("")
        print(f"=== New filetest failures (not in {rel_path(config.baseline)}) ===")
        for name in verdict.new_failures:
            print(f"  {name}")
        print("Fix the case, or -- only with a written reason -- add it to the baseline.")
        status = 1

    if full_run:
        if verdict.now_passing:
            print("")
            print("=== Baselined filetests now pass; delete these entries ===")
            for name in verdict.now_passing:
                print(f"  {name}")
            print(f"The baseline may only shrink. Remove the lines above from {rel_path(config.baseline)}.")
            status = 1
        print("")
        print(f"Ratchet: {len(failed_names)} failing, {len(baseline)} baselined.")

    return status


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
