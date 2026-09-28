# Owned Checked packet admission

This format carries the implemented declaration/type/op subset and marker-constrained
generic templates. It does not qualify member witnesses, effect/diagnostic serialization, package
linking, native cache admission or publication. There is one Checked reader and
no Built, Lowered, legacy, or alternate executable format reader.

All integers use fixed-width little endian, with no native struct padding.
The 64-byte header contains magic `XRCHK\0\0\0`, schema u32 `XR_XIR_CHECKED_SCHEMA`, semantic-contract
u32 `XR_XIR_CHECKED_CONTRACT`, stage u32 2, reserved u32 zero, payload length u64, then SHA-256 (32 bytes)
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
modules (name blob, dependency count/IDs, initializer), one module/exported/nominal_owner/member_access u32 record
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
The metadata reader admits parameterized and nested nominal arguments, with
constraints rechecked in each owning declaration or function context and exact
declaration/ordered-argument identity preserved. This uses the sole
schema 7 / contract 19 representation; no alternate reader is introduced. Closed nominal
transport and STRUCT_NEW/GET/SET (tags74/75/76) follow `xir-nominal-struct-types.md`.
NEW uses the canonical operand-table range; GET/SET store a field ordinal in
immediate. SET takes a writable place and an ordinary field value. All three
recheck declaration permissions, field substitution and value roles after
decoding. Source struct admission remains OPEN.

Schema 7 appends a provenance-presence u32 after every module type pool. Zero
ends the module. One carries an original Checked module using the same encoding
and then one origin per output function: source function index, argument count,
and ordered output-pool type IDs, all u32. The original must have presence zero;
nesting rejects before allocating another module. Both modules share the cursor
allocation and count limits and the cumulative semantic-verification budget.
The reader owns each partial prefix and verifies declaration, body, substitution
and instance closure before publication. Provenance records are evidence to
recheck, never persisted authority. Lowered preserves and rechecks the source while remapping instance arguments.
Native Program12 sealing consumes the packet through shared decode/lower and
descriptor/layout matching. Specialization attaches provenance automatically;
only complete definition and correspondence verification establishes opaque
substitution authority. Packet presence or its digest alone grants no exemption.

Lowering retains an independently owned encoding of its input specialized Checked
module before type-pool projection. The Lowered artifact holds a separate SHA-256
of the entire packet; verification charges retained bytes and hashing work and
rejects missing storage or a changed digest. Artifact destruction releases the
packet through the Checked packet owner, including every failed transition.
This identity is not authentication or native descriptor correspondence; native
admission must decode, reverify, lower and compare the descriptor before use.

Semantic contract 24 admits STRING_LEN=78, EQ_STRING=79 and NE_STRING=80 under
the managed-value string-query contract. Schema 8 and instruction record size
are unchanged. Semantic 23 packets reject even when their digest is repaired.
The independent scalar KAT uses a 121-byte payload and digest
`4cadb7d850aa5cd130bc013f0f133f65368ce80430b1c057c69f34559e480b1a`.
Rehashed string-query records must revalidate result/operand types, dominance,
zero unused fields and immediate; scalar result types never permit treating
string operands as integer payloads.

Semantic contract 25 admits STRING_CONTAINS=81, STRING_STARTS_WITH=82 and
STRING_ENDS_WITH=83. Each has two string operands, one bool result and zero
unused fields. Schema 8 and the 32-byte instruction record remain unchanged.
Semantic 24 rejects even after digest repair. The independent 121-byte scalar
fixture digest is
`0432aecd7c074ba0883c93aefc7d45439b986662aba9fc513d4b8c0a744b8340`.
Queries do not allocate or alter input ownership; runtime evaluation preserves
receiver snapshots across argument effects and suspension. Declaration names
do not grant authority without canonical native identity and exact types.

Semantic contract 26 admits STRING_INDEX_OF=84 and STRING_LAST_INDEX_OF=85.
Both return i64 rune ordinals under the managed-value search contract.
INDEX_OF uses the existing operand table with exactly three entries:
receiver string, pattern string, start i64. LAST_INDEX_OF has two direct
string operands. Unused targets and immediate remain zero; instruction
records remain 32 bytes; the current schema is 9. Table extent, exact count, each type,
value role and dominance are mandatory even after digest repair. Semantic25
packets reject with repaired digests; there is no old-reader compatibility.
The independent121-byte scalar payload digest is `2d208d11b593786586de389e44b40d4fd349bcfc1b2f9625403a6bbc2edbe0a3`.
Bounds faults preserve the supplied start and receiver rune count through
the existing call fault channel; no typed exception or suspension is added.


## Nominal variant metadata revision

Schema 9 / semantic 27 writes nominal kind after export visibility, and writes
variant count after the flat fields. Each ordered variant has a length-prefixed
name followed by field-begin u32 and field-count u32. The owned reader validates
contiguous complete ranges and per-variant field names before publication.
Schema 8 is rejected; there is no absent-kind compatibility interpretation.
