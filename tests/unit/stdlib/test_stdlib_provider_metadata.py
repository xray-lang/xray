#!/usr/bin/env python3
"""Fail-closed tests for source-owned runtime-provider metadata."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "stdlibgen"))

from stdlibgen import parse_def_metadata, parse_defs  # noqa: E402


CLOCK_CONTRACT = "xray.runtime.provider.v1/clock"
CLOCK_OPERATIONS = {
    "xray.runtime.provider-operation.v1/clock/realtime-nanos",
    "xray.runtime.provider-operation.v1/clock/monotonic-nanos",
    "xray.runtime.provider-operation.v1/clock/process-cpu-nanos",
    "xray.runtime.provider-operation.v1/clock/utc-offset-minutes-at",
}


def provider_def(extra: str = "", signature: str = "(): i64", argc: int = 0) -> str:
    return (
        "module sample {\n"
        "  fn __probe {\n"
        f'    signature: "{signature}"\n'
        '    doc: "provider probe"\n'
        '    vm: "sample_probe"\n'
        f"    argc: {argc}\n"
        f'    arg_spec: "{"v" * argc}"\n'
        '    effect: "nothrow"\n'
        '    aot: "xrt_sample_probe"\n'
        "    aot_direct: true\n"
        '    aot_kind: "method"\n'
        '    ret: "value"\n'
        '    visibility: "internal"\n'
        f'    provider_contract: "{CLOCK_CONTRACT}"\n'
        '    provider_operation: '
        '"xray.runtime.provider-operation.v1/clock/realtime-nanos"\n'
        f"{extra}"
        "  }\n"
        "}\n"
    )


def write_def(root: Path, body: str) -> None:
    defs = root / "stdlib" / "defs"
    defs.mkdir(parents=True)
    (defs / "core.def").write_text(body, encoding="utf-8")


class ProviderMetadataTests(unittest.TestCase):
    def parse(self, body: str):
        with tempfile.TemporaryDirectory(prefix="xray-provider-metadata.") as tmp:
            root = Path(tmp)
            write_def(root, body)
            entries, *_ = parse_def_metadata(root)
            return entries

    def test_repository_clock_metadata_is_exact(self) -> None:
        entries = [entry for entry in parse_defs(ROOT) if entry.provider_contract]
        self.assertEqual(4, len(entries))
        self.assertEqual({CLOCK_CONTRACT}, {entry.provider_contract for entry in entries})
        self.assertEqual(CLOCK_OPERATIONS, {entry.provider_operation for entry in entries})
        self.assertTrue(all(entry.visibility == "internal" for entry in entries))
        self.assertTrue(all(entry.effect == "nothrow" for entry in entries))

    def test_nullary_and_unary_i64_leaves_are_accepted(self) -> None:
        self.assertEqual(CLOCK_CONTRACT, self.parse(provider_def())[0].provider_contract)
        unary = provider_def(signature="(seconds: i64): i64", argc=1)
        self.assertEqual(1, len(self.parse(unary)))

    def test_contract_and_operation_must_be_paired(self) -> None:
        body = provider_def().replace(
            f'    provider_contract: "{CLOCK_CONTRACT}"\n', ""
        )
        with self.assertRaisesRegex(SystemExit, "must be declared together"):
            self.parse(body)

    def test_identity_must_be_canonical_and_same_family(self) -> None:
        malformed = provider_def().replace(CLOCK_CONTRACT, "clock")
        with self.assertRaisesRegex(SystemExit, "malformed or cross-family"):
            self.parse(malformed)
        cross_family = provider_def().replace(
            "provider-operation.v1/clock/", "provider-operation.v1/io/"
        )
        with self.assertRaisesRegex(SystemExit, "malformed or cross-family"):
            self.parse(cross_family)

    def test_noncanonical_leaf_shapes_fail_closed(self) -> None:
        mutations = (
            ('visibility: "internal"', 'visibility: "public"'),
            ('effect: "nothrow"', 'effect: "io"'),
            ('signature: "(): i64"', 'signature: "(): bool"'),
            ('aot_kind: "method"', 'aot_kind: "builtin"'),
            ('vm: "sample_probe"', 'vm: "sample_probe"\n    vm_ifdef: "_WIN32"'),
            ('visibility: "internal"', 'visibility: "internal"\n    caps: ["io"]'),
        )
        for old, new in mutations:
            with self.subTest(replacement=new):
                with self.assertRaisesRegex(SystemExit, "unconditional internal nothrow"):
                    self.parse(provider_def().replace(old, new))

    def test_only_nullary_or_unary_i64_parameters_are_allowed(self) -> None:
        with self.assertRaisesRegex(SystemExit, "unconditional internal nothrow"):
            self.parse(provider_def(signature="(value: bool): i64", argc=1))
        with self.assertRaisesRegex(SystemExit, "unconditional internal nothrow"):
            self.parse(provider_def(signature="(a: i64, b: i64): i64", argc=2))


if __name__ == "__main__":
    unittest.main()
