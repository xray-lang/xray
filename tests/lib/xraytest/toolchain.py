"""Probe compilers, interpreters, and emulators by running them.

`command -v` reports presence, not usability. Two cases in this tree bite:

  - A Visual Studio developer prompt puts a `python3` App Execution Alias on
    PATH that exits without executing anything. It "exists" and does nothing.

  - C compiler drivers do not share a syntax-only command line. GCC and Clang
    use `-fsyntax-only`, Zig emits a throwaway object, and MSVC uses `/Zs`.

So every probe here launches the candidate and checks it did the thing. A probe
result is cached per-process: a suite asks the same question many times.
"""

from __future__ import annotations

import hashlib
import os
import re
import shutil
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from . import platform, proc

_probe_cache: "dict[str, str | None]" = {}

CC_DRIVER_GNU = "gnu"
CC_DRIVER_ZIG = "zig"
CC_DRIVER_MSVC = "msvc"

SYMBOL_DUMPER_DUMPBIN = "dumpbin"
SYMBOL_DUMPER_LLVM_NM = "llvm-nm"
SYMBOL_DUMPER_NM = "nm"

_MSVC_LINK_COMMAND_PREFIX = "Link command: "
_MSVC_LINK_DRIVER_RE = re.compile(
    r"^(?P<driver>.+[\\/](?:cl|clang-cl)(?:\.exe)?)\s+(?P<arguments>.+)$",
    re.IGNORECASE,
)
_MSVC_MAP_TIMESTAMP_RE = re.compile(
    r"^\s*Timestamp is (?P<timestamp>[0-9A-Fa-f]{8})(?:\s+\(.+\))?\s*$"
)
_MSVC_MAP_SYMBOL_RE = re.compile(
    r"^\s*[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}\s+"
    r"(?P<symbol>\S+)\s+[0-9A-Fa-f]{16}\s+"
    r"(?:(?:f|i)\s+)*(?P<owner>\S.*)$"
)
_MSVC_MAP_ENTRY_RE = re.compile(
    r"^\s*entry point at\s+[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}\s*$"
)


def find_python() -> "str | None":
    """A Python 3 that actually runs, not merely one that is on PATH.

    Executes a candidate before trusting it, defeating the App Execution Alias
    stub. XRAY_TEST_PYTHON overrides the search for pinned hosts.
    """
    if "python" in _probe_cache:
        return _probe_cache["python"]
    import os

    # An explicit override is authoritative: if the pinned interpreter does not
    # run, that is an error to surface, not a reason to silently pick another.
    # Falling back would defeat the point of pinning and could mask a broken CI
    # image behind a different Python than the one under test.
    override = os.environ.get("XRAY_TEST_PYTHON")
    if override:
        candidates: "list[str]" = [override]
    else:
        candidates = ["python3", "python", "py"]

    chosen: "str | None" = None
    for name in candidates:
        if not name:
            continue
        resolved = shutil.which(name) or (name if os.path.isabs(name) else None)
        if not resolved:
            continue
        result = proc.run([resolved, "-c", "import hashlib,sys;sys.exit(0)"], timeout=30)
        if result.ok:
            chosen = resolved
            break
    _probe_cache["python"] = chosen
    return chosen


@dataclass(frozen=True)
class CCompiler:
    """A usable C11 compiler and its syntax-check driver contract.

    The driver family is captured during the executable probe, not guessed by
    each caller. Generated C always gets an explicit C11 language mode.
    """

    path: str
    driver: str

    def syntax_check_argv(self, source: Path, include_dirs: Sequence, out_obj: Path) -> "list[str]":
        if self.driver == CC_DRIVER_ZIG:
            argv = [self.path, "cc", "-std=c11", "-c", "-o", str(out_obj)]
        elif self.driver == CC_DRIVER_MSVC:
            argv = [
                self.path,
                "/nologo",
                "/TC",
                "/std:c11",
                "/experimental:c11atomics",
                "/utf-8",
                "/Zs",
            ]
        else:
            argv = [self.path, "-std=c11", "-fsyntax-only"]
        include_prefix = "/I" if self.driver == CC_DRIVER_MSVC else "-I"
        for inc in include_dirs:
            argv.append(f"{include_prefix}{inc}")
        argv.append(str(source))
        return argv


_DUMPBIN_SYMBOL_RE = re.compile(
    r"^\s*[0-9A-F]+\s+[0-9A-F]+\s+"
    r"(?P<section>UNDEF|SECT[0-9A-F]+|ABS)\s+.*?\|\s*(?P<symbol>\S.*)$",
    re.IGNORECASE,
)
_POSIX_NM_SYMBOL_RE = re.compile(
    r"^(?:.*?:\s+)?(?P<symbol>\S+)\s+(?P<kind>[A-Za-z?])"
    r"(?:\s+(?:[0-9A-Fa-f]+|-))?(?:\s+(?:[0-9A-Fa-f]+|-))?\s*$"
)


def _normalize_dumpbin_symbols(raw: str) -> "tuple[str | None, str]":
    symbols: "list[str]" = []
    for line in raw.splitlines():
        if "|" not in line:
            continue
        match = _DUMPBIN_SYMBOL_RE.match(line)
        if not match:
            return None, "unrecognized dumpbin symbol row"
        if match.group("section").upper() == "UNDEF":
            continue
        symbols.append(match.group("symbol").split(None, 1)[0])
    if not symbols:
        return None, "dumpbin output contains no defined symbols"
    return "\n".join(symbols), ""


def _normalize_posix_nm_symbols(raw: str) -> "tuple[str | None, str]":
    symbols: "list[str]" = []
    for line in raw.splitlines():
        stripped = line.strip()
        if not stripped or stripped.endswith(":"):
            continue
        match = _POSIX_NM_SYMBOL_RE.match(stripped)
        if not match:
            return None, "unrecognized nm symbol row"
        if match.group("kind").upper() == "U":
            return None, "defined-only nm output contains an undefined symbol"
        symbols.append(match.group("symbol"))
    if not symbols:
        return None, "nm output contains no defined symbols"
    return "\n".join(symbols), ""


@dataclass(frozen=True)
class SymbolDumper:
    """A probed provider that returns only normalized, defined symbol names."""

    path: str
    driver: str

    def dump_argv(self, artifact: Path) -> "list[str]":
        if self.driver == SYMBOL_DUMPER_DUMPBIN:
            return [self.path, "/nologo", "/symbols", str(artifact)]
        if self.driver == SYMBOL_DUMPER_NM and platform.IS_DARWIN:
            return [self.path, "-U", "-P", str(artifact)]
        return [self.path, "--defined-only", "--format=posix", str(artifact)]

    def dump_defined_symbols(self, artifact: Path) -> "tuple[bool, str]":
        result = proc.run(self.dump_argv(artifact), timeout=120)
        if not result.ok:
            state = "timed out" if result.timed_out else f"exit {result.returncode}"
            detail = result.combined_text().strip()
            suffix = f": {detail}" if detail else ""
            return False, f"{self.path} {state}{suffix}"

        raw = result.stdout.decode("utf-8", "replace")
        if self.driver == SYMBOL_DUMPER_DUMPBIN:
            normalized, error = _normalize_dumpbin_symbols(raw)
        else:
            normalized, error = _normalize_posix_nm_symbols(raw)
        if normalized is None:
            detail = result.combined_text().strip()
            suffix = f": {detail}" if detail else ""
            return False, f"{self.path}: {error}{suffix}"
        return True, normalized


@dataclass(frozen=True)
class MsvcLinkMapEvidence:
    """Defined symbols bound to one exact MSVC link command and PE image."""

    symbols: str
    symbol_count: int
    map_path: Path
    driver: str
    command_sha256: str
    binary_sha256: str
    coff_timestamp: int


def _same_path(left: "str | Path", right: "str | Path") -> bool:
    return os.path.normcase(os.path.abspath(os.fspath(left))) == os.path.normcase(
        os.path.abspath(os.fspath(right))
    )


def _pe_coff_timestamp(binary: Path) -> "tuple[int | None, str]":
    try:
        data = binary.read_bytes()
    except OSError as exc:
        return None, f"cannot read linked PE image: {exc}"
    if len(data) < 0x40 or data[:2] != b"MZ":
        return None, "linked output is not a PE image"
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset > len(data) - 12 or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        return None, "linked output has no valid PE signature"
    return struct.unpack_from("<I", data, pe_offset + 8)[0], ""


def _parse_msvc_link_command(
    log: str, compiler: CCompiler, binary: Path
) -> "tuple[list[str] | None, list[Path], list[Path], str]":
    lines = [line[len(_MSVC_LINK_COMMAND_PREFIX):].strip()
             for line in log.splitlines()
             if line.startswith(_MSVC_LINK_COMMAND_PREFIX)]
    if len(lines) != 1:
        return None, [], [], "expected exactly one dumped final link command"
    match = _MSVC_LINK_DRIVER_RE.match(lines[0])
    if not match:
        return None, [], [], "dumped link command has no supported MSVC driver"
    driver = match.group("driver")
    if compiler.driver != CC_DRIVER_MSVC or not _same_path(driver, compiler.path):
        return None, [], [], "dumped link driver does not match the verified MSVC driver"

    arguments = match.group("arguments")
    if any(char in arguments for char in ('"', "'", "\t", "\r", "\n")):
        return None, [], [], "dumped link arguments contain ambiguous quoting or control text"
    argv = [driver, *arguments.split()]
    link_indexes = [i for i, arg in enumerate(argv) if arg.lower() == "/link"]
    if len(link_indexes) != 1:
        return None, [], [], "dumped command has no unique MSVC /link boundary"
    link_index = link_indexes[0]
    if any(arg.lower().startswith("/map") for arg in argv[1:]):
        return None, [], [], "dumped command already contains linker map controls"

    outputs = [arg[3:] for arg in argv[1:link_index]
               if arg.lower().startswith("/fe") and len(arg) > 3]
    if len(outputs) != 1 or not _same_path(outputs[0], binary):
        return None, [], [], "dumped command output does not match the target PE image"

    object_inputs = [Path(arg) for arg in argv[1:link_index]
                     if arg.lower().endswith(".obj")]
    if not object_inputs:
        return None, [], [], "dumped command has no final object input"
    missing_objects = [path for path in object_inputs if not path.is_file()]
    if missing_objects:
        return None, [], [], f"final object input is missing: {missing_objects[0]}"

    archive_inputs = [Path(arg) for arg in argv[1:link_index]
                      if arg.lower().endswith(".lib") and ("/" in arg or "\\" in arg)]
    missing_archives = [path for path in archive_inputs if not path.is_file()]
    if missing_archives:
        return None, [], [], f"final archive input is missing: {missing_archives[0]}"
    return argv, object_inputs, archive_inputs, ""


def _map_target_name(binary: Path) -> str:
    name = binary.name
    if name.lower().endswith(".tmp"):
        return name[:-4]
    return binary.stem


def _parse_msvc_link_map(
    text: str,
    binary: Path,
    object_inputs: Sequence,
    archive_inputs: Sequence,
) -> "tuple[str | None, int | None, str]":
    lines = text.splitlines()
    nonempty = [line.strip() for line in lines if line.strip()]
    if not nonempty or nonempty[0].casefold() != _map_target_name(binary).casefold():
        return None, None, "linker map target does not match the linked PE image"

    timestamps = [_MSVC_MAP_TIMESTAMP_RE.match(line) for line in lines]
    timestamp_values = [int(match.group("timestamp"), 16)
                        for match in timestamps if match]
    if len(timestamp_values) != 1:
        return None, None, "linker map has no unique COFF timestamp"
    pe_timestamp, error = _pe_coff_timestamp(binary)
    if pe_timestamp is None:
        return None, None, error
    if timestamp_values[0] != pe_timestamp:
        return None, None, "linker map timestamp does not match the linked PE image"

    public_indexes = [i for i, line in enumerate(lines) if "Publics by Value" in line]
    entry_indexes = [i for i, line in enumerate(lines) if _MSVC_MAP_ENTRY_RE.match(line)]
    static_indexes = [i for i, line in enumerate(lines) if line.strip() == "Static symbols"]
    if not (len(public_indexes) == len(entry_indexes) == len(static_indexes) == 1):
        return None, None, "linker map is missing a unique defined-symbol section"
    public_index, entry_index, static_index = (
        public_indexes[0], entry_indexes[0], static_indexes[0]
    )
    if not public_index < entry_index < static_index:
        return None, None, "linker map defined-symbol sections are out of order"

    symbols: "list[str]" = []
    owners: "list[str]" = []
    for line in [*lines[public_index + 1:entry_index], *lines[static_index + 1:]]:
        if not line.strip():
            continue
        match = _MSVC_MAP_SYMBOL_RE.match(line)
        if not match:
            return None, None, "linker map contains a malformed defined-symbol row"
        symbols.append(match.group("symbol"))
        owners.append(match.group("owner"))
    if not symbols:
        return None, None, "linker map contains no defined symbols"

    folded_owners = [owner.casefold() for owner in owners]
    for path in object_inputs:
        name = Path(path).name.casefold()
        if not any(owner == name or owner.endswith(":" + name) for owner in folded_owners):
            return None, None, f"linker map does not cover final object input: {Path(path).name}"
    for path in archive_inputs:
        prefix = Path(path).stem.casefold() + ":"
        if not any(owner.startswith(prefix) for owner in folded_owners):
            return None, None, f"linker map does not cover final archive input: {Path(path).name}"
    return "\n".join(symbols), pe_timestamp, ""


def capture_msvc_link_map(
    log: str,
    compiler: CCompiler,
    binary: Path,
    map_path: Path,
    timeout: float | None = 120,
) -> "tuple[MsvcLinkMapEvidence | None, str]":
    """Relink one dumped MSVC command with `/MAP` and bind it to the PE.

    Xray has already selected and successfully invoked the provider command.
    This function accepts only that exact verified MSVC driver, exact output,
    and extant final inputs. The generated map must cover both public and static
    definitions and carry the PE's exact COFF timestamp.
    """
    if map_path.parent.resolve() != binary.parent.resolve() or map_path.suffix.lower() != ".map":
        return None, "linker map path is not owned beside the target PE image"
    if map_path.is_symlink():
        return None, "linker map path must not be a symlink"
    argv, object_inputs, archive_inputs, error = _parse_msvc_link_command(
        log, compiler, binary
    )
    if argv is None:
        return None, error
    if not binary.is_file():
        return None, "target PE image is missing before map relink"
    if map_path.exists():
        try:
            map_path.unlink()
        except OSError as exc:
            return None, f"cannot clear stale linker map: {exc}"

    mapped_argv = [*argv, f"/MAP:{map_path}"]
    result = proc.run(mapped_argv, timeout=timeout)
    if not result.ok:
        state = "timed out" if result.timed_out else f"exit {result.returncode}"
        detail = result.combined_text().strip()
        return None, f"MSVC map relink {state}" + (f": {detail}" if detail else "")
    if not binary.is_file():
        return None, "MSVC map relink produced no target PE image"
    if not map_path.is_file() or map_path.stat().st_size == 0:
        return None, "MSVC map relink produced no non-empty linker map"
    try:
        map_text = map_path.read_text(encoding="utf-8", errors="strict")
    except (OSError, UnicodeError) as exc:
        return None, f"cannot read linker map as strict UTF-8: {exc}"
    symbols, timestamp, error = _parse_msvc_link_map(
        map_text, binary, object_inputs, archive_inputs
    )
    if symbols is None or timestamp is None:
        return None, error

    command_bytes = "\0".join(mapped_argv).encode("utf-8", "strict")
    binary_bytes = binary.read_bytes()
    return MsvcLinkMapEvidence(
        symbols=symbols,
        symbol_count=len(symbols.splitlines()),
        map_path=map_path.resolve(),
        driver=os.path.abspath(compiler.path),
        command_sha256=hashlib.sha256(command_bytes).hexdigest(),
        binary_sha256=hashlib.sha256(binary_bytes).hexdigest(),
        coff_timestamp=timestamp,
    ), ""


def _c_compiler_driver(path: str) -> str:
    name = Path(path).name.lower()
    if name in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
        return CC_DRIVER_MSVC
    if "zig" in name:
        return CC_DRIVER_ZIG
    return CC_DRIVER_GNU


def _c_compiler_probe_argv(path: str, driver: str) -> list[str]:
    if driver == CC_DRIVER_MSVC:
        return [path, "/nologo", "/?"]
    return [path, "--version"]


def _find_c_compiler(cache_key: str, candidates: Sequence) -> "CCompiler | None":
    if cache_key in _probe_cache:
        cached = _probe_cache[cache_key]
        return _CC_OBJS.get(cached) if cached else None

    for name in candidates:
        resolved = shutil.which(name)
        if not resolved:
            continue
        driver = _c_compiler_driver(resolved)
        probe = proc.run(_c_compiler_probe_argv(resolved, driver), timeout=30)
        if probe.ok:
            cc = CCompiler(path=resolved, driver=driver)
            _probe_cache[cache_key] = resolved
            _CC_OBJS[resolved] = cc
            return cc
    _probe_cache[cache_key] = None
    return None


def find_c_compiler() -> "CCompiler | None":
    """Find the general-purpose C driver used by build-and-run test fixtures."""
    import os

    candidates: "list[str]" = []
    if os.environ.get("CC"):
        candidates.append(os.environ["CC"])
    candidates.extend(["cc", "clang", "gcc"])
    return _find_c_compiler("cc", candidates)


def find_c_syntax_compiler() -> "CCompiler | None":
    """Find a verified driver capable of checking generated hosted C11.

    Windows developer environments expose MSVC as `cl`, not `cc`. Keep this
    capability separate from build-and-run fixture compilers: syntax-only C11
    has a precise MSVC contract, while freestanding/shared-library fixture
    construction requires different driver-specific link capabilities.
    """
    import os

    candidates: "list[str]" = []
    if os.environ.get("CC"):
        candidates.append(os.environ["CC"])
    if platform.IS_WINDOWS:
        candidates.extend(["cl", "clang-cl"])
    candidates.extend(["cc", "clang", "gcc"])
    return _find_c_compiler("c_syntax", candidates)


def _symbol_dumper_driver(path: str) -> "str | None":
    name = Path(path).name.lower()
    if name in ("dumpbin", "dumpbin.exe"):
        return SYMBOL_DUMPER_DUMPBIN
    if name in ("llvm-nm", "llvm-nm.exe"):
        return SYMBOL_DUMPER_LLVM_NM
    if name in ("nm", "nm.exe"):
        return SYMBOL_DUMPER_NM
    return None


def _symbol_dumper_probe(path: str, driver: str) -> bool:
    if driver == SYMBOL_DUMPER_DUMPBIN:
        # DUMPBIN's help path returns 1100. Parsing its own PE headers is a
        # real, zero-exit capability probe with an output shape an impostor
        # cannot satisfy merely by accepting the command line.
        result = proc.run([path, "/nologo", "/headers", path], timeout=30)
        text = result.combined_text().lower()
        return (result.ok and "pe signature found" in text and
                "file type: executable image" in text)

    probes = [[path, "--version"]]
    if driver == SYMBOL_DUMPER_NM and platform.IS_DARWIN:
        probes.append([path, "-V"])
    for argv in probes:
        result = proc.run(argv, timeout=30)
        text = result.combined_text().lower()
        if (result.ok and "nm" in text and
                ("gnu" in text or "llvm" in text or "apple" in text)):
            return True
    return False


def find_symbol_dumper() -> "SymbolDumper | None":
    """Find a verified provider of defined symbols for binaries and archives.

    A resolved executable is not enough: the probe validates both its identity
    and a successful provider-specific command. Once selected, a dump failure
    is final for that artifact and never falls through to a different tool.
    """
    if "symbol_dumper" in _probe_cache:
        cached = _probe_cache["symbol_dumper"]
        return _SYMBOL_DUMPER_OBJS.get(cached) if cached else None

    candidates = (["dumpbin", "llvm-nm", "nm"] if platform.IS_WINDOWS
                  else ["nm", "llvm-nm"])
    for name in candidates:
        resolved = shutil.which(name)
        if not resolved:
            continue
        driver = _symbol_dumper_driver(resolved)
        if driver is None or not _symbol_dumper_probe(resolved, driver):
            continue
        dumper = SymbolDumper(path=resolved, driver=driver)
        _probe_cache["symbol_dumper"] = resolved
        _SYMBOL_DUMPER_OBJS[resolved] = dumper
        return dumper

    _probe_cache["symbol_dumper"] = None
    return None


_CC_OBJS: "dict[str, CCompiler]" = {}
_SYMBOL_DUMPER_OBJS: "dict[str, SymbolDumper]" = {}


def reset_probe_cache() -> None:
    """Clear cached probe results. For tests that vary the environment."""
    _probe_cache.clear()
    _CC_OBJS.clear()
    _SYMBOL_DUMPER_OBJS.clear()
