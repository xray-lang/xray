#!/usr/bin/env python3
"""Reject allocation-capable calls reachable from ownership record APIs.

The audit heap preallocates all storage during construction. Runtime tests also
check the observed allocation count, but this gate is the independent evidence:
it builds a conservative local call graph from both record entry points and
fails when a reachable function calls an allocator or a new, unreviewed
external function.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


PROJECT_DIR = Path(__file__).resolve().parent.parent
SOURCE = PROJECT_DIR / "src/runtime/ownership/xr_ownership_audit.c"
ROOTS = ("xr_ownership_audit_record", "xr_ownership_audit_record_lifecycle")

BLOCKED_CALLS = {
    "allocate_table",
    "malloc",
    "calloc",
    "realloc",
    "free",
    "aligned_alloc",
    "posix_memalign",
    "xr_malloc",
    "xr_calloc",
    "xr_realloc",
    "xr_free",
    "xr_malloc_aligned",
    "xr_free_aligned",
    "mi_malloc",
    "mi_calloc",
    "mi_realloc",
    "mi_free",
}

ALLOWED_EXTERNAL_CALLS = {
    "memcmp",
    "xr_runtime_domain_identity_valid",
}

ALLOWED_MACRO_CALLS = {
    "XR_AUDIT_TEST_AFTER_ENTER",
    "XR_AUDIT_TEST_ON_CONTENTION",
    "XR_MATERIALIZATION_MASK",
    "XR_OWN_STATE_MASK",
    "XR_SEMANTIC_DOMAIN_MASK",
}

KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof"}
FUNCTION_RE = re.compile(
    r"(?m)^[ \t]*(?:static[ \t]+)?(?:const[ \t]+)?"
    r"[A-Za-z_]\w*(?:[ \t*]+[A-Za-z_]\w*)*[ \t*]+"
    r"([A-Za-z_]\w*)[ \t]*\([^;{}]*\)[ \t\r\n]*\{"
)
CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")


def scrub(text: str) -> str:
    """Remove comments and literals while preserving byte positions/braces."""

    pattern = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                         re.DOTALL)
    return pattern.sub(lambda match: " " * len(match.group(0)), text)


def brace_depths(text: str) -> list[int]:
    depths: list[int] = [0] * (len(text) + 1)
    depth = 0
    for index, char in enumerate(text):
        depths[index] = depth
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth < 0:
                raise ValueError(f"unbalanced closing brace at byte {index}")
    depths[len(text)] = depth
    if depth != 0:
        raise ValueError("unbalanced braces at end of source")
    return depths


def functions(text: str) -> dict[str, str]:
    cleaned = scrub(text)
    depths = brace_depths(cleaned)
    result: dict[str, str] = {}
    for match in FUNCTION_RE.finditer(cleaned):
        opening = match.end() - 1
        if depths[opening] != 0:
            continue
        name = match.group(1)
        depth = 1
        cursor = opening + 1
        while cursor < len(cleaned) and depth:
            if cleaned[cursor] == "{":
                depth += 1
            elif cleaned[cursor] == "}":
                depth -= 1
            cursor += 1
        if depth != 0:
            raise ValueError(f"unterminated function body: {name}")
        result[name] = cleaned[opening + 1 : cursor - 1]
    return result


def main() -> int:
    try:
        bodies = functions(SOURCE.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"ownership record no-allocation gate: {exc}", file=sys.stderr)
        return 1

    missing = [root for root in ROOTS if root not in bodies]
    if missing:
        print(f"ownership record no-allocation gate: missing roots: {', '.join(missing)}",
              file=sys.stderr)
        return 1

    edges: dict[str, set[str]] = {}
    unknown: dict[str, set[str]] = {}
    for name, body in bodies.items():
        calls = {call for call in CALL_RE.findall(body) if call not in KEYWORDS}
        edges[name] = calls & bodies.keys()
        external = {
            call
            for call in calls - bodies.keys()
            if not call.startswith("atomic_")
            and call not in ALLOWED_MACRO_CALLS
            and call not in ALLOWED_EXTERNAL_CALLS
        }
        if external:
            unknown[name] = external

    reachable: set[str] = set()
    pending = list(ROOTS)
    while pending:
        name = pending.pop()
        if name in reachable:
            continue
        reachable.add(name)
        pending.extend(edges[name] - reachable)

    violations: list[str] = []
    for name in sorted(reachable):
        blocked = set(CALL_RE.findall(bodies[name])) & BLOCKED_CALLS
        if blocked:
            violations.append(f"{name}: allocator call(s) {', '.join(sorted(blocked))}")
        if name in unknown:
            violations.append(
                f"{name}: unreviewed external call(s) {', '.join(sorted(unknown[name]))}"
            )

    if violations:
        print("ownership record no-allocation gate: FAIL", file=sys.stderr)
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        return 1

    print(
        "ownership record no-allocation gate: PASS "
        f"({len(reachable)} reachable functions, {len(ROOTS)} record roots)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
