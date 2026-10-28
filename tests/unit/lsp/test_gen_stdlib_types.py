"""Check exported pure-source function signatures used by LSP metadata."""

import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "gen_stdlib_types", ROOT / "scripts/gen_stdlib_types.py")
GENERATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATOR)


class PureFunctionSignatures(unittest.TestCase):
    def test_real_throwing_exports(self):
        expected = {
            "encoding": {
                "hexDecode": "(hex: string) throws(HexError): Array<u8>",
                "utf16Decode": "(data: Slice<u8>, options?: Utf16DecodeOptions) "
                               "throws(Utf16Error): string",
            },
            "base64": {
                "decode": "(text: string, options?: Base64DecodeOptions) "
                          "throws(Base64Error): Array<u8>",
            },
            "parallel": {
                "forEach": "(range: Range, body: fn(i64), options?: Options) "
                           "throws(ParallelError): ()",
                "map": "(range: Range, body: fn(i64) -> T, options?: Options) "
                       "throws(ParallelError): Array<T>",
                "mapInto": "(range: Range, output: ref Array<T>, body: fn(i64) -> T, "
                           "options?: Options) throws(ParallelError): ()",
                "reduce": "(range: Range, initial: A, body: fn(i64) -> A, "
                          "combine: fn(A, A) -> A, options?: Options) "
                          "throws(ParallelError): A",
            },
        }
        for module, signatures in expected.items():
            data = GENERATOR.scan_pure_xray_module(
                ROOT / "stdlib" / module / (module + ".xr"), module)
            actual = {method["name"]: method["signature"] for method in data["methods"]}
            for name, signature in signatures.items():
                with self.subTest(module=module, name=name):
                    self.assertEqual(actual.get(name), signature)

    def test_private_functions_do_not_leak(self):
        source = """export fn plain(value: i64) -> i64 { return value }
export fn fallible(data: Slice<u8>) throws(MyError) -> i64 { return 1 }
fn hidden(data: Slice<u8>) throws(MyError) -> i64 { return 2 }
"""
        with tempfile.TemporaryDirectory(prefix="lsp-pure-export-") as directory:
            path = Path(directory) / "example.xr"
            path.write_text(source, encoding="utf-8")
            data = GENERATOR.scan_pure_xray_module(path, "example")
        actual = {method["name"]: method["signature"] for method in data["methods"]}
        self.assertEqual(actual, {
            "plain": "(value: i64): i64",
            "fallible": "(data: Slice<u8>) throws(MyError): i64",
        })


if __name__ == "__main__":
    unittest.main()
