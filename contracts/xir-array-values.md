# XIR Array operations, logical places and compact layout

This is the normative contract for the first owned Array operation family.
Implementation and qualification remain OPEN until the corresponding rebuilt
tests establish each assertion. It extends the sole Built -> Checked ->
specialized-and-rechecked -> Lowered pipeline; it introduces no legacy executor,
alternate packet reader, runtime generic registration or source-name shortcut.
The actual source rules remain in spec sections 3 and 14.7. Constructed identity
and TypeArena lifetime belong to `xir-constructed-types.md`; backing ownership
and COW publication belong to `xir-array-storage.md`; fault propagation belongs
to `xir-resumable-calls.md` and `xir-program-instance.md`.

## Opcode and operand authority

The enum in `xxir.h` starts with INVALID=0 before including `xxir_ops.def`;
its 62 definition rows therefore end at GE_FLOAT=62 and OP_COUNT=63. Append
exactly these seven operations, in this order, making XR_XIR_OP_COUNT=70.
All seven are valid in Built, Checked and Lowered (stage mask 7), have no
successors, and are not terminators. All unused args, targets and immediate
fields are zero. Unknown tags and noncanonical fields reject before use.

In this table A is the actual ARRAY<T> node in the owning pool. Node kind,
element and contextual parameter scope must be verified; matching numeric
width or a type-ID range is not ARRAY authority.

| Tag | Operation | Result type | Operand representation |
|---|---|---|---|
| 63 | CELL_PLACE | A | args[0] is an ordinary CELL<A> value ID; args[1]=0, immediate=0. |
| 64 | SLOT_PLACE | A | args are zero; immediate is the owning module's slot ID, whose exact type is A. |
| 65 | ARRAY_NEW | A | args[0]/args[1] are first/count in the function operand table; each entry is an ordinary T value. Empty is exactly zero/zero. |
| 66 | ARRAY_GET | T | args are (ordinary A value or readable A place, ordinary exact-i64 index); immediate=0. |
| 67 | ARRAY_SET | unit | args are first/count for exactly three operand-table entries: writable A place, ordinary exact-i64 index, ordinary T value. |
| 68 | ARRAY_PUSH | unit | args are (writable A place, ordinary T value); immediate=0. |
| 69 | ARRAY_LEN | i64 | args[0] is an ordinary A value or readable A place; args[1]=0, immediate=0. |

ARRAY_NEW and ARRAY_SET ranges join the existing single exact operand-table
partition in instruction order. Gaps, overlaps, trailing entries, overflow,
out-of-range IDs and a SET count other than three reject. A NEW range has at
most 65536 entries under the current operand-group bound and is additionally
charged to aggregate metadata, verification-work and frame budgets. Every
ordinary operand has the exact expected type and dominates its use. The SET
first entry is checked as a place role, not as an ordinary argument. This adds
no type-argument range or successor payload.

## Place roles and current-root access

A place is a producer-and-use role, not an additional type node. Existing
LOCAL_NEW/OWNED_LOCAL_NEW of A, CELL_PLACE and SLOT_PLACE may be Array receivers.
Only ARRAY_GET/LEN may take either a readable place or a normal Array value;
SET/PUSH require a writable place. Existing local read/write continue to accept
their local-place producers only; they do not acquire access to CELL_PLACE or
SLOT_PLACE. A normal Array parameter/result/const value is never writable merely
because it is backed by a unique object.

CELL_PLACE binds a stable, dominating ordinary CELL<A> holder. SLOT_PLACE binds
a verified slot identity in the current function's module and has the same
read permission as SLOT_LOAD. Mutation through it additionally requires the
existing mutable/root-module rule. Instance readiness, slot publication and
current publisher/callback permission are independently checked at execution.
No projection grants cross-module access, initializes an unpublished slot, or
extends root lifetime. The holder/frame/instance remains alive through use.

CELL_PLACE and SLOT_PLACE carry no payload, snapshot, raw address, retained
owner or persistent exclusive loan. They cannot appear in COPY/OWNED_RETAIN,
RETURN, THROW, CALL, captures, PHI, ARRAY_NEW, CELL_WRITE, slot writes, or any
other ordinary-value position. Their frozen frame offsets are UINT32_MAX and
they have no owned-cleanup entry. Verification recomputes both facts; a forged
physical offset or cleanup entry rejects. Existing local places still own real
payload storage and retain their existing exhaustive cleanup obligations.

Bind a ref receiver's root once, then evaluate index and arguments/RHS once in
source order, before resolving the current Array and beginning synchronous
exclusive access. An RHS that rebinds the same root changes the target Array
and the length used for SET bounds checking. A projection must not cache the old
Array or an element pointer across calls, suspension or rebinding. Earlier RHS
effects are not rolled back when the Array operation fails.

A normal GET receiver is a logical snapshot obtained before index evaluation.
The first source GET-place optimization is limited to non-failing literal or
plain-i64-name indexes with equivalent evaluation and failure order. Its runtime
access retains the current receiver transiently, copies out T, and releases the
transient receiver before returning; it leaves no extra whole-Array SSA owner.
Eliding a potentially failing retain requires a separate proof or an equivalent
check in the same order. `a.push(a[0])` owns its argument before any relocation.

## Effects and result ownership

NEW produces one independent owned logical Array value only after complete
success. GET produces an independently owned T; subsequent Array mutation/drop
cannot invalidate it. LEN borrows its receiver and returns canonical i64 without
allocation or a new persistent Array owner. LEN checks self, expected arena,
ARRAY kind/type, layout/length invariants and constant bounded work only; it does
not traverse elements or allocate transitive-admission scratch. It neither
extracts an element nor grants callable execution authority. Existing host/call
input boundaries still perform their full transitive admission. Thus a retained
Array containing a stopped function remains safely observable for length,
without making that function callable. Projections themselves do not allocate,
retain or mutate. Neither LEN nor a projection confers mutability.

SET and PUSH return canonical unit. They may allocate when detaching shared
backing or growing storage, and may fail during element retention/admission.
No NO_HEAP, unsafe-only, infallible or consuming-receiver authority follows from
old Array registries. GET/SET use signed i64 strict bounds, 0 <= index < length;
an invalid index produces the frozen Bounds fault with its observed index and
length. Numeric-range, allocation, retain-limit and work-limit failures retain
their distinct channels. PUSH checks length/capacity arithmetic before commit.
No Array kernel suspends, calls a user provider or performs user I/O.

All fallible argument/element ownership and COW preparation precede root
publication. SET releases the replaced element only after the new owner is
published; aliasing and self-assignment remain valid. An index-assignment
expression returns the already converted T with ownership prepared before
commit, not an alias into backing storage. Failure preserves the pre-commit
logical value, initialized-element count and unrelated copies. The common
storage contract owns exact rollback and physical accounting.

## One compact storage layout owner

The existing pure `xr_xir_layout(types, type, target, context, output)` service
is the only type-width/alignment authority. Its implementation is shared below
both compiler frame layout and runtime value storage, with one symbol and no
duplicated type switch. Frame-layout construction remains a compiler service;
runtime storage must not depend on artifact construction or an old runtime.

An Array uses `XR_XIR_LAYOUT_STORAGE` for its verified closed element T, under
the same selected target and Value ABI as execution. Element stride is checked
alignment rounding of that service's size; a zero size, invalid alignment,
unsupported target/context or arithmetic overflow rejects. ARRAY<unit> is
rejected by element eligibility before layout. Owned elements occupy typed
handle storage; boxed sixteen-byte boundary carriers and eight-byte frame lanes
do not determine element stride. No second width table is allowed in the VM,
emitter, value runtime, TypeArena or native helper.

Data alignment and checked index*stride/capacity*stride calculations obey that
layout. Byte access uses canonical scalar load/store or memcpy of owned handles,
not alignment-unsafe typed casts. Scalar elements preserve canonical bool,
integer and floating representations. Owned element copy/drop resolves actual
kind and arena metadata rather than guessing ownership from stride. Cached
element layout remains a derived, target-bound fact and is reverified before
use; packet metadata never supplies trusted physical sizes.

ARRAY_NEW includes its count in the existing frozen outgoing staging maximum.
Its boundary carriers borrow live SSA owners until the common constructor
copies them. This staging's bytes count against frame limits. ARRAY_SET's first
range item is not materialized as an XrXirValue; the already evaluated index and
element are passed synchronously while the logical root is resolved. A hidden
unbounded stack/VLA or an uncharged temporary element vector is not allowed.

## Generic and Checked boundaries

Built/Checked may contain ARRAY<T> and CELL<ARRAY<T>> in the declaring generic
context. NEW/GET/SET/PUSH/LEN require only ordinary copyable/storable element
admission. An unconstrained T gains no comparison, arithmetic, display,
construction-from-nothing or Sendable witness. Element/value/index types and
place permissions are checked even in unused definitions. Empty NEW requires
an explicit valid A type and does not default-construct any T.

Specialization substitutes the ARRAY/CELL graph in Checked, remaps every
instruction and signature type, preserves value/place roles and operand-table
partition, and rechecks the complete result before Lowered. The node cache is
scoped by declaration and ordered arguments; equal parameter ordinals from
different definitions do not share generic authority. All runtime types and
layout operands are closed before immutable Program sealing.

This cutover uses Checked schema 5 / semantic contract 16, Value ABI 9,
Call ABI 14 and Program ABI 9. Schema 5 remains because instruction records and
tagged type-node wire traversal are unchanged; the newly legal operations and
roles were introduced in semantic revision 15; current admission requires revision 16. Call/Program revisions carry the separately
frozen Bounds detail. Older or mixed semantic/call/program entries reject with
no alias or fallback. TypeArena/backing implementation does not invent a second
serialized physical format. A recomputed digest cannot authorize malformed
roles, ranges, mutability, generic context or physical layout.

## Required assertions before qualification

Stage tests cover every shape above, wrong ARRAY/CELL kind, mismatched element,
unit/internal-cell elements, non-i64 indexes, nonzero unused fields, out-of-range
IDs, table gaps/overlap/trailing entries and dominance failures. Each forbidden
ordinary use of a projection rejects, including forged PHI/capture arguments.
Layout tests require storage-free projections and exact outgoing/frame budget
boundaries; real local places still appear in cleanup. Independent compact
layout expectations cover bool, all admitted integer/floating widths and owned
handles without asserting that equal widths imply equal types.

Checked round-trip tests destroy all producer/input storage before consumption.
Rehashed malformed packets exercise the same role/range/scope failures. Every
new decode/clone/layout allocation and every work-budget edge rolls back without
a partial artifact. Generic tests include unused invalid definitions, T and
Sendable-T, nested ARRAY/CELL/CALLABLE substitution, two unrelated generic owners
with equal ordinals, and no residual parameter after sealing.

VM and strict native execution separately compare the two-module string Array
program to the hand-authored bytes `red\nblue\n2\ngreen\nblue\ngreen\n3\n`.
Further assertions cover generic i64 instances, read/const copy isolation,
self-push, GET snapshot timing, SET RHS rebinding/new-length failure, exact
Bounds detail, unique/shared mutation allocation behavior, element-copy failure
rollback, escaped ownership, nested callable gates and final physical release.
Backend agreement alone is not an oracle; registration below is not a PASS.

verification-test: test_xir_stages
verification-test: test_xir_checked
verification-test: test_xir_checked_allocations
verification-test: test_xir_allocations
verification-test: test_xir_generics
verification-test: test_xir_locals
verification-test: test_xir_values
verification-test: test_xir_value_allocations
verification-test: test_xir_program_vm
verification-test: test_xir_program_native
verification-test: test_xir_source
verification-test: test_xir_source_native
verification-test: test_xir_source_generics
verification-test: test_xir_source_allocations
verification-test: test_xir_packet_vm
