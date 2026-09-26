# Owned Checked packet admission

This format carries the implemented declaration/type/op subset and marker-constrained
generic templates. It does not qualify member witnesses, effect/diagnostic serialization, package
linking, native cache admission or publication. There is one Checked reader and
no Built, Lowered, legacy, or alternate executable format reader.

All integers use fixed-width little endian, with no native struct padding.
The 64-byte header contains magic `XRCHK\0\0\0`, schema u32 2, semantic-contract
u32 3, stage u32 2, reserved u32 zero, payload length u64, then SHA-256 (32 bytes)
over header bytes 0..31 followed by the payload. Unknown versions, stage or
reserved fields, length mismatch, trailing bytes and digest mismatch reject.
The digest is content identity/integrity, not authentication. Schema or semantic
changes require an explicit incompatible revision; no compatibility reader.

Payload order is function count, declaration-presence (0/1), then functions.
Each function is name blob, parameter count/types, result type, block count and
first/count pairs, instruction count and records, operand count and value IDs.
An instruction contains op/type/args[2]/targets[2] u32s and immediate i64 encoded
as two's-complement u64. A blob is u32 length followed by exactly those bytes.
Declaration data, when present, is module/slot/literal counts, root/entry IDs,
modules (name blob, dependency count/IDs, initializer), one module/exported pair
per function, slots (module/type/mutable), and literal blobs. A generic-presence flag and per-function constraint/type-argument
tables follow, governed by `xir-generic-templates.md`. Revision 1 rejects without
a compatibility reader. All IDs retain their
Checked meaning. No source path, pointer, native code, target layout, runtime
state or trusted verification result is persisted.

Writing first verifies an owned Checked artifact. Reading bounds packet bytes
by metadata_bytes and four work units per byte before hashing or allocation;
the same packet bound applies to writing. Decoded allocation bytes, including
artifact and declaration owners, are independently bounded by metadata_bytes.
Function/parameter/block/instruction counts consume aggregate limits before
allocation. Counts must also fit the remaining minimum wire size. Each semantic
verification pass uses the supplied stage budget independently, as other stage
transitions do. Runtime frame limits are not serialized.

The reader owns all decoded arrays and blobs, frees every partial allocation on
failure, and performs the existing full Checked semantic and layout verification
before publishing. Recomputed digests cannot bypass CFG, dominance, types,
canonical fields, import/export, initializer or slot permissions. Lowering then
uses the existing transition and verifies again. Successful output does not
borrow input bytes. Failure publishes no artifact or packet.

verification-test: test_xir_checked
verification-test: test_xir_checked_allocations
verification-test: test_xir_packet_vm
verification-test: test_xir_source_native
