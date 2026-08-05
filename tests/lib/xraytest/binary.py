"""Inspect built artifacts: symbol tables and disassembly.

Several AOT gates assert what a binary is *made of* rather than what it does --
which symbols it leaves undefined, whether a hot function stayed straight-line,
whether generated storage was emitted const. Each shell script grew its own
nm/objdump invocation with its own fallbacks; this is the one implementation.

Tool selection is by probe, not by assumption: llvm-nm and llvm-objdump when
present, otherwise the platform's nm/objdump, otherwise otool on macOS. A gate
that finds no disassembler skips rather than passing vacuously.
"""

from __future__ import annotations

import re
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Sequence

from . import proc

# Darwin's nm prefixes C symbols with '_'; callers match on the bare name.
_LEADING_UNDERSCORE = re.compile(r"^_")


def strip_underscore(symbol: str) -> str:
    return _LEADING_UNDERSCORE.sub("", symbol)


def _first_tool(names: Sequence, env_override: "Optional[str]" = None) -> "Optional[str]":
    import os

    if env_override:
        candidate = os.environ.get(env_override)
        if candidate and shutil.which(candidate):
            return shutil.which(candidate)
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    return None


def find_nm() -> "Optional[str]":
    return _first_tool(("llvm-nm", "nm"), "LLVM_NM")


def find_disassembler() -> "Optional[tuple]":
    """(path, argv_template_kind) for the best available disassembler."""
    llvm = shutil.which("llvm-objdump")
    if llvm:
        return llvm, "llvm-objdump"
    objdump = shutil.which("objdump")
    if objdump:
        return objdump, "objdump"
    otool = shutil.which("otool")
    if otool:
        return otool, "otool"
    return None


def symbols(binary: Path, *, undefined_only: bool = False,
            global_only: bool = False,
            timeout: "float | None" = 120) -> "Optional[List[str]]":
    """Symbol lines from nm, or None when nm is unavailable or fails.

    `undefined_only` asks for the undefined set (-u), which is what the ABI
    gates compare against an allow-list.

    `global_only` asks for external symbols (-g), defined AND undefined. A
    containment gate needs both: a binary that merely *references* a compiler
    symbol is linked against the compiler just as surely as one that defines
    it, and -U (defined only) would not see that.
    """
    tool = find_nm()
    if not tool:
        return None
    if undefined_only:
        flags = ["-u"]
    elif global_only:
        flags = ["-g"]
    else:
        flags = ["-U"]
    result = proc.run([tool, *flags, binary], timeout=timeout)
    if not result.ok and not undefined_only:
        # -U (defined only) is not universal; plain nm is the fallback.
        result = proc.run([tool, binary], timeout=timeout)
    if not result.ok:
        return None
    return result.stdout.decode("utf-8", "replace").splitlines()


def undefined_symbol_names(binary: Path, timeout: "float | None" = 120
                           ) -> "Optional[List[str]]":
    """Sorted, de-duplicated undefined symbol names with any '_' prefix removed."""
    lines = symbols(binary, undefined_only=True, timeout=timeout)
    if lines is None:
        return None
    names = set()
    for line in lines:
        parts = line.split()
        if parts:
            names.add(strip_underscore(parts[-1]))
    return sorted(names)


def disassemble(binary: Path, timeout: "float | None" = 300) -> "Optional[str]":
    """Disassembly text, or None when no disassembler is installed."""
    found = find_disassembler()
    if not found:
        return None
    tool, kind = found
    if kind == "llvm-objdump":
        argv = [tool, "--no-show-raw-insn", "-d", binary]
    elif kind == "objdump":
        argv = [tool, "-d", binary]
    else:
        argv = [tool, "-tvV", binary]
    result = proc.run(argv, timeout=timeout)
    if not result.ok:
        return None
    return result.stdout.decode("utf-8", "replace")


# A symbol's body ends where the next symbol label begins. Both the
# `<name>:` (objdump) and `name:` (otool) label forms terminate it.
_LABEL_RE = re.compile(r"<[A-Za-z_.$][A-Za-z0-9_.$]*>:|^_?[A-Za-z_.$][A-Za-z0-9_.$]*:")


def extract_symbol_body(disassembly: str, symbol: str) -> str:
    """The disassembled body of one symbol, tolerating the '_' prefix."""
    start = re.compile(r"<_?" + re.escape(symbol) + r">:|^_?" + re.escape(symbol) + r":")
    out: List[str] = []
    active = False
    for line in disassembly.splitlines():
        if not active:
            if start.search(line):
                active = True
                out.append(line)
            continue
        if _LABEL_RE.search(line):
            break
        out.append(line)
    return "\n".join(out)


# Calls and conditional branches, across x86 / ARM / RISC-V mnemonics.
CONTROL_FLOW_RE = re.compile(
    r"[\s](call[a-z]*|bl|blr|cbz|cbnz|tbz|tbnz|b\.[a-z]+|j[a-z]+)[\s]", re.IGNORECASE
)


def has_control_flow(body: str) -> bool:
    """Whether a disassembled body contains a call or conditional branch.

    Used by the straight-line gates: an unchecked scalar load/store must not
    re-enter a helper or take a bounds branch.
    """
    return bool(CONTROL_FLOW_RE.search(body))
