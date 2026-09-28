# XIR managed value boundary

This contract adds internal string values and explicit result ownership to the
resumable runtime. It does not qualify source syntax, Program/Instance sealing,
generic library publication, concurrent activation driving, or product cutover.

Value ABI 9 replaces prior value ABIs without an alias or reader. A value remains a
16-byte, eight-aligned carrier: u32 type, zero reserved u32, and eight payload
bytes. Unit/bool/i64 retain their canonical meanings. String payload bytes encode
a live opaque pointer by memcpy; they are never language integers. Host values
must originate from constructors or owned copies. This trusted native interface
does not validate forged/dangling pointers or accept serialized pointer bytes.

Constructed IDs are local to an independently owned TypeArena under
`xir-constructed-types.md`. A function, cell or Array object owns that arena; its kind
and element/signature are resolved from the verified pool. Copy/drop validates
the object's own metadata, while execution admission additionally requires the
expected arena and type. Equal numeric IDs from separately sealed arenas do not
grant interchangeability. The carrier size alone does not preserve an older ABI.
Array backing, compact owned elements, allocation-domain accounting, bounded
admission, COW mutation and release are governed by `xir-array-storage.md`.
Registering that contract does not establish implementation or qualification;
its runtime and complete-program evidence remain OPEN until actually executed.

Strings own strict UTF-8, including embedded NUL. No normalization or replacement
occurs. Length queries distinguish bytes from Unicode scalar count, neither is a
grapheme count. Borrowed byte views expire on owner mutation/drop. Copy retains
atomic storage; drop releases it exactly once. Counts cannot wrap or resurrect:
retain saturation fails, and invalid release is a hard invariant failure. Atomic
RC does not permit racing access to one mutable value handle. Independent owned
copies may be read/dropped concurrently; publication synchronization is external.

An explicit refcounted allocation domain owns its limit and physical accounting.
Each string keeps only this small domain alive, never an activation, instance,
artifact, or code image. Domain admission charges metadata and byte capacity;
allocation failure rolls back reservations. The domain's host handle may be
dropped before strings. Its final string releases remaining storage physically.
Append requires exclusive access to the destination handle. Unique storage grows
in place when capacity permits and reallocates its byte buffer otherwise; shared
storage separates. Self-append is valid. Failure preserves the original value,
published copies, and live accounting. Capacity is an implementation detail.

Call ABI 14 admits strings and Atomic<i64> identity values, with function/cell
rules governed by their respective contracts. Its typed entry, action and inbox
boundaries carry the expected arena. Arguments are borrowed at admission and copied into
owned frame storage before publication. Resume arguments/inbox and action return
values are borrowed views. The driver retains a return before child cleanup and
owns each inbox and terminal result. Poll exposes a borrowed result; explicit
take transfers it once and changes the activation to CONSUMED. Destruction drops
an untaken result. Taking a result needs no allocation and the resulting owned
value survives activation and input destruction. Frame cleanup runs before its
arguments and inbox are dropped. Failure/cancellation publishes no partial result.

Typed output is an explicit synchronous provider effect. Call ABI 14 uses a
borrowed typed group. Raw output has one value and
names stdout or stderr. Line output names stdout and carries zero or more
bool/i64/string values: arguments evaluate left to right before one provider call.
Rendering inserts exactly one ASCII space between values and one final LF, as
required by source print. A raw group adds neither spacing nor a newline.
The renderer validates and allocates the complete group before calling its byte
sink exactly once; malformed values, budget failure and OOM cause no sink call.
Embedded NUL and UTF-8 bytes are length-delimited. Values remain typed until the
provider chooses rendering. Missing/rejecting providers fail with OUTPUT_ERROR, not a
language throw or fallback. Reentrant cancellation takes effect after the callback;
already accepted output is not rolled back. The provider cannot suspend and must
copy any value it retains. Provider context is borrowed for activation lifetime.
Both native groups and XIR PRINT admit up to 65536 values within resource budgets.
PRINT uses a first/count range into the owned function operand table and a zero
immediate. Each operand must dominate the instruction and have bool/i64/string
type. Empty ranges are canonical zero/zero; nonempty ranges partition the table
in instruction order. Lowering reserves the exact maximum outgoing group capacity
in the stable activation frame and includes it in physical-byte admission.
Older call ABI providers and generated entries are rejected, with no adapter.

WRITE_STREAM is bool-typed, borrows exactly one string and names stdout/stderr
with immediate 1/2. It publishes one raw group through the instance output provider
and receives a canonical bool inbox before execution continues. A configured
provider's false result is an observable language value, while an absent provider
remains OUTPUT_ERROR. PRINT and OUTPUT still fail on provider rejection.
Cancellation requested by the provider takes precedence over either bool result;
accepted bytes cannot be rolled back. Provider calls cannot suspend. Native action
admission rejects line groups, non-string values, invalid streams/counts and dirty
unit payloads before calling the provider. Old call ABI entries are rejected.
This is the typed write-result primitive; source stdlib binding, host stream
flushing and filesystem short-write policy require their own implementation.

verification-test: test_xir_values
verification-test: test_xir_value_allocations

String XIR operations admit string parameters/results, abstract COPY before
lowering, physical OWNED_RETAIN after lowering, CONCAT_STRING, and OUTPUT.
OUTPUT is unit-typed, consumes a bool/i64/string operand, and uses immediate 1/2 for
stdout/stderr. CONCAT_STRING preserves both borrowed operands and returns an owned
string. Lowering publishes and reverifies an exact list of owned frame offsets.
Every string slot starts empty, owns its current value, drops before replacement,
and is cleared on all exits in reverse slot order. Both backends consume this
cleanup list. Parameters and child results are retained into slots; return actions
borrow until the driver captures ownership. Repeated loop definitions replace
and release previous values. Closed scalar leaf execution rejects managed values.
Literal tables are sealed with Program metadata. Source-to-XIR string production
is governed by `xir-source-owner.md`; it adds no alternate value representation.

verification-test: test_xir_string_vm
verification-test: test_xir_string_native
verification-test: test_xir_emit_strings
verification-test: test_xir_emit_output
verification-test: test_xir_output_vm
verification-test: test_xir_output_native

## String scalar queries

Checked semantic contract 24 replaces 23 without a reader or compatibility path.
STRING_LEN (78) returns i64 from one string value; EQ_STRING (79) and NE_STRING
(80) return canonical bool from two string values. All stages admit them, with
zero immediate, no successors and no operand-table range. Non-string operands,
local/place operands without a value read, wrong result types and dirty unused
fields reject during ordinary verification and packet reading.

Length reads the existing strict UTF-8 scalar count in O(1), with embedded NUL
counted and no normalization. Equality compares full lengths and bytes, not
C-string termination, hashes or intern identity. Queries borrow existing owned
values without allocating, retaining, mutating or releasing them. Operand
evaluation remains left-to-right and may fail independently. Results contain no
borrowed pointers and survive operand destruction. Length exceeding INT64_MAX
fails with the ordinary resource limit; no narrowing wrap is allowed.

Both backends use the same owned string primitives. Source len lookup keeps its
core declaration identity and lexical shadowing; string comparisons require
concrete string types at definition checking. Existing Array len behavior stays
in the same intrinsic, with its governed read-place rules. No generic constraint
is inferred from future instantiations. String methods, ordering, iterators,
parsing and independently published stdlib definitions remain separate work.

Evidence must cover empty/ASCII/multibyte/supplementary/combining/NUL content,
distinct equal allocations, prefixes, mutation after snapshot, operand effects
and suspension, dead producers, bad unused generic definitions, forged packets,
VM/native independent expected outputs, cleanup and allocation-failure walks.

## String predicate implementation contract

The pending predicate batch adds contains, startsWith and endsWith over two
strict UTF-8 strings. Empty patterns match; overlong patterns do not. Full byte
payloads include NUL and are not normalized. Queries borrow inputs and publish
a bool only after validation; invalid native arguments leave output unchanged.
They allocate no heap storage, change no reference counts, and add no suspension
or throw. Operand evaluation and snapshot acquisition retain ordinary failures.

Source calls bind the governed string declaration and exact public member
identity. One generated registry supplies compiler and tool queries; method
spelling alone grants no authority. Receiver snapshots precede argument effects
and suspension. Generic definitions gain no capability from future arguments.

VM and native lowering consume verified predicate instructions with bool
results, two string values and zero unused fields. Checked semantic versioning
must change atomically at instruction admission; this contract alone does not
establish implementation. Reuse runtime-neutral byte helpers without old
isolate, intern or GC ownership.

Evidence covers empty/overlong patterns, distinct/shared objects, Unicode/NUL,
dead producers, allocation accounting, receiver mutation and suspension, source
member queries, hostile packets, independent VM/native output and physical
release. Source NUL remains closed until remaining consumers migrate or retire.

The string declaration uses native identity 2, value kind, and zero generic
parameters. Its declaration schema spells struct string; this does not create
a user-constructible nominal struct or grant field construction authority.
Array retains identity 1 and its one-parameter prelude binding. The registry
validates each type against its canonical generated row, including source
spans and generic arity. Primitive string binding remains the existing scalar
type authority. The old analyzer projects the same rows when its embedded
string declaration is removed; no second string declaration loader remains.


## String search rune coordinates

The immutable string indexOf query takes a signed i64 rune start in the
inclusive range zero through the receiver scalar count. Omitted source start
is zero. Negative and excessive starts produce the existing bounds panic;
there is no clamping or legacy byte-coordinate overload. Empty patterns match
at start. lastIndexOf has no start argument and matches an empty pattern at
the scalar count. Both return i64 rune ordinals, or minus one for no match.

Valid UTF-8 byte search is an implementation mechanism, not the public
coordinate system. Matching preserves NUL and does not normalize. The query
borrows its operands without allocation, reference-count mutation, suspension
or ownership transfer. Receiver, pattern and optional start evaluate once in
that order, preserving snapshots across effects and suspension. Native invalid
arguments, bounds and representability failures leave the output unchanged.
Scalar counts beyond i64 produce LIMIT. Byte offsets beyond ptrdiff_t are
rejected before using byte-search helpers. ASCII can use byte coordinates
because its stored byte and scalar counts are equal; Unicode prefix counting
has linear cost and repeated queries can repeat that work.

Source and Checked admission require canonical declaration identity and the
ordinary definition-time constraints. This value-layer contract does not
claim those consumers are implemented, nor does it admit source NUL literals.
