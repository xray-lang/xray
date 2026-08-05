#!/usr/bin/env python3
"""Reverse invariants for the Xi IR layer: static checks that block merge.

These are the properties that cannot be tested from the outside, only asserted
against the source: that IR code allocates through the arena, that no pass
silently falls through a switch, that generated metadata still matches its
source, that every pass is registered and tested. Type B (verify pass) and type
C (pass table validator) are enforced at runtime, not here.

Exit 0 when all checks pass, 1 when any fails.

Usage:
    check_xi_invariants.py              run all checks
    check_xi_invariants.py --verbose    show every match rather than the first 20
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Sequence


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import proc  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
IR_DIR = REPO_ROOT / "src" / "ir"

MAX_SHOWN = 20
MAX_FUNCTION_LINES = 150
COMBINED_TEST = Path("tests/unit/ir/test_xi_opt.c")
COMBINED_MIN_REFS = 3

# Pre-existing long functions grandfathered before strict enforcement. New code
# must not exceed the limit. Each entry MUST be removed when its function is
# refactored.
INV13_ALLOWLIST: tuple[tuple[str, str], ...] = (
    ("xi_emit.c", "xi_emit"),
    ("xi_emit_cf.c", "emit_block"),
    ("xi_emit_object.c", "emit_class_create_impl"),
    ("xi_escape.c", "use_escape_level"),
    ("xi_loop.c", "xi_compute_loops"),
    ("xi_lower_class.inc.c", "xi_lower_class_decl"),
    ("xi_lower_expr.c", "lower_call"),
    ("xi_lower_expr.c", "lower_new_expr"),
    ("xi_lower_expr.c", "xi_lower_expr"),
    ("xi_lower_stmt.c", "lower_try_catch_impl"),
)

RAW_ALLOC = re.compile(r"\b(malloc|calloc|realloc|free)\s*\(")
ARENA_NAMES = ("xr_malloc", "xr_calloc", "xr_realloc", "xr_free", "xr_arena")
COMMENT_LINE = re.compile(r"^\s*(//|\*)")
PHASE_COORD = re.compile(r"Xi-[0-9]+\.[0-9]+")
PHASE_COMMENT = re.compile(r"^\s*(\*|//).*\bPhase [A-Z0-9]")
MODE_FLAG = re.compile(
    r"XI_[A-Z_]*_MODE|XI_[A-Z_]*_MODEL|XI_ENABLE_TBAA|XI_RANGE_ENABLED|"
    r"XI_GVN_MODE|XRAY_XI_TBAA|XRAY_XI_SPEC[^=_]")
DEFAULT_BREAK_ONE_LINE = re.compile(r"^\s*default:\s*break;\s*$")
DEFAULT_ALONE = re.compile(r"^\s*default:\s*$")
BREAK_ALONE = re.compile(r"^\s*break;\s*$")
OP_ENUM = re.compile(r"^\s*(XI_[A-Z0-9_]+)")
OP_CASE = re.compile(r"case (XI_[A-Z0-9_]+)")
PASS_ENTRY = re.compile(r'\{"([a-z0-9_]+)",')
MAX_STATS = re.compile(r"XI_MAX_PASS_STATS\s+([0-9]+)")
PASS_FUNC_DEF = re.compile(r"^XR_FUNC.*?(xi_opt_[a-z_]*)\s*\(")
FUNC_OPEN = re.compile(r"^[A-Za-z_].*\(")
FUNC_CLOSE = re.compile(r"^\}")


@dataclass
class Report:
    verbose: bool = False
    failed: bool = False

    def ok(self, message: str) -> None:
        print(f"  [PASS] {message}")

    def bad(self, message: str) -> None:
        print(f"  [FAIL] {message}")
        self.failed = True

    def warn(self, message: str) -> None:
        print(f"  [WARN] {message}")

    def matches(self, lines: Sequence[str]) -> None:
        """Always print the offending locations.

        A gate that says "violations found" without saying where sends the
        reader back to re-derive the grep by hand. --verbose only raises the cap.
        """
        if not lines:
            return
        shown = lines if self.verbose else lines[:MAX_SHOWN]
        for line in shown:
            print(f"    {line}")
        if not self.verbose and len(lines) > MAX_SHOWN:
            print("    ... (--verbose for the rest)")


def sources(root: Path, suffixes: Sequence[str] = (".c", ".h")) -> list[Path]:
    if not root.is_dir():
        return []
    return sorted(p for p in root.rglob("*") if p.is_file() and p.suffix in suffixes)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def grep(roots: Sequence[Path], pattern: re.Pattern,
         skip_comments: bool = False,
         exclude_substrings: Sequence[str] = ()) -> list[str]:
    """`path:line:text` for each matching line, in path then line order."""
    hits: list[str] = []
    for root in roots:
        for path in sources(root):
            relative = path.relative_to(REPO_ROOT)
            for number, line in enumerate(read(path).splitlines(), start=1):
                if not pattern.search(line):
                    continue
                if any(token in line for token in exclude_substrings):
                    continue
                if skip_comments and COMMENT_LINE.search(line):
                    continue
                hits.append(f"{relative}:{number}:{line}")
    return hits


def check_no_raw_alloc(report: Report) -> None:
    print("--- INV-5: No raw malloc/free in src/ir/ ---")
    hits = grep([IR_DIR], RAW_ALLOC, skip_comments=True,
                exclude_substrings=ARENA_NAMES)
    if not hits:
        report.ok("No raw malloc/free in src/ir/")
    else:
        report.bad("Raw malloc/free found in src/ir/")
        report.matches(hits)


def check_no_phase_coordinates(report: Report) -> None:
    print("--- INV-14: No phase coordinates in source ---")
    roots = [REPO_ROOT / "src", REPO_ROOT / "stdlib", REPO_ROOT / "include"]
    hits = grep(roots, PHASE_COORD, skip_comments=True)
    if not hits:
        report.ok("No Xi-N.M phase coordinates")
    else:
        report.bad("Phase coordinates found in source")
        report.matches(hits)

    hits = grep([IR_DIR], PHASE_COMMENT)
    if not hits:
        report.ok("No 'Phase X' in comments")
    else:
        report.bad("'Phase X' found in comments")
        report.matches(hits)


def check_no_mode_flags(report: Report) -> None:
    print("--- INV-15: No fallback mode flags ---")
    hits = grep([IR_DIR], MODE_FLAG)
    if not hits:
        report.ok("No fallback mode flags")
    else:
        report.bad("Fallback mode flags found")
        report.matches(hits)


def check_no_silent_defaults(report: Report) -> None:
    print("--- INV-X: No silent default branches in src/ir/ ---")
    hits: list[str] = []
    for path in sources(IR_DIR):
        lines = read(path).splitlines()
        relative = path.relative_to(REPO_ROOT)
        for index, line in enumerate(lines):
            one_line = DEFAULT_BREAK_ONE_LINE.match(line)
            split = (DEFAULT_ALONE.match(line) and index + 1 < len(lines)
                     and BREAK_ALONE.match(lines[index + 1]))
            if one_line or split:
                hits.append(f"{relative}:{index + 1}:{line}")
    if not hits:
        report.ok("No default: break silent fallbacks in src/ir/")
    else:
        report.bad("Silent default branches found in src/ir/")
        report.matches(hits)


def check_op_name_sync(report: Report) -> None:
    print("--- INV-2: op name sync ---")
    xi_h = IR_DIR / "xi.h"
    gen_h = IR_DIR / "xi_ops_gen.h"
    if not (xi_h.is_file() and gen_h.is_file()):
        report.warn("xi.h or xi_ops_gen.h not found, skipping")
        return

    ops: set[str] = set()
    inside = False
    for line in read(xi_h).splitlines():
        if "XI_CONST = 0" in line:
            inside = True
        if inside:
            match = OP_ENUM.match(line)
            if match:
                ops.add(match.group(1))
            if "XI_OP_COUNT" in line:
                break
    ops.discard("XI_OP_COUNT")

    names = set(OP_CASE.findall(read(gen_h)))
    names.discard("XI_OP_COUNT")

    missing = sorted(ops - names)
    extra = sorted(names - ops)
    if not missing and not extra:
        report.ok("every XI_* op has a generated name entry, no orphan cases")
    elif missing:
        report.bad("ops missing from generated Xi metadata:")
        for name in missing:
            print(f"    {name}")
    else:
        report.bad("generated Xi metadata has cases for unknown XI_* ops:")
        for name in extra:
            print(f"    {name}")


def check_generated_sync(report: Report) -> None:
    print("--- INV-2B: generated Xi/AOT source sync ---")
    result = proc.run([sys.executable,
                                    REPO_ROOT / "scripts" / "check_xi_aot_generated_sync.py"],
                                   cwd=REPO_ROOT)
    if result.ok:
        report.ok("generated Xi/AOT metadata is in sync")
        if report.verbose:
            sys.stdout.write(result.combined_text())
    else:
        report.bad("generated Xi/AOT metadata is stale or cannot be regenerated")
        for line in result.combined_text().splitlines():
            print(f"    {line}")


def allowlisted(path: Path, signature: str) -> bool:
    for allowed_file, allowed_func in INV13_ALLOWLIST:
        if path.name == allowed_file and allowed_func in signature:
            return True
    return False


def check_function_length(report: Report) -> None:
    print("--- INV-13: Function length check (src/ir/) ---")
    violations = False
    for path in sorted(IR_DIR.glob("*.c")):
        lines = read(path).splitlines()
        start = 0
        signature = ""
        relative = path.relative_to(REPO_ROOT)
        for number, line in enumerate(lines, start=1):
            if FUNC_OPEN.match(line) and line.rstrip().endswith("{"):
                start = number
                signature = line
                continue
            if FUNC_CLOSE.match(line) and start > 0:
                length = number - start
                if length > MAX_FUNCTION_LINES:
                    if allowlisted(path, signature):
                        if report.verbose:
                            print(f"  [ALLOW] {relative}:{start}: ~{length} "
                                  f"lines: {signature}")
                    else:
                        print(f"  [NEW-VIOLATION] {relative}:{start}: ~{length} "
                              f"lines (>{MAX_FUNCTION_LINES}): {signature}")
                        violations = True
                start = 0
    if violations:
        report.bad(f"New functions exceeding {MAX_FUNCTION_LINES} lines found "
                   "in src/ir/")
    else:
        report.ok(f"No new functions exceed {MAX_FUNCTION_LINES} lines in src/ir/")


def pass_table_names(text: str) -> list[str]:
    """Pass names in declaration order, read from the single pass table."""
    names: list[str] = []
    inside = False
    for line in text.splitlines():
        if "static const XiPassDesc xi_pass_table[]" in line:
            inside = True
        if inside:
            names.extend(PASS_ENTRY.findall(line))
            if line.startswith("};"):
                break
    return names


def check_stats_slots(report: Report) -> None:
    print("--- INV-10: Pipeline stats slot sync ---")
    opt_c = IR_DIR / "xi_opt.c"
    pass_h = IR_DIR / "xi_pass.h"
    if not (opt_c.is_file() and pass_h.is_file()):
        report.warn("xi_opt.c or xi_pass.h not found, skipping")
        return

    count = len(pass_table_names(read(opt_c)))
    match = MAX_STATS.search(read(pass_h))
    if not match:
        report.warn("XI_MAX_PASS_STATS not found in xi_pass.h")
    elif count > int(match.group(1)):
        report.bad(f"Pass table has {count} entries but XI_MAX_PASS_STATS is "
                   f"only {match.group(1)}")
    else:
        report.ok(f"Pass table ({count} entries) fits within XI_MAX_PASS_STATS "
                  f"({match.group(1)})")


def check_pass_registration(report: Report) -> None:
    print("--- INV-1: Pass registration check ---")
    opt_c = IR_DIR / "xi_opt.c"
    if not opt_c.is_file():
        report.warn("xi_opt.c not found, skipping")
        return

    functions: set[str] = set()
    for path in sources(IR_DIR, (".c",)):
        for line in read(path).splitlines():
            match = PASS_FUNC_DEF.match(line)
            if match:
                functions.add(match.group(1))

    text = read(opt_c)
    missing = sorted(name for name in functions if name not in text)
    if not missing:
        report.ok("All xi_opt_* functions referenced in xi_opt.c")
    else:
        report.bad("xi_opt_* functions not found in xi_opt.c (must register or "
                   "mark as static helper):")
        for name in missing:
            print(f"  {name}")


def check_pass_table_coverage(report: Report) -> None:
    print("--- INV-11: XRAY_XI_PASS pass table coverage ---")
    opt_c = IR_DIR / "xi_opt.c"
    if not opt_c.is_file():
        report.warn("xi_opt.c not found, skipping")
        return

    text = read(opt_c)
    names = pass_table_names(text)
    duplicates = sorted({name for name in names if names.count(name) > 1})
    if "XRAY_XI_PASS" not in text or "pass_index_by_name" not in text:
        report.bad("XRAY_XI_PASS parser or pass_index_by_name is missing")
    elif duplicates:
        report.bad("Duplicate Xi pass names:")
        for name in duplicates:
            print(f"    {name}")
    else:
        report.ok("XRAY_XI_PASS uses the single pass table and pass names are "
                  "unique")


def check_test_coverage(report: Report) -> None:
    print("--- INV-12: Test coverage check ---")
    combined = REPO_ROOT / COMBINED_TEST
    combined_text = read(combined) if combined.is_file() else ""
    test_dir = REPO_ROOT / "tests" / "unit" / "ir"

    missing_dedicated: list[str] = []
    no_coverage: list[str] = []
    for path in sorted(IR_DIR.glob("xi_opt_*.c")):
        base = path.stem
        short = base[len("xi_opt_"):]
        # Accept any of: test_<base>.c, test_xi_<short>.c, test_<base>_*.c
        if ((test_dir / f"test_{base}.c").is_file()
                or (test_dir / f"test_xi_{short}.c").is_file()
                or any(test_dir.glob(f"test_{base}_*.c"))):
            continue
        refs = sum(1 for line in combined_text.splitlines() if base in line)
        if refs >= COMBINED_MIN_REFS:
            missing_dedicated.append(f"{base} ({refs} refs in test_xi_opt.c)")
        else:
            no_coverage.append(base)

    if no_coverage:
        report.bad("Passes with no test coverage at all:")
        for entry in no_coverage:
            print(f"    {entry}")
    elif missing_dedicated:
        report.bad("Passes covered only by test_xi_opt.c (must have dedicated "
                   "test):")
        for entry in missing_dedicated:
            print(f"    {entry}")
    else:
        report.ok("Every xi_opt_*.c has a dedicated test file")


def main(argv: list[str]) -> int:
    report = Report(verbose="--verbose" in argv[1:])

    print("=== Xi IR Reverse Invariants ===")
    print("")

    check_no_raw_alloc(report)
    check_no_phase_coordinates(report)
    check_no_mode_flags(report)
    check_no_silent_defaults(report)
    check_op_name_sync(report)
    check_generated_sync(report)
    check_function_length(report)
    check_stats_slots(report)
    check_pass_registration(report)
    check_pass_table_coverage(report)
    check_test_coverage(report)

    print("")
    if not report.failed:
        print("=== ALL XI INVARIANTS PASSED ===")
        return 0
    print("=== XI INVARIANT FAILURES DETECTED ===")
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
