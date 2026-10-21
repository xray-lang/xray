#!/usr/bin/env python3
"""Keep new compiler layers inside the ownership scanner's real file walk."""

from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from check_meta_ownership import build_inventory


class CompilerLayers(unittest.TestCase):
    def test_lir_and_hir_detect_borrows_and_growth(self):
        with tempfile.TemporaryDirectory(prefix="xray-meta-scope-") as temporary:
            root = Path(temporary)
            for layer in ("lir", "hir"):
                source = root / "src" / layer / "fixture.c"
                source.parent.mkdir(parents=True)
                source.write_text("void build(void) {\n"
                                  "    result->name = node->name;\n"
                                  "    Item *borrow = &items[0];\n"
                                  "    array_push(&items, next);\n"
                                  "}\n", encoding="utf-8")
            inventory = build_inventory(root)
            for category in ("AST_PTR_INTO_IR", "PTR_ACROSS_GROWTH"):
                self.assertEqual({Path(hit.path).parts[1] for hit in inventory[category]}, {"lir", "hir"})
                self.assertEqual(len(inventory[category]), 2)


if __name__ == "__main__":
    unittest.main()
