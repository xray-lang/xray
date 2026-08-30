#!/usr/bin/env python3
"""Reject compiler-header dependencies reachable from runtime product headers."""

from __future__ import annotations

import argparse
import re
import tempfile
from collections import deque
from pathlib import Path


INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
FORBIDDEN_PREFIXES = (
    "src/analysis/",
    "src/aot/",
    "src/app/",
    "src/frontend/",
    "src/ir/",
    "src/toolchain/",
)


def relative(root: Path, path: Path) -> str:
    return path.relative_to(root).as_posix()


def resolve_include(root: Path, owner: Path, include: str,
                    by_name: dict[str, list[Path]]) -> list[Path]:
    candidates = (
        owner.parent / include,
        root / include,
        root / "src" / include,
        root / "include" / include,
        root / "stdlib" / include,
    )
    for candidate in candidates:
        try:
            resolved = candidate.resolve(strict=True)
            resolved.relative_to(root)
        except (FileNotFoundError, ValueError):
            continue
        if resolved.is_file():
            return [resolved]
    return by_name.get(Path(include).name, [])


def header_graph(root: Path) -> dict[Path, list[Path]]:
    headers = sorted((root / "include").glob("*.h"))
    headers.extend(sorted((root / "src").rglob("*.h")))
    headers.extend(sorted((root / "stdlib").rglob("*.h")))
    by_name: dict[str, list[Path]] = {}
    for header in headers:
        by_name.setdefault(header.name, []).append(header.resolve())
    graph: dict[Path, list[Path]] = {}
    for header in headers:
        edges: list[Path] = []
        text = header.read_text(encoding="utf-8", errors="strict")
        for include in INCLUDE_RE.findall(text):
            for target in resolve_include(root, header, include, by_name):
                if target.suffix == ".h":
                    edges.append(target)
        graph[header.resolve()] = sorted(set(edges))
    return graph


def runtime_roots(root: Path) -> list[Path]:
    roots = sorted((root / "include").glob("*.h"))
    for directory in ("src/runtime", "src/vm", "src/coro"):
        roots.extend(sorted((root / directory).rglob("*.h")))
    roots.append(root / "src/module/xproto_codec.h")
    return sorted({path.resolve() for path in roots if path.is_file()})


def find_forbidden_chains(root: Path,
                          graph: dict[Path, list[Path]]) -> list[str]:
    violations: set[str] = set()
    for start in runtime_roots(root):
        queue: deque[tuple[Path, tuple[Path, ...]]] = deque([(start, (start,))])
        visited = {start}
        while queue:
            current, chain = queue.popleft()
            for target in graph.get(current, []):
                target_name = relative(root, target)
                next_chain = (*chain, target)
                if target_name.startswith(FORBIDDEN_PREFIXES):
                    violations.add(
                        " -> ".join(relative(root, item) for item in next_chain)
                    )
                    continue
                if target not in visited:
                    visited.add(target)
                    queue.append((target, next_chain))
    return sorted(violations)


def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="xray-runtime-header-lint-") as directory:
        root = Path(directory).resolve(strict=True)
        (root / "include").mkdir()
        (root / "src/runtime").mkdir(parents=True)
        (root / "src/frontend").mkdir(parents=True)
        (root / "stdlib").mkdir()
        (root / "include/runtime.h").write_text(
            '#include "../src/runtime/entry.h"\n', encoding="utf-8"
        )
        (root / "src/runtime/entry.h").write_text(
            "#include <compiler_fixture.h>\n", encoding="utf-8"
        )
        (root / "src/frontend/compiler_fixture.h").write_text(
            "#pragma once\n", encoding="utf-8"
        )
        violations = find_forbidden_chains(root, header_graph(root))
    if not violations or not all(
        "src/frontend/compiler_fixture.h" in violation
        for violation in violations
    ):
        print("runtime header dependency lint self-test: FAIL")
        return 1
    print("runtime header dependency lint self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    root = args.root.resolve(strict=True)
    violations = find_forbidden_chains(root, header_graph(root))
    if violations:
        print("runtime header dependency lint: FAIL")
        for violation in violations:
            print(f"  - runtime/compiler dependency: {violation}")
        return 1
    print(f"runtime header dependency lint: PASS ({len(runtime_roots(root))} roots)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
