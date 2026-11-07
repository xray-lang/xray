#!/usr/bin/env python3
"""Current declaration provenance for the source-derived API inventory."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

import gen_api_inventory as api_inventory  # noqa: E402


class ApiInventoryEnumMetadataTest(unittest.TestCase):
    def test_inventory_uses_surviving_source_declarations(self) -> None:
        inventory = api_inventory.build_inventory(ROOT)
        self.assertFalse(inventory["execution_authority"])
        for entry in inventory["items"]:
            with self.subTest(symbol=entry["qualified"]):
                self.assertTrue((ROOT / entry["source"]).is_file(), entry["source"])
                self.assertFalse(entry["source"].startswith("src/frontend/analyzer/"))
                self.assertFalse(entry["source"].startswith("src/ir/"))
                self.assertFalse(entry["source"].startswith("src/runtime/value/"))

    def test_compiler_module_schema_keeps_xray_semantic_source(self) -> None:
        items = api_inventory.collect_stdlib_metadata(ROOT)
        coro_items = [entry for entry in items if entry["namespace"] == "Coro"]

        self.assertEqual(27, len(coro_items))
        self.assertEqual(
            {"stdlib/types/coro.xr"},
            {entry["source"] for entry in coro_items},
        )
        self.assertEqual(
            [],
            api_inventory.check_stdlib_metadata_source(ROOT, {"items": items}),
        )

    def test_compiler_module_schema_rejects_wrong_source_provenance(self) -> None:
        items = api_inventory.collect_stdlib_metadata(ROOT)
        wrong_items = [dict(entry) for entry in items]
        stats = next(entry for entry in wrong_items if entry["name"] == "CoroStats")
        stats["source"] = "stdlib/defs/core.def"

        self.assertEqual(
            [
                "stdlib API symbol must use declared semantic source "
                "stdlib/types/coro.xr, not stdlib/defs/core.def: Coro.CoroStats"
            ],
            api_inventory.check_stdlib_metadata_source(ROOT, {"items": wrong_items}),
        )


if __name__ == "__main__":
    unittest.main()
