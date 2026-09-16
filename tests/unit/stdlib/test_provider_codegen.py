#!/usr/bin/env python3
"""Provider code generation preserves identity and refuses unadmitted contracts."""

from dataclasses import replace
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/stdlibgen"))
from provider_codegen import (admitted_entries, emit_provider_aot_sources, emit_provider_bindings, emit_provider_descriptors, emit_provider_keys,
                              logical_bytes, logical_fingerprint)
from stdlibgen import parse_defs


class ProviderCodegenTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.entries = parse_defs(ROOT)
        cls.clock = next(entry.provider_declaration for entry in cls.entries
                         if entry.module == "time" and entry.name == "__realtimeNanos")

    def test_scalar_wire_known_answer(self):
        self.assertEqual("010000000800000007000000010002000101010101010301",
                         logical_bytes(self.clock).hex())
        self.assertEqual("bef6491cf0d7096b74b51f399d6662bff705e1275a49d404fccc76db5f8ca064",
                         logical_fingerprint(self.clock).hex())

    def test_host_selection_cannot_change_logical_identity(self):
        other = replace(self.clock, host=replace(self.clock.host, symbol="another_clock"))
        self.assertEqual(logical_bytes(other), logical_bytes(self.clock))
        self.assertEqual(logical_fingerprint(other), logical_fingerprint(self.clock))
        changed = replace(self.clock, logical=replace(self.clock.logical, effects=("reads-process",)))
        self.assertNotEqual(logical_fingerprint(changed), logical_fingerprint(self.clock))

    def test_unimplemented_contract_cannot_produce_runtime_bytes(self):
        declaration = replace(self.clock, logical=replace(self.clock.logical, callbacks="synchronous"))
        with self.assertRaisesRegex(ValueError, "not admitted"):
            logical_bytes(declaration)
        with self.assertRaisesRegex(ValueError, "not admitted"):
            logical_fingerprint(declaration)

    def test_generated_records_have_deterministic_order(self):
        self.assertEqual(emit_provider_descriptors(self.entries),
                         emit_provider_descriptors(list(reversed(self.entries))))
        self.assertEqual(emit_provider_keys(self.entries),
                         emit_provider_keys(list(reversed(self.entries))))
        self.assertEqual((ROOT / "src/runtime/abi/xr_stdlib_provider_descriptors_gen.inc.c").read_text(),
                         emit_provider_descriptors(self.entries))
        self.assertEqual((ROOT / "src/runtime/abi/xr_stdlib_provider_keys_gen.h").read_text(),
                         emit_provider_keys(self.entries))

    def test_generated_bindings_use_declared_typed_host_functions(self):
        generated = emit_provider_bindings(self.entries)
        self.assertEqual(generated, emit_provider_bindings(list(reversed(self.entries))))
        self.assertEqual((ROOT / "src/execution/xr_stdlib_provider_bindings_gen.inc.c").read_text(),
                         generated)
        self.assertIn("xr_time_utc_offset_at(argument, result_out)", generated)
        self.assertIn("*result_out = xr_os_core_getpid();", generated)
        self.assertNotIn("XrValue", generated)
        self.assertNotIn("xrt_", generated)

    def test_new_family_needs_only_a_declaration(self):
        declaration = replace(self.clock, contract="xray.runtime.provider.v1/environment",
                              operation="xray.runtime.provider-operation.v1/environment/probe",
                              logical=replace(self.clock.logical, effects=("reads-process",)),
                              host=replace(self.clock.host, adapter="i64-nullary-i64",
                                           header="shared/xr_os_core.h", symbol="xr_os_core_getpid"))
        entry = SimpleNamespace(symbol="sample.__probe", provider_declaration=declaration)
        self.assertEqual(8, len(admitted_entries([*self.entries, entry])))
        generated = emit_provider_descriptors([*self.entries, entry])
        self.assertIn('"xray.runtime.provider-operation.v1/environment/probe"', generated)
        self.assertIn('"xr_os_core_getpid"', generated)
        self.assertIn("XR_STDLIB_PROVIDER_I64_NULLARY_I64", generated)
        native = emit_provider_aot_sources([*self.entries, entry])
        self.assertIn("XR_AOT_NATIVE_PROVIDER_SOURCE_COUNT 8u", native)
        self.assertEqual(native, emit_provider_aot_sources(list(reversed([*self.entries, entry]))))

    def test_aot_uses_shared_typed_host_bodies(self):
        generated = emit_provider_aot_sources(self.entries)
        self.assertEqual(generated, emit_provider_aot_sources(list(reversed(self.entries))))
        self.assertEqual((ROOT / "src/aot/program/xr_provider_aot_sources_gen.inc.c").read_text(), generated)
        self.assertIn("xr_time_utc_offset_at(argument, result_out)", generated)
        self.assertIn("*result_out = xr_os_core_getpid();", generated)
        self.assertIn("xr_pipe_create(&pipe, NULL)", generated)
        self.assertIn("_Generic(&xr_pipe_create", generated)
        for forbidden in ("GetProcessTimes(", "CreatePipe(", "mktime(", "XrValue", "xrt_"):
            self.assertNotIn(forbidden, generated)

    def test_duplicate_operations_cannot_acquire_an_alias(self):
        alias = SimpleNamespace(symbol="sample.__alias", provider_declaration=self.clock)
        with self.assertRaisesRegex(ValueError, "same provider operation identity"):
            emit_provider_descriptors([*self.entries, alias])
        alias.provider_declaration = replace(self.clock, host=replace(self.clock.host,
                                                                      adapter="future-adapter"))
        with self.assertRaisesRegex(ValueError, "same provider operation identity"):
            emit_provider_descriptors([*self.entries, alias])

    def test_empty_registry_is_valid_c11(self):
        generated = emit_provider_descriptors([])
        self.assertIn("{0}", generated)
        self.assertIn("XR_STDLIB_PROVIDER_DESCRIPTOR_COUNT 0u", generated)


if __name__ == "__main__":
    unittest.main()
