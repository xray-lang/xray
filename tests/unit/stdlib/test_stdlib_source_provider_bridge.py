#!/usr/bin/env python3
"""Fail-closed contracts for stdlib source/provider bridge metadata."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "stdlibgen"))

from stdlibgen import parse_def_metadata  # noqa: E402


def write_fixture(root: Path, definition: str, source: str, module: str = "sample") -> None:
    defs = root / "stdlib" / "defs"
    defs.mkdir(parents=True)
    (defs / "core.def").write_text(definition, encoding="utf-8")
    source_dir = root / "stdlib" / module
    source_dir.mkdir(parents=True)
    (source_dir / f"{module}.xr").write_text(source, encoding="utf-8")


def provider_def(
    function_visibility: str = "internal",
    signature: str = "(value: Box): i64",
    argc: int = 1,
    arg_spec: str = "v",
) -> str:
    return (
        "module sample {\n"
        "  native_class __BoxStorage {\n"
        '    core_slot: "boxStorageClass"\n'
        '    native_body_expr: "&box_body"\n'
        '    visibility: "internal"\n'
        '    source_wrapper: "Box"\n'
        '    source_storage_field: "_storage"\n'
        "  }\n"
        "  fn __probe {\n"
        f'    signature: "{signature}"\n'
        '    doc: "probe"\n'
        '    vm: "sample_probe"\n'
        f"    argc: {argc}\n"
        f'    arg_spec: "{arg_spec}"\n'
        f'    visibility: "{function_visibility}"\n'
        "  }\n"
        "}\n"
    )


GOOD_SOURCE = """export final class Box {
    private _storage: __BoxStorage

    private constructor(storage: __BoxStorage) { this._storage = storage }
}
"""


class SourceProviderBridgeTests(unittest.TestCase):
    def parse(self, definition: str = "", source: str = GOOD_SOURCE) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-source-provider.") as tmp:
            root = Path(tmp)
            write_fixture(root, definition or provider_def(), source)
            parse_def_metadata(root)

    def test_valid_private_same_module_bridge(self) -> None:
        self.parse()

    def test_wrapper_may_have_additional_source_fields(self) -> None:
        self.parse(
            source="""export final class Box {
    private _tag: i64
    private _storage: __BoxStorage
    private _cached: bool
}
"""
        )

    def test_public_native_leaf_is_rejected(self) -> None:
        with self.assertRaisesRegex(SystemExit, "stdlib-internal only"):
            self.parse(provider_def(function_visibility="public"))

    def test_cross_module_wrapper_reference_is_rejected(self) -> None:
        definition = provider_def(signature="(value: i64): i64") + (
            "module other {\n"
            "  fn __probeOther {\n"
            '    signature: "(value: Box): i64"\n'
            '    doc: "probe"\n'
            '    vm: "other_probe"\n'
            "    argc: 1\n"
            '    arg_spec: "v"\n'
            '    visibility: "internal"\n'
            "  }\n"
            "}\n"
        )
        with tempfile.TemporaryDirectory(prefix="xray-source-provider.") as tmp:
            root = Path(tmp)
            write_fixture(root, definition, GOOD_SOURCE)
            with self.assertRaisesRegex(SystemExit, "belongs to module sample, not other"):
                parse_def_metadata(root)

    def test_missing_storage_field_is_rejected(self) -> None:
        with self.assertRaisesRegex(SystemExit, "exactly one _storage storage field"):
            self.parse(source="export final class Box {}\n")

    def test_duplicate_storage_field_is_rejected(self) -> None:
        with self.assertRaisesRegex(SystemExit, "exactly one _storage storage field"):
            self.parse(
                source="""export final class Box {
    private _storage: __BoxStorage
    private _storage: __BoxStorage
}
"""
            )

    def test_wrong_storage_type_is_rejected(self) -> None:
        with self.assertRaisesRegex(SystemExit, "exact storage type __BoxStorage"):
            self.parse(source="export final class Box {\n    private _storage: i64\n}\n")

    def test_non_private_storage_field_is_rejected(self) -> None:
        with self.assertRaisesRegex(SystemExit, "must be private"):
            self.parse(source="export final class Box {\n    _storage: __BoxStorage\n}\n")

    def test_static_storage_field_is_rejected(self) -> None:
        with self.assertRaisesRegex(SystemExit, "must be an instance field"):
            self.parse(
                source="export final class Box {\n    private static _storage: __BoxStorage\n}\n"
            )

    def test_wrapper_return_is_rejected(self) -> None:
        with self.assertRaisesRegex(SystemExit, "valid only in parameters"):
            definition = provider_def(signature="(value: i64): Box")
            before, marker, after = definition.rpartition('    visibility: "internal"\n')
            definition = before + marker + '    return_ownership: "fresh"\n' + after
            self.parse(definition)

    def test_nullable_wrapper_parameter_is_rejected(self) -> None:
        with self.assertRaisesRegex(SystemExit, "exact non-nullable nominal parameter type"):
            self.parse(provider_def(signature="(value: Box?): i64"))

    def test_nested_wrapper_parameter_is_rejected(self) -> None:
        with self.assertRaisesRegex(SystemExit, "exact non-nullable nominal parameter type"):
            self.parse(provider_def(signature="(value: Array<Box>): i64"))

    def test_bridge_has_no_64_parameter_limit(self) -> None:
        params = [f"value{index}: i64" for index in range(64)] + ["wrapper: Box"]
        self.parse(
            provider_def(
                signature=f"({', '.join(params)}): i64",
                argc=len(params),
                arg_spec="v" * len(params),
            )
        )


if __name__ == "__main__":
    unittest.main()
