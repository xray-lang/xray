#!/usr/bin/env python3
"""Ensure ownership inventory checks cannot silently omit the new IR owners."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> None:
    checker, scratch = map(Path, sys.argv[1:])
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="lir-inventory-", dir=scratch) as directory:
        root = Path(directory)
        for owner in ("lir", "hir"):
            target = root / "src" / owner / "xborrow.c"
            target.parent.mkdir(parents=True)
            target.write_text(
                "void bad(void) {\n"
                "    ctx->name = node->name;\n"
                "    Item *saved = &items[0];\n"
                "    grow(items);\n"
                "}\n", encoding="utf-8")
        command = [sys.executable, str(checker), "--root", str(root), "--json",
                   "--max-category", "AST_PTR_INTO_IR=0",
                   "--max-category", "PTR_ACROSS_GROWTH=0"]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        if result.returncode != 1:
            raise AssertionError(f"inventory accepted borrowed new-IR metadata: {result}")
        report = json.loads(result.stdout)
        for category in ("AST_PTR_INTO_IR", "PTR_ACROSS_GROWTH"):
            paths = {Path(hit["path"]).as_posix() for hit in report[category]}
            if paths != {"src/lir/xborrow.c", "src/hir/xborrow.c"}:
                raise AssertionError(f"{category} missing new owner coverage: {paths}")
    print("LIR and HIR ownership inventory rejects both borrow classes")


if __name__ == "__main__":
    main()
