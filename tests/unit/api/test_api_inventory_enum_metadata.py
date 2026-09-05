#!/usr/bin/env python3
"""Enum descriptor coverage for the source-derived API inventory."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

import gen_api_inventory as api_inventory  # noqa: E402


class ApiInventoryEnumMetadataTest(unittest.TestCase):
    def test_descriptor_types_and_properties_come_from_shared_definition(self) -> None:
        items = api_inventory.collect_enum_metadata_api(ROOT)
        by_key = {(entry["namespace"], entry["name"], entry["kind"]): entry for entry in items}

        self.assertEqual(
            "EnumVariant<E>",
            by_key[("EnumVariant", "EnumVariant", "type")]["signature"],
        )
        self.assertEqual(
            ": EnumPayloads<E>",
            by_key[("EnumVariant", "payloads", "property")]["signature"],
        )
        self.assertEqual(
            ": i64",
            by_key[("EnumPayloadField", "type", "property")]["signature"],
        )
        self.assertEqual(
            ": EnumVariants<E>",
            by_key[("enum", "variants", "static-property")]["signature"],
        )

    def test_descriptor_contracts_are_machine_readable(self) -> None:
        items = api_inventory.collect_enum_metadata_api(ROOT)
        ordinal = next(
            entry
            for entry in items
            if entry["namespace"] == "EnumVariant" and entry["name"] == "ordinal"
        )
        name = next(
            entry
            for entry in items
            if entry["namespace"] == "EnumVariant" and entry["name"] == "name"
        )

        self.assertEqual("all", ordinal["profile"])
        self.assertEqual("pure", ordinal["effect"])
        self.assertEqual("none", ordinal["allocation"])
        self.assertEqual("materializes-string", name["allocation"])

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
