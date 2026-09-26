#!/usr/bin/env python3
"""Provider code generation preserves identity and refuses unadmitted contracts."""

from dataclasses import replace
from pathlib import Path
import sys
import hashlib
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/stdlibgen"))
from provider_codegen import (admitted_entries, emit_provider_aot_sources, emit_provider_bindings, emit_provider_descriptors, emit_provider_keys,
                              logical_bytes, logical_fingerprint)
from stdlibgen import parse_defs
from provider_types import logical_type, native_resource_id, resolve_native_types


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
        self.assertEqual(13, len(admitted_entries([*self.entries, entry])))
        generated = emit_provider_descriptors([*self.entries, entry])
        self.assertIn('"xray.runtime.provider-operation.v1/environment/probe"', generated)
        self.assertIn('"xr_os_core_getpid"', generated)
        self.assertIn("XR_STDLIB_PROVIDER_I64_NULLARY_I64", generated)
        native = emit_provider_aot_sources([*self.entries, entry])
        self.assertIn("XR_AOT_NATIVE_PROVIDER_SOURCE_COUNT 13u", native)
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

    def test_storage_identity_and_recursive_wire_have_independent_framing(self):
        framed = (b"xray-stdlib-resource-v1\0\x03\0\0\0\0\0\0\0mem"
                  b"\x0f\0\0\0\0\0\0\0__BufferStorage")
        identity = hashlib.sha256(framed).digest()[:16]
        self.assertEqual(identity, native_resource_id("mem", "__BufferStorage"))
        self.assertNotEqual(identity, native_resource_id("other", "__BufferStorage"))
        encoded, affine = logical_type("(i64, (bool, resource<mem.__BufferStorage>)?)")
        self.assertEqual(b"\x05\x02\x03\x06\x05\x02\x02\x07" + identity, encoded)
        self.assertTrue(affine)
        self.assertEqual((b"\x06\x05\x02\x03\x02", False), logical_type("(i64, bool)?"))

    def test_byte_array_type_is_distinct_and_affine(self):
        self.assertEqual((b"\x08", True), logical_type("Array<u8>"))
        self.assertEqual((b"\x05\x02\x08\x06\x08", True), logical_type("(Array<u8>, Array<u8>?)"))
        self.assertNotEqual(logical_type("string"), logical_type("Array<u8>"))
        for spelling in ("Array<i64>", "Array<u16>", "Array<bool>", "ref Array<u8>"):
            with self.subTest(spelling=spelling), self.assertRaises(ValueError):
                logical_type(spelling)

    def test_entropy_requires_exact_exclusive_byte_view(self):
        declaration = next(entry.provider_declaration for entry in self.entries
                           if entry.symbol == "crypto.__fillRandomBytes")
        parameter, = declaration.logical.parameters
        self.assertEqual(("Array<u8>", "ref", "borrow"),
                         (parameter.type, parameter.mode, parameter.owner))
        self.assertEqual("forbidden", declaration.logical.reentry)
        wire = logical_bytes(declaration)
        self.assertEqual(2, wire[19])
        self.assertEqual(b"\x02\x02\x08\x01\x01", wire[22:])
        for changed in (replace(parameter, mode="in"), replace(parameter, mode="out"),
                        replace(parameter, type="Array<u8>?"), replace(parameter, owner="consume")):
            with self.subTest(parameter=changed), self.assertRaises(ValueError):
                logical_bytes(replace(declaration, logical=replace(declaration.logical, parameters=(changed,))))
        with self.assertRaises(ValueError):
            logical_bytes(replace(declaration, logical=replace(declaration.logical, reentry="allowed")))

    def test_storage_declarations_have_exact_ownership_and_error_authority(self):
        storage = {entry.name: entry for entry in self.entries
                   if entry.module == "mem" and entry.provider_declaration}
        self.assertEqual({"__alloc", "__allocZeroed", "__allocAligned", "__bufferLength"},
                         set(storage))
        for name, entry in storage.items():
            declaration = entry.provider_declaration
            self.assertEqual("nothrow", entry.effect)
            self.assertEqual("typed", declaration.host.adapter)
            self.assertEqual("none", declaration.logical.error)
            if name == "__bufferLength":
                self.assertEqual("borrow", declaration.logical.parameters[0].owner)
                self.assertEqual("resource<mem.__BufferStorage>",
                                 declaration.logical.parameters[0].type)
                self.assertEqual("trivial", declaration.logical.result_owner)
            else:
                self.assertEqual("owned", declaration.logical.result_owner)
                self.assertEqual("resource<mem.__BufferStorage>", declaration.logical.result_type)
                self.assertEqual(2 if name == "__allocAligned" else 1,
                                 len(declaration.logical.parameters))

    def test_recursive_types_fail_closed_at_transport_boundaries(self):
        for spelling in ("unknown", "resource<mem.Missing", "(i64,)", "(i64,,bool)",
                         "i64" + "?" * 33, "(" + ",".join(["i64"] * 63) + ")"):
            with self.subTest(spelling=spelling), self.assertRaises(ValueError):
                logical_type(spelling)
        self.assertEqual("(resource<mem.__BufferStorage>, Missing)?",
                         resolve_native_types("(__BufferStorage, Missing)?", "mem",
                                              {"__BufferStorage"}))

    def test_resource_macro_collision_does_not_alias_nominal_identity(self):
        storage = next(entry.provider_declaration for entry in self.entries
                       if entry.module == "mem" and entry.name == "__alloc")
        changed = replace(storage, operation="xray.runtime.provider-operation.v1/sample/case",
                          logical=replace(storage.logical,
                                          result_type="resource<mem.__bufferstorage>"))
        entry = SimpleNamespace(symbol="sample.__case", provider_declaration=changed)
        with self.assertRaisesRegex(ValueError, "colliding C macro names"):
            emit_provider_keys([*self.entries, entry])


if __name__ == "__main__":
    unittest.main()
