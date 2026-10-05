#!/usr/bin/env python3
"""Keep declaration predicates source-backed and distinct from interfaces."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

import gen_api_inventory as inventory  # noqa: E402


class ConstraintPredicateInventoryTest(unittest.TestCase):
    def test_equal_has_real_declaration_span_and_identity(self) -> None:
        rows = inventory.collect_constraint_predicates(ROOT)
        expected = (
            ("Equal", "VALUE_EQUAL"),
            ("AtomicValue", "ATOMIC_VALUE"),
            ("AtomicNumber", "ATOMIC_NUMBER"),
            ("AtomicBoolean", "ATOMIC_BOOLEAN"),
        )
        self.assertEqual([name for name, _ in expected], [row["name"] for row in rows])
        self.assertEqual(4, len(rows))
        interfaces = inventory.collect_interfaces(ROOT)
        lines = (ROOT / "stdlib/prelude/builtin_symbols.def").read_text(
            encoding="utf-8").splitlines()
        for row, (name, identity) in zip(rows, expected):
            with self.subTest(name=name):
                self.assertEqual(name, row["name"])
                self.assertEqual(name, row["qualified"])
                self.assertEqual(name, row["signature"])
                self.assertEqual("constraint-predicate", row["category"])
                self.assertEqual("predicate", row["kind"])
                self.assertEqual(0, row["arity"])
                self.assertEqual(identity, row["predicate_identity"])
                self.assertEqual("stdlib/prelude/builtin_symbols.def", row["source"])
                self.assertGreater(row["line"], 0)
                self.assertLessEqual(row["line"], len(lines))
                self.assertEqual(f'XR_BUILTIN_PREDICATE("{name}", 0, {identity})',
                                 lines[row["line"] - 1])
                self.assertFalse(any(entry["name"] == name for entry in interfaces))

    def test_complete_inventory_includes_the_predicate(self) -> None:
        with patch("subprocess.run", side_effect=AssertionError("inventory must read source")):
            data = inventory.build_inventory(ROOT)
        rows = [entry for entry in data["items"]
                if entry["category"] == "constraint-predicate"]
        expected_order = ["AtomicBoolean", "AtomicNumber", "AtomicValue", "Equal"]
        self.assertEqual(expected_order, [row["name"] for row in rows])
        collected = {row["name"]: row for row in inventory.collect_constraint_predicates(ROOT)}
        self.assertEqual(set(expected_order), set(collected))
        self.assertEqual([collected[name] for name in expected_order], rows)
        self.assertEqual(4, len(rows))

    def test_private_provider_inventory_keeps_source_denominator(self) -> None:
        data = inventory.build_inventory(ROOT)
        private = [row for row in data["items"] if row.get("internal")]
        expected = {"crypto": 2, "io": 34, "math": 28, "mem": 18,
                    "net": 33, "os": 21, "regex": 2, "runtime": 5,
                    "sys": 16, "time": 5}
        self.assertEqual(164, len(private))
        self.assertEqual(expected, {module: sum(row["namespace"] == module for row in private)
                                    for module in sorted(expected)})
        lines = (ROOT / "stdlib/defs/core.def").read_text(encoding="utf-8").splitlines()
        for row in private:
            self.assertEqual("stdlib/defs/core.def", row["source"])
            self.assertGreater(row["line"], 1)
            self.assertLessEqual(row["line"], len(lines))
            owner = row["name"].split(".", 1)[0]
            self.assertIn(owner, lines[row["line"] - 1])
            self.assertEqual("", row["doc_surface"])
            self.assertEqual("", row["doc_module"])
        self.assertEqual("source-declarations", data["scope"])
        self.assertIs(False, data["execution_authority"])
        self.assertFalse(any(row["source"] == "xray builtin-dump" for row in data["items"]))

    def test_registry_changes_do_not_leave_phantom_or_interface_rows(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "stdlib/prelude/builtin_symbols.def"
            path.parent.mkdir(parents=True)
            header = "#define XR_BUILTIN_PREDICATE(name, arity, identity)\n"
            path.write_text(header, encoding="utf-8")
            self.assertEqual([], inventory.collect_constraint_predicates(root))
            path.write_text(header + 'XR_BUILTIN_INTERFACE("Equal", 0)\n',
                            encoding="utf-8")
            self.assertEqual([], inventory.collect_constraint_predicates(root))
            path.write_text(header + 'XR_BUILTIN_PREDICATE("Witness", 1, VALUE_EQUAL)\n',
                            encoding="utf-8")
            rows = inventory.collect_constraint_predicates(root)
            self.assertEqual(1, len(rows))
            self.assertEqual("Witness", rows[0]["name"])
            self.assertEqual("Witness<T>", rows[0]["signature"])
            self.assertEqual(2, rows[0]["line"])
            self.assertEqual("VALUE_EQUAL", rows[0]["predicate_identity"])


if __name__ == "__main__":
    unittest.main()
