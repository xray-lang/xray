#!/usr/bin/env python3
"""Keep native Array inventory complete and bound to compiler admission metadata."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

import gen_api_inventory as inventory  # noqa: E402
import gen_native_declarations as declarations  # noqa: E402


class NativeDeclarationInventoryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.path = ROOT / "stdlib/types/array.xr"
        cls.source = cls.path.read_text(encoding="utf-8")
        cls.prelude = (ROOT / "stdlib/prelude/builtin_symbols.def").read_text(encoding="utf-8")
        cls.header, cls.members = declarations.parse_source(cls.source, cls.prelude)
        cls.items = [entry for entry in inventory.collect_native_types(ROOT)
                     if entry["namespace"] == "Array"]

    def test_exact_member_set_includes_ref_methods(self) -> None:
        expected = {
            "withCapacity", "capacity", "push", "get", "set", "ptr", "mutPtr",
            "pop", "shift", "unshift", "clear", "reserve", "resize", "concat",
            "indexOf", "contains", "join", "reverse", "sort", "map", "filter",
            "reduce", "forEach", "find", "findIndex", "every", "some", "fill",
            "toString", "iterator", "entriesIterator", "entries",
        }
        members = [entry for entry in self.items if entry["kind"] != "type"]
        self.assertEqual(32, len(members))
        self.assertEqual(expected, {entry["name"] for entry in members})
        declaration = next(entry for entry in self.items if entry["kind"] == "type")
        self.assertEqual("Array<T>", declaration["signature"])
        self.assertEqual("struct", declaration["declaration_kind"])

    def test_only_three_operations_are_admitted(self) -> None:
        members = [entry for entry in self.items if entry["kind"] != "type"]
        admitted = {entry["name"]: entry for entry in members if entry["xir_admitted"]}
        self.assertEqual({"get", "set", "push"}, set(admitted))
        for name, operation, receiver, ownership, failures in (
            ("get", "ARRAY_GET", "read", "owned", ["bounds", "allocation", "retain", "limit"]),
            ("set", "ARRAY_SET", "ref", "unit", ["bounds", "allocation", "retain", "limit"]),
            ("push", "ARRAY_PUSH", "ref", "unit", ["allocation", "retain", "limit"]),
        ):
            with self.subTest(name=name):
                entry = admitted[name]
                self.assertEqual(operation, entry["operation"])
                self.assertEqual(receiver, entry["receiver"])
                self.assertEqual("may_heap", entry["allocation"])
                self.assertEqual(ownership, entry["ownership"])
                self.assertEqual(failures, entry["failures"])
        unadmitted = [entry for entry in members if not entry["xir_admitted"]]
        self.assertEqual(29, len(unadmitted))
        self.assertTrue(all(entry["operation"] == "NONE" for entry in unadmitted))
        self.assertTrue(all(entry["allocation"] == "unknown" for entry in unadmitted))
        self.assertTrue(all(entry["ownership"] == "unknown" for entry in unadmitted))

    def test_source_spans_and_member_identities_match_shared_schema(self) -> None:
        actual = {entry["name"]: entry for entry in self.items if entry["kind"] != "type"}
        for member_id, member in enumerate(self.members, 1):
            with self.subTest(name=member.name):
                entry = actual[member.name]
                self.assertEqual(1, entry["native_type_id"])
                self.assertEqual(member_id, entry["native_member_id"])
                self.assertEqual(member.line, entry["line"])
                self.assertEqual(member.column, entry["column"])
                self.assertEqual("stdlib/types/array.xr", entry["source"])
                self.assertEqual(member.receiver.lower(), entry["receiver"])
        self.assertEqual("(index: i64, value: T): ()", actual["set"]["signature"])
        self.assertEqual("(fn: fn(item: T, index: i64) -> U): Array<U>", actual["map"]["signature"])
        self.assertEqual("(value: T, start?: i64, end?: i64): Array<T>", actual["fill"]["signature"])

    def test_collector_uses_shared_parser_and_rejects_invalid_admission(self) -> None:
        with patch.object(declarations, "parse_source", wraps=declarations.parse_source) as parse:
            inventory.collect_native_types(ROOT)
        parse.assert_called_once_with(self.source, self.prelude)
        for source in (self.source.replace("struct Array", "class Array"),
                       self.source.replace("get(index: i64)", "get(index: i32)"),
                       self.source.replace("ARRAY_PUSH", "UNKNOWN_OP")):
            with self.subTest(source=source[:80]), self.assertRaises(ValueError):
                inventory.collect_native_array(ROOT, self.path, source, "stdlib", "array")


if __name__ == "__main__":
    unittest.main()
