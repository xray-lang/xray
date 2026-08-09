#!/usr/bin/env python3
"""Fail-closed tests for stdlib arg_spec/argc manifest metadata.

The AOT direct-call emitter consumes one arg_spec character per fixed
argument, so a spec shorter than argc used to read past the string literal.
The manifest parser now rejects any fixed-arity entry whose spec length
differs from argc; these tests pin that gate and the repository manifest.
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "stdlibgen"))

from stdlibgen import parse_def_metadata, parse_defs  # noqa: E402


def write_def(root: Path, body: str) -> None:
    defs = root / "stdlib" / "defs"
    defs.mkdir(parents=True)
    (defs / "core.def").write_text(body, encoding="utf-8")


def sample_fn(argc_line: str, extra_lines: str = "") -> str:
    return (
        "module sample {\n"
        "  fn probe {\n"
        '    signature: "(a: int, b: int): int"\n'
        '    doc: "probe"\n'
        '    vm: "sample_probe"\n'
        f"    {argc_line}\n"
        f"{extra_lines}"
        "  }\n"
        "}\n"
    )


class ArgSpecManifestTests(unittest.TestCase):
    def test_repository_manifest_arg_specs_are_exact(self) -> None:
        for entry in parse_defs(ROOT):
            if entry.argc == "variadic":
                self.assertIn(entry.arg_spec, {"", "*"}, entry.symbol)
                continue
            if entry.aot_kind == "builtin":
                self.assertEqual("", entry.arg_spec, entry.symbol)
                continue
            self.assertEqual(int(entry.argc), len(entry.arg_spec), entry.symbol)
            self.assertTrue(set(entry.arg_spec) <= set("ipsv"), entry.symbol)

    def test_short_arg_spec_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-arg-spec.") as tmp:
            root = Path(tmp)
            write_def(root, sample_fn("argc: 2", '    arg_spec: "s"\n'))
            with self.assertRaisesRegex(SystemExit, "length 1 does not match argc 2"):
                parse_def_metadata(root)

    def test_empty_arg_spec_with_positive_argc_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-arg-spec.") as tmp:
            root = Path(tmp)
            write_def(root, sample_fn("argc: 2", '    arg_spec: ""\n'))
            with self.assertRaisesRegex(SystemExit, "length 0 does not match argc 2"):
                parse_def_metadata(root)

    def test_omitted_arg_spec_with_positive_argc_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-arg-spec.") as tmp:
            root = Path(tmp)
            write_def(root, sample_fn("argc: 2"))
            with self.assertRaisesRegex(SystemExit, "length 0 does not match argc 2"):
                parse_def_metadata(root)

    def test_long_arg_spec_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-arg-spec.") as tmp:
            root = Path(tmp)
            write_def(root, sample_fn("argc: 2", '    arg_spec: "svv"\n'))
            with self.assertRaisesRegex(SystemExit, "length 3 does not match argc 2"):
                parse_def_metadata(root)

    def test_unknown_arg_spec_character_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-arg-spec.") as tmp:
            root = Path(tmp)
            write_def(root, sample_fn("argc: 2", '    arg_spec: "sx"\n'))
            with self.assertRaisesRegex(SystemExit, "unsupported characters 'x'"):
                parse_def_metadata(root)

    def test_non_integer_argc_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-arg-spec.") as tmp:
            root = Path(tmp)
            write_def(root, sample_fn("argc: two", '    arg_spec: "vv"\n'))
            with self.assertRaisesRegex(SystemExit, "argc must be an integer or variadic"):
                parse_def_metadata(root)

    def test_builtin_rows_reject_arg_spec(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-arg-spec.") as tmp:
            root = Path(tmp)
            write_def(
                root,
                sample_fn(
                    "argc: 2",
                    '    arg_spec: "vv"\n'
                    '    aot: "builtin"\n'
                    "    aot_direct: true\n"
                    '    aot_kind: "builtin"\n',
                ),
            )
            with self.assertRaisesRegex(SystemExit, "do not consume arg_spec"):
                parse_def_metadata(root)

    def test_variadic_method_requires_star_spec(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-arg-spec.") as tmp:
            root = Path(tmp)
            write_def(
                root,
                sample_fn(
                    "argc: variadic",
                    '    aot: "xrt_sample_probe"\n    aot_direct: true\n',
                ),
            )
            with self.assertRaisesRegex(SystemExit, "variadic arg_spec must be "):
                parse_def_metadata(root)

    def test_variadic_vm_row_rejects_star_spec(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-arg-spec.") as tmp:
            root = Path(tmp)
            write_def(root, sample_fn("argc: variadic", '    arg_spec: "*"\n'))
            with self.assertRaisesRegex(SystemExit, "variadic arg_spec must be "):
                parse_def_metadata(root)

    def test_short_spec_entry_rejected_before_reaching_the_emitter_table(self) -> None:
        """The historical net.__copy shape: yieldable VM binding, argc 3, empty
        spec. It must be rejected at parse time so no generated table row can
        ever make the emitter walk past the spec's NUL terminator."""
        with tempfile.TemporaryDirectory(prefix="xray-arg-spec.") as tmp:
            root = Path(tmp)
            write_def(
                root,
                "module net {\n"
                "  fn __copy {\n"
                '    signature: "(src: NetConn, dst: NetConn, bufferSize?: int): int"\n'
                '    doc: "copy"\n'
                '    vm: "net_copy_yieldable"\n'
                '    vm_binding: "yieldable"\n'
                "    argc: 3\n"
                '    arg_spec: ""\n'
                "  }\n"
                "}\n",
            )
            with self.assertRaisesRegex(SystemExit, "length 0 does not match argc 3"):
                parse_def_metadata(root)


if __name__ == "__main__":
    unittest.main()
