"""Keep scheduler diagnostic schemas nominal and declaration ordered."""

from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "stdlibgen"))
sys.path.insert(0, str(ROOT / "scripts"))

import stdlibgen
import gen_api_inventory
import gen_stdlib_types


class CoroNominalSchemaTests(unittest.TestCase):
    def parse_schema(self, text: str):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "stdlib/types/sample.xr"
            source.parent.mkdir(parents=True)
            source.write_text(text, encoding="utf-8")
            return stdlibgen.parse_compiler_module_schemas(root)

    def test_nominal_fields_keep_declared_order_and_readonly_flags(self):
        schemas = self.parse_schema("""// @compiler-module Example
struct Snapshot {
    const z: i64
    a: string
}
""")
        snapshot = schemas[0].nominal_structs[0]
        self.assertEqual(snapshot.symbol, "Example.Snapshot")
        self.assertEqual([field.name for field in snapshot.fields], ["z", "a"])
        self.assertEqual([field.is_const for field in snapshot.fields], [True, False])
        self.assertEqual(snapshot.semantic_source, "stdlib/types/sample.xr")
        self.assertEqual(snapshot.semantic_authority, "xray_schema")

    def test_structural_alias_is_rejected_in_schema(self):
        with self.assertRaisesRegex(SystemExit, "structural object aliases were removed"):
            self.parse_schema("""// @compiler-module Example
type Snapshot = { const id: i64 }
""")

    def test_duplicate_fields_and_comma_separators_are_rejected(self):
        for fields, reason in (
            ("const id: i64\nconst id: i64", "duplicate struct field"),
            ("const id: i64,", "separated by newlines"),
            ("id: i64, name: string", "requires its own declaration"),
        ):
            with self.subTest(fields=fields):
                with self.assertRaisesRegex(SystemExit, reason):
                    self.parse_schema("// @compiler-module Example\nstruct Snapshot {\n" +
                                      fields + "\n}\n")

    def test_repository_coro_carriers_are_nominal(self):
        carriers = stdlibgen.parse_nominal_structs(ROOT)
        self.assertEqual([entry.symbol for entry in carriers], [
            "Coro.CoroStats", "Coro.CoroInfo", "Coro.CoroDeadlock",
        ])
        stats = carriers[0]
        self.assertEqual([field.name for field in stats.fields], [
            "active", "blocked", "ready", "total", "created",
        ])
        self.assertTrue(all(field.is_const for entry in carriers for field in entry.fields))
        self.assertFalse(hasattr(stdlibgen, "parse_object_shapes"))

    def test_generated_c_keeps_module_and_declaration_identity(self):
        header = stdlibgen.emit_defs_header(*stdlibgen.parse_def_metadata(ROOT))
        self.assertIn("XrStdlibNominalStructDefEntry", header)
        self.assertIn('"Coro", "CoroStats"', header)
        self.assertIn("xr_stdlib_nominal_struct_fields_Coro_CoroStats", header)
        self.assertNotIn("XrStdlibObjectShapeDefEntry", header)
        self.assertNotIn("bool exact;", header)

    def test_tooling_exposes_struct_declarations(self):
        rows = gen_api_inventory.collect_stdlib_metadata(ROOT)
        stats = next(row for row in rows if row["qualified"] == "Coro.CoroStats")
        self.assertTrue(stats["signature"].startswith("struct CoroStats {"))
        self.assertEqual(stats["source"], "stdlib/types/coro.xr")
        modules = gen_stdlib_types.load_def_module_nominal_structs()
        self.assertEqual(modules["Coro"][0]["name"], "CoroStats")
        self.assertNotIn("exact", modules["Coro"][0])


if __name__ == "__main__":
    unittest.main()
