#!/usr/bin/env python3
"""Task 220 P2 — scan for removed Xray surface forms (surface-drift gate).

The language surface is free to drift ("no backward compatibility"), but that
freedom rots verification assets: old test cases, docs, and stdlib snippets
keep using forms that have since been deleted. This scanner maintains a regex
list of REMOVED surface forms and flags any live occurrence, turning "old code
rots" from passive discovery into an active zero (see §4.2 of
xray-docs/tasks/220-semantic-contract-freeze-and-verification-asset-governance-cn.md).

By default this is an inventory tool: it prints classified hits and exits 0
(mirroring scripts/check_error_effect_convergence.py). Pass ``--fail-on-hit``
(used by the ``surface_drift_scan`` CTest) to fail closed on any hit.

Scan targets: ``tests/``, ``stdlib/``, ``docs/knowledge/`` (in-repo) and, when
present, ``xray-docs/knowledge/`` (cross-repo, read-only). Negative/rejection
fixtures, fuzzer corpus, and fixed legacy-oracle probes legitimately embed
removed forms; they are excluded (see :func:`is_excluded`) because cleaning
them would break the tests or historical compiler input they preserve.

The removed-form list grows with every surface-deletion task.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path


@dataclass(frozen=True)
class RemovedForm:
    category: str
    description: str
    source_task: str
    pattern: "re.Pattern[str]"


# One entry per removed surface form. Append (never rewrite history) as further
# surface-deletion tasks land; keep patterns tight enough to avoid matching the
# current replacement surface.
REMOVED_FORMS: tuple[RemovedForm, ...] = (
    RemovedForm(
        "LET_BINDING",
        "`let` binding removed; use `var` / `const`",
        "161",
        re.compile(r"\blet\s+\w+\s*="),
    ),
    RemovedForm(
        "MEM_FREE_CALL",
        "mem.free(...) removed; a Buffer is RAII-managed and released on drop",
        "157",
        re.compile(r"\bmem\.free\s*\("),
    ),
    RemovedForm(
        "LOAD_LE_UNCHECKED",
        ".loadLEUnchecked removed unchecked little-endian load",
        "173",
        re.compile(r"\.loadLEUnchecked\b"),
    ),
    RemovedForm(
        "STORE_LE_UNCHECKED",
        ".storeLEUnchecked removed unchecked little-endian store",
        "173",
        re.compile(r"\.storeLEUnchecked\b"),
    ),
    RemovedForm(
        "BUFFER_DIRECT_INDEX",
        "direct [] index on a mem.alloc* Buffer; use .asBytes()/.asMutBytes()",
        "157",
        re.compile(r"\bmem\.alloc\w*\s*\([^()]*\)\s*\["),
    ),
)

CATEGORIES: tuple[str, ...] = tuple(form.category for form in REMOVED_FORMS)

# In-repo scan roots (relative to --root).
IN_REPO_SCAN_DIRS = ("tests", "stdlib", "docs/knowledge")

TEXT_SUFFIXES = (
    ".xr",
    ".xrd",
    ".md",
    ".toml",
    ".json",
    ".def",
    ".c",
    ".h",
    ".inc",
    ".inc.c",
    ".py",
    ".sh",
    ".yml",
    ".yaml",
    ".expect",
    ".expected",
)

SKIP_DIR_NAMES = {
    ".git",
    ".mypy_cache",
    "__pycache__",
    "build",
    "build-fuzz",
    "cmake-build-debug",
    "cmake-build-release",
    "node_modules",
}

# Negative-test and adversarial-input locations that legitimately embed removed
# forms: fuzz corpus stresses the parser with legacy syntax; compile_errors and
# */negative/* assert that removed forms are rejected.
NEGATIVE_PATH_SEGMENTS = {"fuzz", "compile_errors", "negative"}
# Fixture naming conventions for "this form is gone" assertions.
NEGATIVE_STEM_SUFFIXES = ("_removed", "_rejected")


@dataclass(frozen=True)
class Hit:
    category: str
    path: str
    line: int
    text: str


def is_excluded(path: Path) -> bool:
    """True for fixtures that intentionally preserve removed surface forms."""
    parts = path.parts
    if any(part in SKIP_DIR_NAMES for part in parts):
        return True
    if any(part in NEGATIVE_PATH_SEGMENTS for part in parts):
        return True
    # Legacy-oracle probes are compiled by the historical toolchain recorded in
    # contract.toml. Their removed syntax is evidence, not live source drift.
    if (
        len(parts) >= 6
        and parts[-6] == "tests"
        and parts[-5] == "stdlib"
        and parts[-4] == "contracts"
        and parts[-2] == "probes"
        and parts[-1] == "legacy.xr"
    ):
        return True
    # Logical stem, ignoring compound suffixes like `.xr.expected`.
    stem = path.name.split(".", 1)[0]
    if stem.endswith(NEGATIVE_STEM_SUFFIXES):
        return True
    return False


def display_path(path: Path, root: Path) -> str:
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


def compute_scan_roots(root: Path, extra_knowledge: list[str], include_external: bool) -> list[Path]:
    roots: list[Path] = [root / rel for rel in IN_REPO_SCAN_DIRS]
    if include_external:
        # Auto-detect a sibling xray-docs/knowledge checkout. Handles both a
        # direct sibling of the repo and the roadmap-worktree nesting layout.
        for candidate in (
            root / ".." / "xray-docs" / "knowledge",
            root / ".." / ".." / "xray-docs" / "knowledge",
        ):
            if candidate.is_dir():
                roots.append(candidate)
    roots.extend(Path(k) for k in extra_knowledge)

    seen: set[Path] = set()
    unique: list[Path] = []
    for candidate in roots:
        resolved = candidate.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        unique.append(candidate)
    return unique


def iter_text_files(scan_roots: list[Path]):
    seen: set[Path] = set()
    for base in scan_roots:
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file():
                continue
            if not any(str(path).endswith(suffix) for suffix in TEXT_SUFFIXES):
                continue
            if is_excluded(path):
                continue
            resolved = path.resolve()
            if resolved in seen:
                continue
            seen.add(resolved)
            yield path


def scan_file(root: Path, path: Path) -> list[Hit]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (UnicodeDecodeError, OSError):
        return []
    rel_path = display_path(path, root)
    hits: list[Hit] = []
    for lineno, line in enumerate(lines, 1):
        for form in REMOVED_FORMS:
            if form.pattern.search(line):
                hits.append(Hit(form.category, rel_path, lineno, line.strip()))
    return hits


def build_inventory(root: Path, scan_roots: list[Path]) -> dict[str, list[Hit]]:
    by_category: dict[str, list[Hit]] = defaultdict(list)
    for category in CATEGORIES:
        by_category[category]
    for path in iter_text_files(scan_roots):
        for hit in scan_file(root, path):
            by_category[hit.category].append(hit)
    return {category: by_category[category] for category in CATEGORIES}


def print_text_inventory(inventory: dict[str, list[Hit]], max_per_category: int) -> None:
    print("Task 220 removed-surface-form drift inventory")
    total = 0
    for category, hits in inventory.items():
        total += len(hits)
        print(f"{category}: {len(hits)}")
        shown = hits if max_per_category <= 0 else hits[:max_per_category]
        for hit in shown:
            print(f"  {hit.path}:{hit.line}: {hit.text}")
        if max_per_category > 0 and len(hits) > max_per_category:
            print(f"  ... {len(hits) - max_per_category} more")
    print(f"TOTAL: {total}")


def to_json(inventory: dict[str, list[Hit]]) -> str:
    counts = {category: len(hits) for category, hits in inventory.items()}
    return json.dumps(
        {
            "forms": {
                form.category: {
                    "description": form.description,
                    "source_task": form.source_task,
                    "pattern": form.pattern.pattern,
                }
                for form in REMOVED_FORMS
            },
            "counts": counts,
            "total": sum(counts.values()),
            "hits": {
                category: [asdict(hit) for hit in hits]
                for category, hits in inventory.items()
            },
        },
        indent=2,
        sort_keys=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--json", action="store_true", help="print machine-readable JSON")
    parser.add_argument(
        "--max-per-category",
        type=int,
        default=20,
        help="text output limit per category; 0 prints all hits",
    )
    parser.add_argument(
        "--fail-on-hit",
        action="store_true",
        help="exit non-zero if any removed surface form is found (fail-closed)",
    )
    parser.add_argument(
        "--knowledge-dir",
        action="append",
        default=[],
        metavar="DIR",
        help="additional knowledge root to scan (repeatable)",
    )
    parser.add_argument(
        "--no-external",
        action="store_true",
        help="skip auto-detection of a sibling xray-docs/knowledge checkout",
    )
    args = parser.parse_args()

    root = Path(args.root).resolve()
    scan_roots = compute_scan_roots(root, args.knowledge_dir, include_external=not args.no_external)
    inventory = build_inventory(root, scan_roots)

    if args.json:
        print(to_json(inventory))
    else:
        print_text_inventory(inventory, args.max_per_category)

    if args.fail_on_hit:
        total = sum(len(hits) for hits in inventory.values())
        if total:
            print(
                f"task-220 surface-drift gate failed: {total} removed-form hit(s)",
                file=sys.stderr,
            )
            for category, hits in inventory.items():
                if hits:
                    print(f"  {category}: {len(hits)}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
