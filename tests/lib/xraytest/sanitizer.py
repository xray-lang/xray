"""Shared plumbing for the sanitizer lanes (ASan / UBSan / TSan / LSan).

All four lanes have the same skeleton -- configure a sanitizer build tree, build
it, run a focused surface, decide a verdict -- and each shell script had grown
its own copy of the setup, with the copies drifting. This module owns the parts
that must not drift; a lane declares only what is actually specific to it.

Two invariants are enforced here rather than left to each lane:

  - **A sanitizer lane must not certify a binary it did not sanitize.** The
    configured cache is checked for the expected ENABLE_* flag, so pointing a
    lane at an ordinary build directory fails loudly instead of reporting a
    clean result it never earned.

  - **A skipped build must not certify stale code.** When a lane reuses an
    existing binary, any source newer than that binary is a hard error: a green
    sanitizer result for code that was never built under the sanitizer is worse
    than no result.
"""

from __future__ import annotations

import os
import shutil
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

from . import platform, proc

# Trees whose contents decide whether a reused binary is stale.
# What a sanitizer binary is built from. `tests` belongs here: the lanes run
# ctest over the unit tests in that tree, so a binary built before a test source
# changed is exactly as stale as one built before a compiler source changed --
# and staleness there is invisible without it.
SOURCE_ROOTS = ("src", "include", "stdlib", "tests", "CMakeLists.txt")


def write_console(stream, text: str) -> None:
    """Write subprocess output without crashing on a narrow console codec.

    Windows may expose a GBK stdout even when test programs emit UTF-8 symbols.
    Preserve the diagnostic as an ASCII escape when the active stream encoding
    cannot represent a character; the sanitizer verdict must never be hidden by
    the reporting path itself.
    """
    encoding = getattr(stream, "encoding", None) or "utf-8"
    safe = text.encode(encoding, errors="backslashreplace").decode(encoding)
    stream.write(safe)


@dataclass
class BuildSpec:
    """How to configure one sanitizer build tree."""

    build_dir: Path
    # CMake cache variables that turn the sanitizer on, e.g. ENABLE_ASAN=ON.
    sanitizer_flags: tuple[str, ...]
    build_type: str = "Debug"
    c_compiler: str = "clang"
    cxx_compiler: str = "clang++"
    # Extra cache variables beyond the sanitizer flags.
    extra_cache: tuple[str, ...] = ()
    targets: tuple[str, ...] = ()          # empty = build everything
    # What proves this tree really is instrumented. Normally the sanitizer
    # flags themselves, but a lane that instruments through raw CMAKE_C_FLAGS
    # rather than an ENABLE_* option has no BOOL to check, so it names the
    # cache line to look for instead. Kept separate from sanitizer_flags
    # because "how to configure" and "how to verify" are not the same question:
    # conflating them either skips the check or invents an option that changes
    # what the build does.
    verify_cache_contains: tuple[str, ...] = ()

    def verification_targets(self) -> "tuple[str, ...]":
        return self.verify_cache_contains or self.sanitizer_flags


def cache_file(build_dir: Path) -> Path:
    return build_dir / "CMakeCache.txt"


def configured_generator(build_dir: Path) -> str | None:
    cache = cache_file(build_dir)
    if not cache.is_file():
        return None
    for line in cache.read_text(encoding="utf-8").splitlines():
        if line.startswith("CMAKE_GENERATOR:INTERNAL="):
            return line.split("=", 1)[1].strip()
    return None


def resolve_compiler_command(command: str) -> str:
    """Resolve a compiler driver without making a standard Windows LLVM
    installation depend on a process-global PATH mutation.
    """
    resolved = shutil.which(command)
    if resolved:
        return resolved
    program_files = os.environ.get("ProgramFiles")
    if platform.IS_WINDOWS and program_files:
        candidate = Path(program_files) / "LLVM" / "bin" / platform.exe_name(command)
        if candidate.is_file():
            return str(candidate)
    return command


def activate_windows_dynamic_asan_runtime(spec: BuildSpec, log) -> bool:
    """Expose Clang's Windows ASan DLL to every child in this lane.

    Linking records an import of ``clang_rt.asan_dynamic-x86_64.dll``; Windows
    then resolves that import from the executable directory or ``PATH``.  A
    standard LLVM installation keeps the DLL under the compiler resource
    directory, which is intentionally not a process-global PATH entry.  Ask
    the selected compiler for its own resource/runtime roots so this remains
    independent of the LLVM version and installation prefix.
    """
    if not platform.IS_WINDOWS:
        return True

    compiler = resolve_compiler_command(spec.c_compiler)
    roots: list[Path] = []
    for option in ("--print-runtime-dir", "--print-resource-dir"):
        result = proc.run([compiler, option], timeout=30)
        if not result.ok:
            continue
        reported = result.stdout.decode("utf-8", "replace").strip()
        if reported:
            roots.append(Path(reported))

    dll_name = "clang_rt.asan_dynamic-x86_64.dll"
    candidates: list[Path] = []
    for root in roots:
        candidates.extend((root, root / "lib" / "windows"))
    runtime_dir = next(
        (candidate for candidate in candidates
         if (candidate / dll_name).is_file()),
        None,
    )
    if runtime_dir is None:
        log(f"{dll_name} was not found under the selected Clang resource roots",
            error=True)
        return False

    path_entries = os.environ.get("PATH", "").split(os.pathsep)
    runtime_text = str(runtime_dir)
    if not any(entry and Path(entry) == runtime_dir for entry in path_entries):
        os.environ["PATH"] = runtime_text + os.pathsep + os.environ.get("PATH", "")
    log(f"Windows ASan runtime={runtime_text}")
    return True


def verify_configured(build_dir: Path, required_flag: str) -> str | None:
    """None when the tree really has the sanitizer on, else an error message.

    Checked even when the build is skipped: a caller may point the lane at a
    directory it configured itself, and a lane that silently runs a
    non-sanitized binary reports a clean result it never earned.
    """
    cache = cache_file(build_dir)
    if not cache.is_file():
        return None
    text = cache.read_text(encoding="utf-8")

    # Two shapes. `ENABLE_ASAN=ON` is a CMake option and appears as a BOOL
    # entry; a lane that instruments via raw flags instead passes a substring
    # (e.g. "-fsanitize=thread") that must occur somewhere in the cache.
    if "=" in required_flag and not required_flag.startswith("-"):
        name, value = required_flag.split("=", 1)
        if not any(line.startswith(f"{name}:") and line.endswith(f"={value}")
                   for line in text.splitlines()):
            return f"{build_dir} is configured without {required_flag}"
        return None

    if required_flag not in text:
        return f"{build_dir} is configured without {required_flag}"
    return None


def stale_source(binary: Path, project_dir: Path) -> Path | None:
    """A source file newer than `binary`, or None when the binary is current."""
    if not binary.is_file():
        return None
    binary_mtime = binary.stat().st_mtime
    for root in SOURCE_ROOTS:
        target = project_dir / root
        if target.is_file():
            if target.stat().st_mtime > binary_mtime:
                return target
            continue
        if not target.is_dir():
            continue
        for path in target.rglob("*"):
            try:
                if path.is_file() and path.stat().st_mtime > binary_mtime:
                    return path
            except OSError:
                continue
    return None


def rebuild_reason(binary: Path, project_dir: Path) -> str | None:
    """Why this lane must build, or None when the existing tree is current.

    The lanes ask this instead of taking a skip-build flag. A flag puts the
    answer in the hands of whoever remembers to set it, and gets it wrong in
    both directions: unset on an unchanged tree it rebuilds a whole sanitized
    compiler for nothing, and set on a changed one the lane used to fail rather
    than build what it needed. The tree already knows which case it is in.

    Deciding by mtime is deliberately eager -- a checkout that only restores a
    file's timestamp triggers a rebuild that ccache then serves from cache.
    Erring the other way would report a clean sanitizer result the current tree
    never earned, which is the one outcome a gate may not produce.
    """
    if not binary.is_file():
        return "no existing binary"
    stale = stale_source(binary, project_dir)
    if stale is not None:
        try:
            stale = stale.relative_to(project_dir)
        except ValueError:
            pass
        return f"{stale} is newer than the binary"
    return None


def configure(spec: BuildSpec, project_dir: Path, jobs: int,
              timeout: float | None, log) -> bool:
    """Configure the tree, reusing an existing one unless its generator differs.

    Ninja is the project's one generator. A Makefiles tree costs the ASan lane
    roughly 700s of near-serial build against ~100s of tests, because CMake's
    recursive make barely parallelizes. CMake cannot switch generators in place,
    so a stale non-Ninja tree is discarded and reconfigured; ccache keeps the
    one-time cost small.
    """
    build_dir = spec.build_dir
    generator = configured_generator(build_dir)
    if generator is not None and generator != "Ninja":
        log(f"{build_dir} was configured with '{generator}'; reconfiguring with Ninja")
        shutil.rmtree(build_dir, ignore_errors=True)
        generator = None

    # CMake writes the cache before it finishes generating build.ninja. An
    # interrupted configure therefore looks like a matching Ninja tree but
    # cannot be built. Reconfigure that incomplete tree instead of treating
    # the cache alone as reusable authority.
    if generator == "Ninja" and not (build_dir / "build.ninja").is_file():
        log(f"{build_dir} has no build.ninja; reconfiguring incomplete Ninja tree")
        shutil.rmtree(build_dir, ignore_errors=True)
        generator = None

    # Reuse requires the sanitizer flags to match too, not just the generator.
    # A tree left behind by an interrupted or differently-configured run can be
    # a perfectly good Ninja tree with the sanitizer OFF; reusing it would build
    # an uninstrumented binary and let the lane report a clean result it never
    # earned. Checked before the build, because verify_configured() afterwards
    # would only turn that into a late failure instead of a correct run.
    if generator is not None:
        mismatched = [flag for flag in spec.verification_targets()
                      if verify_configured(build_dir, flag) is not None]
        if mismatched:
            log(f"{build_dir} is configured without {', '.join(mismatched)}; reconfiguring")
            shutil.rmtree(build_dir, ignore_errors=True)
            generator = None

    if generator is not None:
        log(f"reusing existing configuration in {build_dir}")
        return True

    if not shutil.which("ninja"):
        log("ninja not found -- install it (brew install ninja / apt-get install ninja-build)",
            error=True)
        return False

    log(f"configuring {build_dir} (Ninja)")
    argv = [
        "cmake", "-S", project_dir, "-B", build_dir, "-G", "Ninja",
        f"-DCMAKE_BUILD_TYPE={spec.build_type}",
        f"-DCMAKE_C_COMPILER={resolve_compiler_command(spec.c_compiler)}",
        f"-DCMAKE_CXX_COMPILER={resolve_compiler_command(spec.cxx_compiler)}",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    ]
    argv.extend(f"-D{flag}" for flag in spec.sanitizer_flags)
    argv.extend(f"-D{flag}" for flag in spec.extra_cache)
    result = proc.run(argv, timeout=timeout)
    if not result.ok:
        log("configure failed", error=True)
        log(result.combined_text(), error=True)
        return False
    return True


def build(spec: BuildSpec, jobs: int, timeout: float | None, log) -> bool:
    argv = ["cmake", "--build", spec.build_dir, "-j", str(jobs)]
    if spec.targets:
        argv.append("--target")
        argv.extend(spec.targets)
    result = proc.run(argv, timeout=timeout)
    if not result.ok:
        log("build failed", error=True)
        log(result.combined_text()[-8000:], error=True)
        return False
    return True


def ctest(build_dir: Path, *, include: str = "", exclude: str = "", jobs: int = 1,
          timeout_each: int = 300, timeout: float | None = None) -> proc.ProcResult:
    argv = ["ctest", "--test-dir", str(build_dir), "--output-on-failure",
            "-j", str(jobs), "--timeout", str(timeout_each)]
    if include:
        argv.extend(["-R", include])
    if exclude:
        argv.extend(["-E", exclude])
    return proc.run(argv, timeout=timeout)


def ctest_has_match(build_dir: Path, pattern: str,
                    timeout: float | None = 120) -> bool:
    """Whether any registered test matches, so a lane can skip an empty subset
    instead of failing on ctest's 'no tests were found' exit."""
    result = proc.run(["ctest", "--test-dir", str(build_dir), "-N", "-R", pattern],
                      timeout=timeout)
    if not result.ok:
        return False
    import re

    return bool(re.search(r"Test +#", result.stdout.decode("utf-8", "replace")))


def default_jobs(env_var: str) -> int:
    """Sanitizer lanes run RUN_SERIAL, so they own the machine: default to all
    cores rather than a fixed number that leaves most of them idle."""
    return platform.env_int(env_var, platform.cpu_count())


class LaneLog:
    """Prefixed progress output, matching the shell lanes' `== [name]` shape."""

    def __init__(self, name: str) -> None:
        self.name = name

    def __call__(self, message: str, *, error: bool = False) -> None:
        import sys

        stream = sys.stderr if error else sys.stdout
        mark = "!!" if error else "=="
        stream.write(f"{mark} [{self.name}] {message}\n")
        stream.flush()
