#!/usr/bin/env python3
"""Frontend architecture lint: layer direction and file-size caps.

The include rules encode the frontend's layering. Each says a directory must
not reach into another -- lexer stays below runtime, parser below analyzer,
format and codegen stay leaves. A violation is not a style problem: it is a
cycle in the layer graph that makes the offending pair impossible to build or
test separately.

The size caps are cohesion pressure. The oversize allowlist carries a per-file
ceiling rather than a blanket exemption, so an already-too-large file can still
only shrink.

Exit 0 when every rule holds, 1 otherwise.

Usage: check_frontend_arch.py
"""

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Sequence, Tuple

REPO_ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = Path("src")
FRONTEND = SRC_DIR / "frontend"

C_CAP = 3000
H_CAP = 800

# Files already over the cap when it was introduced. The value is that file's
# own ceiling, so it can shrink but never grow. Remove an entry once its file
# is back under C_CAP.
OVERSIZE_CAPS: Dict[str, int] = {
    "src/frontend/analyzer/xanalyzer_errorset.c": 4327,
    "src/frontend/analyzer/xanalyzer_visitor_stmt.c": 9210,
    "src/frontend/analyzer/xanalyzer_visitor.c": 7636,
    "src/frontend/analyzer/xanalyzer_visitor_call.c": 5358,
    "src/frontend/analyzer/xanalyzer_visitor_expr.c": 5190,
    "src/frontend/analyzer/xanalyzer_visitor_decl.c": 4772,
}


@dataclass(frozen=True)
class IncludeRule:
    label: str          # the "--- Rx: ... ---" header
    directory: str      # scanned, relative to src/
    forbidden: str      # regex fragment matched against the include target
    failure: str        # message when hits are found
    success: str        # message when clean


INCLUDE_RULES: Tuple[IncludeRule, ...] = (
    IncludeRule("R1: lexer/ -> runtime/", "frontend/lexer", r".*runtime/",
                "lexer/ includes runtime/:", "lexer/ does not include runtime/"),
    IncludeRule("R2: parser/ -> analyzer/", "frontend/parser", r".*analyzer/",
                "parser/ includes analyzer/:", "parser/ does not include analyzer/"),
    IncludeRule("R3: format/ -> analyzer/", "frontend/format", r".*analyzer/",
                "format/ includes analyzer/:", "format/ does not include analyzer/"),
    IncludeRule("R4: format/ -> include/xray*.h", "frontend/format",
                r".*include/xray.*\.h",
                "format/ includes a public xray header:",
                "format/ does not include public API headers"),
    IncludeRule("R5: frontend/** -> include/xray.h or include/xray_vm.h",
                "frontend", r".*include/xray(_vm)?\.h",
                "frontend/ includes a public xray header:",
                "frontend/** does not include public API headers"),
    IncludeRule("R6: analyzer/ -> codegen/", "frontend/analyzer", r".*codegen/",
                "analyzer/ includes codegen/:", "analyzer/ does not include codegen/"),
    IncludeRule("R7: codegen/ -> format/", "frontend/codegen", r".*format/",
                "codegen/ includes format/:", "codegen/ does not include format/"),
)

AST_FIELD = re.compile(r"^[^/]*\bcompile_type(_legacy)?\s*[;,\[]")

USE_COLOR = sys.stdout.isatty() and not os.environ.get("NO_COLOR")
RED = "\033[0;31m" if USE_COLOR else ""
GREEN = "\033[0;32m" if USE_COLOR else ""
YELLOW = "\033[1;33m" if USE_COLOR else ""
NC = "\033[0m" if USE_COLOR else ""


class Lint:
    def __init__(self) -> None:
        self.errors = 0

    def fail(self, message: str) -> None:
        print(f"  {RED}FAIL{NC}: {message}")
        self.errors += 1

    def ok(self, message: str) -> None:
        print(f"  {GREEN}PASS{NC}: {message}")

    def note(self, message: str) -> None:
        print(f"  {YELLOW}NOTE{NC}: {message}")

    def show(self, lines: Sequence[str]) -> None:
        for line in lines:
            print(f"      {line}")


def sources(root: Path, suffixes: Sequence[str]) -> List[Path]:
    if not root.is_dir():
        return []
    return sorted(p for p in root.rglob("*")
                  if p.is_file() and p.suffix in suffixes)


def check_include_rule(lint: Lint, rule: IncludeRule) -> None:
    print(f"--- {rule.label} ---")
    pattern = re.compile(r'#include\s+["<]' + rule.forbidden)
    hits: List[str] = []
    for path in sources(REPO_ROOT / SRC_DIR / rule.directory, (".c", ".h")):
        relative = path.relative_to(REPO_ROOT)
        for number, line in enumerate(
                path.read_text(encoding="utf-8").splitlines(),
                start=1):
            if pattern.search(line):
                hits.append(f"{relative}:{number}:{line}")
    if hits:
        lint.fail(rule.failure)
        lint.show(hits)
    else:
        lint.ok(rule.success)
    print("")


def check_ast_fields(lint: Lint) -> None:
    print("--- R8: AstNode has no inline semantic-state fields ---")
    # Field declarations only, not commentary: a declaration looks like
    # `... compile_type;` or `... *compile_type;`, and the anchor keeps
    # `// ... compile_type ...` comments from tripping it.
    header = REPO_ROOT / SRC_DIR / "frontend" / "parser" / "xast_nodes.h"
    hits: List[str] = []
    if header.is_file():
        for number, line in enumerate(
                header.read_text(encoding="utf-8").splitlines(),
                start=1):
            if AST_FIELD.search(line):
                hits.append(f"{number}:{line}")
    if hits:
        lint.fail("xast_nodes.h still declares compile_type / compile_type_legacy:")
        lint.show(hits)
    else:
        lint.ok("xast_nodes.h has no compile_type field declaration")
    print("")


def check_size_caps(lint: Lint) -> None:
    print(f"--- R9: frontend .c file size cap (≤ {C_CAP} lines) ---")
    allowed: List[str] = []
    oversize: List[str] = []
    for path in sources(REPO_ROOT / FRONTEND, (".c",)):
        relative = str(path.relative_to(REPO_ROOT))
        count = len(path.read_text(encoding="utf-8").splitlines())
        if count <= C_CAP:
            continue
        ceiling = OVERSIZE_CAPS.get(relative)
        if ceiling is not None and count <= ceiling:
            allowed.append(f"{count} {relative} (temporary cap {ceiling})")
        else:
            oversize.append(f"{count} {relative}")
    if allowed:
        lint.note("temporary capped exceptions:")
        lint.show(allowed)
    if oversize:
        lint.fail(f"frontend .c files over {C_CAP} lines:")
        lint.show(oversize)
    else:
        lint.ok("all non-exempt frontend .c files within size limit")
    print("")

    print(f"--- R10: frontend .h file size cap (≤ {H_CAP} lines) ---")
    oversize = []
    for path in sources(REPO_ROOT / FRONTEND, (".h",)):
        count = len(path.read_text(encoding="utf-8").splitlines())
        if count > H_CAP:
            oversize.append(f"{count} {path.relative_to(REPO_ROOT)}")
    if oversize:
        lint.fail(f"frontend .h files over {H_CAP} lines:")
        lint.show(oversize)
    else:
        lint.ok("all frontend .h files within size limit")
    print("")


def main(argv: List[str]) -> int:
    os.chdir(REPO_ROOT)
    lint = Lint()

    print("============================================")
    print("  xray frontend architecture lint")
    print("============================================")
    print("")

    for rule in INCLUDE_RULES:
        check_include_rule(lint, rule)
    check_ast_fields(lint)
    check_size_caps(lint)

    print("============================================")
    if lint.errors:
        print(f"{RED}Frontend arch lint FAILED ({lint.errors} violation(s)){NC}")
        return 1
    print(f"{GREEN}Frontend arch lint passed{NC}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
