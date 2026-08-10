#!/usr/bin/env python3
"""Fail-closed tests for native return-ownership manifest metadata."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "stdlibgen"))

from stdlibgen import (  # noqa: E402
    parse_def_metadata,
    parse_defs,
    parse_function_signature_shape,
    return_type_requires_ownership_contract,
)


class ReturnOwnershipManifestTests(unittest.TestCase):
    def test_nested_signature_shape_is_parsed_at_top_level(self) -> None:
        params, result = parse_function_signature_shape(
            "(callback: fn(Array<int>): string, value: int): Array<string>?"
        )
        self.assertEqual(2, len(params))
        self.assertEqual("Array<string>?", result)

    def test_reference_capable_shapes_require_contracts(self) -> None:
        for result in ("string", "Array<int>?", "Channel", "any", "(int, string)"):
            self.assertTrue(return_type_requires_ownership_contract(result), result)
        for result in ("()", "bool", "int?", "float", "Ptr<byte>?", "Slice<int>"):
            self.assertFalse(return_type_requires_ownership_contract(result), result)

    def test_repository_manifest_has_no_implicit_reference_returns(self) -> None:
        for entry in parse_defs(ROOT):
            _, result = parse_function_signature_shape(entry.signature)
            if return_type_requires_ownership_contract(result):
                self.assertTrue(entry.return_ownership or entry.semantic_intrinsic, entry.symbol)
            self.assertNotEqual("unknown", entry.return_ownership, entry.symbol)

    def test_missing_contract_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-return-ownership.") as tmp:
            root = Path(tmp)
            defs = root / "stdlib" / "defs"
            defs.mkdir(parents=True)
            (defs / "core.def").write_text(
                """module sample {
  fn make {
    signature: \"(): string\"
    doc: \"make\"
    vm: \"sample_make\"
    argc: 0
  }
}
""",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(SystemExit, "requires explicit return_ownership"):
                parse_def_metadata(root)

    def test_borrowed_parameter_index_is_validated(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-return-ownership.") as tmp:
            root = Path(tmp)
            defs = root / "stdlib" / "defs"
            defs.mkdir(parents=True)
            (defs / "core.def").write_text(
                """module sample {
  fn alias {
    signature: \"(value: string): string\"
    doc: \"alias\"
    vm: \"sample_alias\"
    argc: 1
    arg_spec: \"s\"
    return_ownership: \"borrowed_param:1\"
  }
}
""",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(SystemExit, "references missing parameter 1"):
                parse_def_metadata(root)

    def test_unknown_reference_contract_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-return-ownership.") as tmp:
            root = Path(tmp)
            defs = root / "stdlib" / "defs"
            defs.mkdir(parents=True)
            (defs / "core.def").write_text(
                """module sample {
  fn make {
    signature: "(): string"
    doc: "make"
    vm: "sample_make"
    argc: 0
    return_ownership: "unknown"
  }
}
""",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(SystemExit, "unsupported return ownership"):
                parse_def_metadata(root)

    def test_semantic_intrinsic_must_not_declare_generic_ownership(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-return-ownership.") as tmp:
            root = Path(tmp)
            defs = root / "stdlib" / "defs"
            defs.mkdir(parents=True)
            (defs / "core.def").write_text(
                """module sample {
  fn invoke {
    signature: "(callback: any): any"
    doc: "invoke"
    vm: "sample_invoke"
    argc: 1
    arg_spec: "v"
    semantic_intrinsic: true
    return_ownership: "fresh"
  }
}
""",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(SystemExit, "must specialize return ownership"):
                parse_def_metadata(root)

    def test_semantic_intrinsic_must_be_boolean(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-return-ownership.") as tmp:
            root = Path(tmp)
            defs = root / "stdlib" / "defs"
            defs.mkdir(parents=True)
            (defs / "core.def").write_text(
                """module sample {
  fn invoke {
    signature: "(callback: any): any"
    doc: "invoke"
    vm: "sample_invoke"
    argc: 1
    arg_spec: "v"
    semantic_intrinsic: "false"
  }
}
""",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(SystemExit, "semantic_intrinsic must be a boolean"):
                parse_def_metadata(root)


if __name__ == "__main__":
    unittest.main()
