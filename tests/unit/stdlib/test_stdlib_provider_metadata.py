#!/usr/bin/env python3
"""Fail-closed tests for source-owned runtime-provider metadata."""

from __future__ import annotations

import sys
import json
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "stdlibgen"))

from provider_declarations import admission_reasons, declaration_inventory
from stdlibgen import parse_def_metadata, parse_defs  # noqa: E402


CLOCK_CONTRACT = "xray.runtime.provider.v1/clock"
CLOCK_OPERATIONS = {
    "xray.runtime.provider-operation.v1/clock/realtime-nanos",
    "xray.runtime.provider-operation.v1/clock/monotonic-nanos",
    "xray.runtime.provider-operation.v1/clock/process-cpu-nanos",
    "xray.runtime.provider-operation.v1/clock/utc-offset-minutes-at",
}
IO_CONTRACT = "xray.runtime.provider.v1/io"
IO_OPERATIONS = {
    "xray.runtime.provider-operation.v1/io/pipe-open",
    "xray.runtime.provider-operation.v1/io/pipe-close",
}


def provider_def(extra: str = "", signature: str = "(): i64", argc: int = 0) -> str:
    descriptor = {
        "provider_parameter_modes": ",".join("in" for _ in range(argc)),
        "provider_parameter_owners": ",".join("trivial" for _ in range(argc)),
        "provider_result_owner": "trivial", "provider_error": "none", "provider_panic": "none",
        "provider_suspend": "never", "provider_refusal": "trap",
        "provider_resources": [], "provider_effects": "reads-clock",
        "provider_threads": "any", "provider_reentry": "allowed",
        "provider_callbacks": "none", "provider_platforms": "linux,macos,windows",
        "provider_profiles": "hosted", "provider_adapter": "i64-nullary-u64",
        "provider_host_header": "os/os_time.h", "provider_host_symbol": "sample_probe",
    }
    if signature.endswith(": i64") and argc == 1:
        descriptor["provider_adapter"] = "i64-unary-status-out"
    elif signature.endswith(": bool") and argc == 1:
        descriptor["provider_adapter"] = "bool-i64-pipe-close"
        descriptor["provider_resources"] = [{"resource": "xray.runtime.resource.v1/pipe-endpoint",
                                             "value": "parameter.0", "action": "consume",
                                             "when": "call-enter"}]
    descriptor_text = "".join(f"    {key}: {json.dumps(value)}\n"
                               for key, value in descriptor.items())
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
        f"{descriptor_text}"
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

    def test_repository_provider_metadata_is_exact(self) -> None:
        entries = [entry for entry in parse_defs(ROOT) if entry.provider_declaration]
        self.assertEqual(12, len(entries))
        self.assertEqual({"mem.__alloc", "mem.__allocZeroed", "mem.__allocAligned", "mem.__bufferLength"},
                         {entry.symbol for entry in entries if entry.module == "mem"})
        entropy = next(entry for entry in entries if entry.symbol == "crypto.__fillRandomBytes")
        self.assertEqual("Array<u8>", entropy.provider_declaration.logical.parameters[0].type)
        self.assertEqual("ref", entropy.provider_declaration.logical.parameters[0].mode)
        clock_entries = [
            entry for entry in entries if entry.provider_declaration.contract == CLOCK_CONTRACT
        ]
        io_entries = [entry for entry in entries if entry.provider_declaration.contract == IO_CONTRACT]
        self.assertEqual(4, len(clock_entries))
        self.assertEqual(CLOCK_OPERATIONS, {entry.provider_declaration.operation for entry in clock_entries})
        self.assertEqual(2, len(io_entries))
        self.assertEqual(IO_OPERATIONS, {entry.provider_declaration.operation for entry in io_entries})
        self.assertTrue(all(entry.visibility == "internal" for entry in entries))
        self.assertTrue(all(entry.effect == "nothrow" for entry in entries))
        self.assertTrue(all(entry.provider_declaration is not None for entry in entries))
        process = next(entry for entry in entries if entry.symbol == "os.__getpid")
        self.assertEqual("xray.runtime.provider.v1/process", process.provider_declaration.contract)
        self.assertEqual("i64-nullary-i64", process.provider_declaration.host.adapter)
        pipe_close = next(entry for entry in io_entries if entry.name == "__pipeClose")
        self.assertEqual("sys", pipe_close.module)
        self.assertEqual("(handle: i64): bool", pipe_close.signature)
        self.assertEqual("1", pipe_close.argc)
        self.assertEqual("v", pipe_close.arg_spec)

    def test_nullary_and_unary_i64_leaves_are_accepted(self) -> None:
        self.assertEqual(CLOCK_CONTRACT, self.parse(provider_def())[0].provider_declaration.contract)
        unary = provider_def(signature="(seconds: i64): i64", argc=1)
        self.assertEqual(1, len(self.parse(unary)))

    def test_unary_i64_bool_leaf_is_accepted(self) -> None:
        unary = provider_def(signature="(value: i64): bool", argc=1)
        self.assertEqual(1, len(self.parse(unary)))

    def test_contract_and_operation_must_be_paired(self) -> None:
        body = provider_def().replace(
            f'    provider_contract: "{CLOCK_CONTRACT}"\n', ""
        )
        with self.assertRaisesRegex(SystemExit, "missing explicit provider facts"):
            self.parse(body)

    def test_duplicate_properties_cannot_replace_provider_facts(self) -> None:
        with self.assertRaisesRegex(SystemExit, "duplicate declaration property: provider_threads"):
            self.parse(provider_def(extra='    provider_threads: "instance-affine"\n'))

    def test_identity_must_be_canonical_and_same_family(self) -> None:
        malformed = provider_def().replace(CLOCK_CONTRACT, "clock")
        with self.assertRaisesRegex(SystemExit, "malformed or cross-family"):
            self.parse(malformed)
        cross_family = provider_def().replace(
            "provider-operation.v1/clock/", "provider-operation.v1/io/"
        )
        with self.assertRaisesRegex(SystemExit, "malformed or cross-family"):
            self.parse(cross_family)

    def test_provider_has_one_private_semantic_owner(self) -> None:
        body = provider_def().replace('visibility: "internal"', 'visibility: "public"')
        with self.assertRaisesRegex(SystemExit, "one private semantic owner"):
            self.parse(body)

    def test_removed_target_leaf_cannot_restore_numeric_dispatch(self) -> None:
        for value in ('"i64-getpid"', '""'):
            with self.subTest(value=value):
                with self.assertRaisesRegex(SystemExit, "removed target_leaf declaration"):
                    self.parse(provider_def(extra=f"    target_leaf: {value}\n"))

    def test_hosted_declaration_does_not_depend_on_legacy_aot_metadata(self) -> None:
        body = provider_def().replace('aot_direct: true', 'aot_direct: false')
        body = body.replace('aot: "xrt_sample_probe"', 'aot: ""')
        body = body.replace('    aot_kind: "method"\n', '')
        entry = self.parse(body)[0]
        self.assertFalse(admission_reasons(entry.provider_declaration))

    def test_unadmitted_signatures_still_appear_in_inventory(self) -> None:
        for signature, argc in [("(): bool", 0), ("(value: bool): i64", 1),
                                 ("(a: i64, b: i64): i64", 2)]:
            with self.subTest(signature=signature):
                entries = self.parse(provider_def(signature=signature, argc=argc))
                row, = declaration_inventory(entries)["leaves"]
                self.assertTrue(row["declared"])
                self.assertFalse(row["admitted"])
                self.assertIn("declared-signature-does-not-match-explicit-host-adapter",
                              row["blocking_reasons"])


if __name__ == "__main__":
    unittest.main()
