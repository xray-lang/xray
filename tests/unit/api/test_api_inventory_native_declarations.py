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

    def test_fifteen_operations_have_exact_admission_contracts(self) -> None:
        members = [entry for entry in self.items if entry["kind"] != "type"]
        admitted = {entry["name"]: entry for entry in members if entry["xir_admitted"]}
        ordinary = ["allocation", "retain", "limit"]
        callback = ordinary + ["callback"]
        expected = (
            ("get", "ARRAY_GET", "read", "owned", ["bounds", "allocation", "retain", "limit"]),
            ("set", "ARRAY_SET", "ref", "unit", ["bounds", "allocation", "retain", "limit"]),
            ("push", "ARRAY_PUSH", "ref", "unit", ordinary),
            ("clear", "ARRAY_CLEAR", "ref", "unit", ordinary),
            ("contains", "ARRAY_CONTAINS", "read", "owned", ordinary),
            ("indexOf", "ARRAY_INDEX_OF", "read", "owned", ordinary),
            ("join", "ARRAY_JOIN", "read", "owned", ordinary),
            ("map", "ARRAY_MAP", "read", "owned", callback),
            ("filter", "ARRAY_FILTER", "read", "owned", callback),
            ("reduce", "ARRAY_REDUCE", "read", "owned", callback),
            ("forEach", "ARRAY_FOR_EACH", "read", "unit", callback),
            ("find", "ARRAY_FIND", "read", "owned", callback),
            ("findIndex", "ARRAY_FIND_INDEX", "read", "owned", callback),
            ("every", "ARRAY_EVERY", "read", "owned", callback),
            ("some", "ARRAY_SOME", "read", "owned", callback),
        )
        self.assertEqual(15, len(admitted))
        self.assertEqual({row[0] for row in expected}, set(admitted))
        for name, operation, receiver, ownership, failures in expected:
            with self.subTest(name=name):
                entry = admitted[name]
                self.assertEqual(operation, entry["operation"])
                self.assertEqual(receiver, entry["receiver"])
                self.assertEqual("may_heap", entry["allocation"])
                self.assertEqual(ownership, entry["ownership"])
                self.assertEqual(failures, entry["failures"])
        unadmitted = [entry for entry in members if not entry["xir_admitted"]]
        self.assertEqual(17, len(unadmitted))
        self.assertEqual({"withCapacity", "capacity", "ptr", "mutPtr", "pop", "shift", "unshift",
                          "reserve", "resize", "concat", "reverse", "sort", "fill", "toString",
                          "iterator", "entriesIterator", "entries"}, {entry["name"] for entry in unadmitted})
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

    def test_string_identity_contracts_and_member_completeness(self) -> None:
        path = ROOT / "stdlib/types/string.xr"
        source = path.read_text(encoding="utf-8")
        header, members = declarations.parse_source(source, self.prelude)
        entries = [entry for entry in inventory.collect_native_types(ROOT) if entry["namespace"] == "string"]
        self.assertEqual(len(entries), 20)
        declaration = next(entry for entry in entries if entry["kind"] == "type")
        self.assertEqual(declaration["signature"], "string")
        self.assertEqual(declaration["native_type_id"], 2)
        self.assertEqual(declaration["declaration_kind"], "struct")
        actual = {entry["name"]: entry for entry in entries if entry["kind"] != "type"}
        self.assertEqual(set(actual), {member.name for member in members})
        for index, member in enumerate(members, 1):
            entry = actual[member.name]
            self.assertEqual(entry["native_type_id"], 2)
            self.assertEqual(entry["native_member_id"], index)
            self.assertEqual(entry["line"], member.line)
            self.assertEqual(entry["column"], member.column)
            self.assertEqual(entry["source"], "stdlib/types/string.xr")
            self.assertEqual(entry["operation"], member.operation)
            self.assertEqual(entry["xir_admitted"], member.name in {"contains", "startsWith", "endsWith", "indexOf", "lastIndexOf"})
            if entry["xir_admitted"]:
                self.assertEqual(entry["allocation"], "no_heap")
                self.assertEqual(entry["failures"], ["bounds", "limit"] if member.name == "indexOf" else ["limit"] if member.name == "lastIndexOf" else [])
                self.assertEqual(entry["ownership"], "owned")
        with self.assertRaises(ValueError):
            inventory.collect_native_declaration(ROOT, path,
                source.replace("contains(search: string)", "contains(search: i64)"), "stdlib", "string")

    def test_native_identity_rejects_relocated_declarations(self) -> None:
        with self.assertRaises(ValueError):
            inventory.collect_native_declaration(ROOT, self.path.with_name("imposter.xr"),
                self.source, "stdlib", "array")
        string_path = ROOT / "stdlib/types/string.xr"
        with self.assertRaises(ValueError):
            inventory.collect_native_declaration(ROOT, string_path.with_name("imposter.xr"),
                string_path.read_text(encoding="utf-8"), "stdlib", "string")

    def test_collector_uses_shared_parser_and_rejects_invalid_admission(self) -> None:
        with patch.object(declarations, "parse_source", wraps=declarations.parse_source) as parse:
            inventory.collect_native_types(ROOT)
        self.assertEqual(parse.call_count, 2)
        parse.assert_any_call(self.source, self.prelude)
        parse.assert_any_call((ROOT / "stdlib/types/string.xr").read_text(encoding="utf-8"), self.prelude)
        for source in (self.source.replace("struct Array", "class Array"),
                       self.source.replace("get(index: i64)", "get(index: i32)"),
                       self.source.replace("ARRAY_PUSH", "UNKNOWN_OP")):
            with self.subTest(source=source[:80]), self.assertRaises(ValueError):
                inventory.collect_native_declaration(ROOT, self.path, source, "stdlib", "array")


if __name__ == "__main__":
    unittest.main()
