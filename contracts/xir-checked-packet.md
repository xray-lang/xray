# Owned Checked packet admission

This format carries the implemented declaration/type/op subset and marker-constrained
generic templates. It does not qualify member witnesses, effect/diagnostic serialization, package
linking, native cache admission or publication. There is one Checked reader and
no Built, Lowered, legacy, or alternate executable format reader.

All integers use fixed-width little endian, with no native struct padding.
The 64-byte header contains magic `XRCHK\0\0\0`, schema u32 6, semantic-contract
u32 17, stage u32 2, reserved u32 zero, payload length u64, then SHA-256 (32 bytes)
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
modules (name blob, dependency count/IDs, initializer), one module/exported/nominal_owner u32 triple
per function, slots (module/type/mutable), and literal blobs. A generic-presence flag and per-function constraint/type-argument
tables follow, governed by `xir-generic-templates.md`. A constructed-node u32 count
and nominal-declaration u32 count follow. Each node record starts with u32 kind and u32 exact free-parameter span.
CALLABLE (kind 1) carries parameter count, ordered type/mode u32 pairs, result
type and flags. ARRAY (kind 2) and CELL (kind 3) each carry one u32 element ID.
Both counts zero have no pool owner; a declaration-only pool has an owner. Nested identity, kind-specific payloads and canonical
contracts follow `xir-constructed-types.md`; descriptor admission alone does not
implement Array source syntax or runtime operations. `xir-array-values.md` owns
the semantic-16 Array tags 63..69 and floating arithmetic tags 70..73, exact operand roles and logical places.
The fixed instruction and non-nominal type-node records are unchanged; schema 6 adds the nominal count, declarations and function declaration-owner word. Projection IDs are
ordinary instruction indices with restricted roles, never serialized addresses,
physical offsets or capability proofs. NEW/SET operand-table ranges participate
in the same complete partition and decode/work budgets. Rehashed packets still
must pass role, mutability, declaration, generic-scope and dominance checks.
Earlier revisions reject without
a compatibility reader. All IDs retain their
Checked meaning. No source path, pointer, native code, target layout, runtime
state, arena address or trusted verification result is persisted.

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

Nominal declarations follow all expression nodes, in declaration order. Each is
module identity blob, declaration name blob, exported u32, parameter count and
constraint u32 sequence, then field count. Each field is name blob, type u32 and
flags u32. Names gain an owned terminator in memory but serialize only identity
bytes. Duplicate identities/names, invalid flags or generic scope and unknown
owning modules reject even with a recomputed digest. Physical field offsets,
closed instance caches and runtime permissions are not serialized. This metadata
extension admits nominal metadata nodes under the following rules; it does not admit executable struct bodies.

NOMINAL kind 4 uses kind/span/declaration/argument-count u32 words followed by
ordered u32 argument IDs, then field-count and derived field-type u32 words.
A present field vector is recomputed against the declaration and arguments;
empty leaves fields unresolved. No physical layout offsets are serialized.
The current metadata reader admits closed arguments under declaration
constraints and preserves exact declaration/argument identity; unsupported
abstract and nominal arguments reject. This uses the sole
schema 6 / contract 17 representation; no alternate reader is introduced. Closed nominal
transport and STRUCT_NEW/GET/SET (tags74/75/76) follow `xir-nominal-struct-types.md`.
NEW uses the canonical operand-table range; GET/SET store a field ordinal in
immediate. SET takes a writable place and an ordinary field value. All three
recheck declaration permissions, field substitution and value roles after
decoding. Source struct admission remains OPEN.
