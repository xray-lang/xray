#!/usr/bin/env python3
"""API diff coverage for source-derived inventory reports."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

import gen_api_inventory as api_inventory  # noqa: E402


def inventory_with_signature(signature: str) -> dict[str, object]:
    return {
        "schema": 1,
        "items": [
            {
                "category": "stdlib",
                "namespace": "modes",
                "name": "update",
                "qualified": "modes.update",
                "kind": "function",
                "signature": signature,
                "summary": "",
                "source": "stdlib/modes/modes.xr",
                "line": 1,
                "doc_surface": "stdlib",
                "doc_module": "modes",
            }
        ],
    }


class ApiInventoryDiffTest(unittest.TestCase):
    def test_param_mode_signature_change_is_breaking(self) -> None:
        old_inventory = inventory_with_signature("(value: int): ()")
        new_inventory = inventory_with_signature("(value: ref int): ()")

        report = api_inventory.compare_api_inventories(old_inventory, new_inventory)

        self.assertEqual(1, report["counts"]["changes"])
        self.assertEqual(1, report["counts"]["breaking"])
        self.assertEqual(
            {
                "change": "signature_changed",
                "severity": "breaking",
                "category": "stdlib",
                "namespace": "modes",
                "name": "update",
                "qualified": "modes.update",
                "kind": "function",
                "old_signature": "(value: int): ()",
                "new_signature": "(value: ref int): ()",
                "reason": "API signature changed",
            },
            report["changes"][0],
        )

    def test_move_param_mode_signature_change_is_breaking(self) -> None:
        old_inventory = inventory_with_signature("(slot: ref int): ()")
        new_inventory = inventory_with_signature("(slot: move Buffer): ()")

        report = api_inventory.compare_api_inventories(old_inventory, new_inventory)

        self.assertEqual(1, report["counts"]["breaking"])
        self.assertEqual("signature_changed", report["changes"][0]["change"])
        self.assertEqual("(slot: ref int): ()", report["changes"][0]["old_signature"])
        self.assertEqual("(slot: move Buffer): ()", report["changes"][0]["new_signature"])


if __name__ == "__main__":
    unittest.main()
