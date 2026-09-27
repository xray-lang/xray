# XIR constructed types and independently owned type arenas

Normative implementation contract, frozen before the constructed-type cutover.
Implementation and qualification are OPEN. Registration of existing test names
does not establish their new assertions or successful execution.

## Scope and sole representation

This contract covers three constructed kinds: CALLABLE, ARRAY, and internal CELL.
Existing primitive identities remain unchanged. It does not implement user
structs, classes, recursive nominal types, views, noncopyable resources, new
callable modes, or a new Sendable-function facility.

One owned constructed-type node pool replaces the separate callable table and
the encoded CELL flag. All producers and consumers use that pool: source
checking, Built/Checked verification, specialization, packet admission, lowering,
Program sealing, VM execution, native emission, and host value admission.
The old callable-table payload and `0x40000000 | element` CELL encoding are
removed in the same cutover. There is no old-format reader, type alias, adapter,
alternate executor, or runtime registration fallback.

## Type IDs and node structure

The bounded ID partition is:

| Domain | IDs | Meaning |
|---|---|---|
| Existing primitives | Existing IDs 0 through 13 | Keep each existing meaning; unit remains subject to its current restricted roles. |
| Reserved | 14 through 255 | Reject; no implicit primitive or constructed kind. |
| Constructed | 256 through 65535 inclusive | `node_index = type_id - 256`, with index strictly below the owned pool count. At most 65280 nodes. |
| Function-local type parameters | 65536 through 131071 inclusive | `ordinal = type_id - 65536`, with ordinal strictly below the enclosing function's declared parameter count. |
| Outside these domains | All remaining u32 values | Reject, including former CELL-flag values. |

Consumers check both bounds before interpreting a parameter or node ID. A bare
`type_id >= TYPE_PARAMETER_BASE` is not a valid parameter test. A constructed ID
does not identify its kind without its owning pool. Numeric range membership
alone never authorizes a callable invocation, an array operation, or cell access.

Each node has exactly one kind and the corresponding payload:

- CALLABLE: ordered parameter types and modes, result type, callable flags, and
  the derived parameter span. Only modes and flags already admitted by the
  callable contract are legal; this migration grants no additional modes.
- ARRAY: exactly one element type and the derived parameter span. The element
  must satisfy ordinary copyable/storable admission in the relevant context.
- CELL: exactly one element type and the derived parameter span. This is an
  internal shared capture place, not a source type or an ordinary generic
  argument. It may contain an admitted ARRAY or CALLABLE. Existing restrictions
  on public cell parameters/results and direct cell nesting remain in force.

Every constructed child references a strictly earlier node in the same pool.
Primitive and parameter references are not node edges. Forward edges, self edges,
cycles, missing children, wrong payload lengths, unknown kinds, nonzero reserved
fields, and duplicate canonical nodes reject. A builder interns a child before
its parent. `Array<fn() -> Array<T>>` is a finite ordered graph and is admitted
when its element and callable contracts are satisfied; no recursive type alias
or recursive nominal-type mechanism is implied.

Canonical equality includes kind, all admitted callable modes/flags, the ordered
parameter/result types, or the exact ARRAY/CELL element. Hashes may accelerate
interning but must be followed by complete equality on collisions. Layout
coincidence does not make two kinds or element types interchangeable.

## Derived facts and definition checking

Parameter span is zero for a primitive, ordinal plus one for a parameter, and
the maximum child span for a constructed node. It is recomputed under metadata
and work budgets. A serialized or cached span is not evidence of correctness;
if carried, it must exactly match the recomputed value.

Copyability, storability, Sendable, and potential contained callable identity are
derived from the admitted kind and its children. Cached facts are reverified and
are not independent authority. Internal CELL, unit, views, and noncopyable types
are not ordinary generic arguments or ARRAY elements. Internal capture storage
uses its existing separate role checks; it does not make CELL an ordinary value
type.

An unconstrained T is copyable and storable because of ordinary generic
admission. It has no additional arithmetic, comparison, formatting, reflection,
or Sendable authority. ARRAY requires storage capability only; it does not
require Comparable, Stringable, or Hashable. ARRAY's Sendable obligation is its
element obligation. A parameter requires a declared Sendable constraint to
prove that obligation. A current ordinary CALLABLE is not made Sendable by its
primitive parameters/result, atomic RC, or placement inside ARRAY.

All generic definitions, including unused ones, are checked in their declared
context. Definition checking may retain abstract copy/drop/element obligations.
Concrete arguments cannot repair an invalid generic body.

## Checked specialization and sealing

Specialization operates only on Checked XIR. It substitutes node children,
interns the resulting closed nodes, and remaps every type reference, including
function parameters/results, instructions, operands with type roles, callable
descriptors, capture cells, module slots, and explicit generic arguments.
A cache maps source node identities to specialized nodes for each ordered
generic instance; closed nodes may share a separate context-independent cache.
No cache may confuse a function-local parameter ordinal across declarations.

The resulting Checked graph is independently reverified before Lowered is
produced. A sealed runtime pool contains no parameters and every span is zero.
Ordinary reachable instances finish before immutable Program sealing. Uncovered
stdlib native-cache instances execute Lowered generated by this same pipeline;
execution does not register types or instantiate ordinary generics on demand.

Lowering computes target/context-bound layouts and owned cleanup obligations.
The three initial constructed kinds use managed handles, but their common
physical width grants no type conversion. Abstract copy/drop is discharged only
after concrete type verification. Runtime array element access uses the admitted
element descriptor, checked length/stride arithmetic, and its copy/drop rules.
`xir-array-values.md` owns exact opcode/place/operand roles and the one shared
compact-layout service; `xir-array-storage.md` owns backing/COW publication and
physical accounting. Neither contract grants kind authority from equal widths.

## TypeArena ownership

A TypeArena owns an immutable, fully verified closed pool and all variable-sized
descriptor payloads needed after execution. It has atomic reference counting,
explicit metadata allocation accounting, and bounded construction. It stores
node edges as local IDs, not independently reference-counted child pointers.
Its final release frees the pool without recursion proportional to graph depth.

A Program owns a reference to its runtime TypeArena. Constructed heap values
own enough arena lifetime to validate, read, copy, and release themselves after
their creating activation, Instance, artifact, and Program host handles are gone.
TypeArena has **no reverse Program lease** and no Instance pointer or module-slot
ownership. Pure `Array<string>` and nested primitive arrays must not keep Program
code alive merely to obtain element metadata.

For these three kinds, array destruction is performed by the common runtime
using the owned descriptors; an array must not retain an unowned generated-code
destructor pointer. Actual callable elements retain their existing FunctionGate
and required code lease. Stopping/freeing the Instance revokes execution
admission without preventing escaped function values from being read, copied,
or released. A callable code lease may outlive the Program host handle; this is
different from a TypeArena retaining Program in reverse.

Holding the complete immutable arena may retain unused type descriptors. Those
bytes remain accounted and are reclaimed at the last owner. It must not retain
unrelated module values or the whole Instance. Per-result descriptor-subgraph
compaction is an implementation optimization, not an admission prerequisite.

All allocation and retain failures roll back unpublished arenas, descriptors,
values, and partial elements. Reference counts cannot wrap, underflow, or revive
a dead owner. Retain saturation is a deterministic failure. Invalid release is
a hard invariant failure, independent of debug assertions.

## Runtime identity and foreign-arena rejection

A constructed runtime identity is the pair `(TypeArena owner, local type ID)`.
The u32 in a value carrier is not a process-global type identity. The managed
object records and owns its arena; its kind and descriptor must agree with its
carrier. Self-validation for copy/drop is distinct from admission against an
expected `(arena, type)` at an execution boundary.

The minimum host boundary rejects constructed values from a different arena,
even when the local ID or the complete structural type happens to match. It
does so before releasing an existing result, changing an execution epoch,
initializing modules, publishing arguments, or performing program side effects.
For example, Program P's ID256=`Array<string>` cannot enter Program Q whose
ID256=`Array<i64>`; two independently sealed arenas with ID256=`Array<string>`
also reject under this boundary. Fingerprint equality alone does not rehome the
foreign value or rewrite its local ID.

Instances of the same Program share that Program's arena. A pure ARRAY from one
such Instance may enter another by value when the existing element and domain
rules are met; copy-on-write preserves independent logical values. Do not impose
an Instance restriction on all arrays as a shortcut for checking function
elements. Existing globally identified primitive/string/Atomic behavior is not
silently narrowed by the constructed-type boundary.

Foreign-arena rejection is the current embedded-interface boundary, not a ban on
ordinary cross-module source arrays. Explicit cross-Program structural import
or ID remapping is outside this slice and must not be simulated with a raw u32
comparison. Checked package linking interns/remaps the constituent local type
graphs into one verified pool before Program sealing.

## Transitive callable and cell admission

Array wrapping must not bypass the existing callable gate. Each encountered
function still requires the correct live Program/Instance gate. A foreign or
stopped function inside `Array<fn>`, nested ARRAY, a CELL-held ARRAY, or a closure
capture rejects wherever the corresponding direct function would reject.
An empty array is not assigned a permanent Instance identity merely because its
type could contain a function.

One admission service covers all relevant boundaries: public host arguments,
direct/indirect call arguments, native action arguments and returns, inbox and
resumed values, result capture, module-slot writes, capture construction, cell
writes, and array construction/insertion/replacement. Expected arena/type and
execution authority reach these boundaries; repairing only `instance_start`
does not satisfy this contract. Native code cannot publish an array whose
elements would fail the VM boundary's checks.

Admission is bounded by explicit work and scratch budgets. Pure arrays may use
a reverified no-callable fact to avoid walking elements. Function-containing
arrays are checked without unbounded C recursion; budget exhaustion returns a
limit failure with no partial publication. Do not leave an unmetered fallback
for native, host, or nested values. A validated FunctionGate represents its
already-admitted capture environment, so checking that gate need not recurse
through closure/cell cycles.

A future cached transitive-admission proof is valid only if all constructors
and mutators maintain it and independent negative tests cover nested and empty
cases. Until then it cannot replace the actual bounded check. A stale memo or
type-level “may contain a function” flag is not proof of a particular value's
execution authority.

## Cleanup and physical release

ARRAY uses the existing allocation-free, nonrecursive managed destruction
worklist. Releasing an array queues owned elements exactly once, releases its
backing storage, and drops its metadata owner only after all required descriptor
information has been consumed or is independently owned by queued children.
Shared backing is destroyed only at its last owner. Metadata release introduces
neither a recursive type destructor nor cleanup allocation.

The array value contract separately specifies COW publication and mutation
failure atomicity. This type contract requires correct element ownership for
construction, reads, parameter/result copies, replacement, and partial cleanup.
Raw byte relocation under exclusive ownership is not a logical element copy.

Array/function/cell combinations may still form strong reference cycles. This
contract adds no collector and does not turn no-cycle physical-zero evidence
into a claim that unreachable cycles are reclaimed. The Task315 cycle boundary
remains explicit.

## Packet and version cutover

The old callable section is replaced by one u32 count and ordered tagged records.
Each record starts with u32 kind (CALLABLE=1, ARRAY=2, CELL=3), then u32 derived
parameter span. CALLABLE then encodes u32 parameter count, ordered u32
(type, mode) pairs, u32 result and u32 flags. ARRAY/CELL then encode one u32
element ID. The existing packet integer byte order is preserved. All encoded counts and child
IDs are u32 with checked multiplication and minimum-remaining-wire-size checks.
No arena address, object pointer, gate, code pointer, or process-local identity
is serialized. Unknown/trailing payload rejects.

Decode verifies the digest, versions, budgets, node graph, derived facts,
function-local parameter scope, declarations, and complete Checked semantics
before publishing an owned artifact. The digest provides content integrity, not
authentication or semantic authority. A packet with a recomputed digest but an
invalid graph or forged span must still fail.

The unified-pool foundation established schema 5, semantic 14, Value9, Call13
and Program8. The Array operation/fault cutover replaces its current admission
boundary atomically as follows; there is only one current reader/executor.

| Revision | Current cutover value | Reason |
|---|---:|---|
| Checked schema | 5 (unchanged) | Tagged pool and fixed instruction records retain their exact wire traversal. |
| Checked semantic contract | 15 | Tags 63..69, Array operand/place/permission rules and role-aware layout become part of admission. |
| Value ABI | 9 (unchanged) | The carrier and `(TypeArena, local type ID)` identity stay fixed; Array objects obey that existing owned identity. |
| Call ABI | 14 | Typed Bounds detail crosses actions, results and unwind under `xir-resumable-calls.md`. |
| Program ABI | 9 | Instance initialization/sticky failure copy preserves the full fault under `xir-program-instance.md`. |

Semantic15/Call14/Program9 land together with target/native entry checks, cache
identities, emitters, consumers and tests. Reject older and mixed versions,
rather than adapting them. A matching digest, local node index or carrier width
is insufficient. No physical Array backing, place address or cached layout is
serialized. Pool allocation strategy, hashing, arena clone/sharing, COW capacity
and worklist implementation do not independently revise the ABI when they
preserve this frozen observable boundary.

## Required gate responsibilities — implementation qualification OPEN

The named tests below exist in the current CMake inventory. Their current
registration does not mean they already cover the new assertions. Extend them
or add real registered tests, then bind their final names to the owning contract.

| Responsibility | Positive assertions | Negative assertions | Existing extension points |
|---|---|---|---|
| Graph, kind, context | ARRAY<string>, nested ARRAY, ARRAY<CALLABLE>, CALLABLE(ARRAY<T>)→ARRAY<T>, ARRAY<CALLABLE()→ARRAY<T>>, internal CELL<ARRAY<T>> | Duplicate/forward/cyclic edges, unknown kind, invalid range/reserved ID, old CELL flag, wrong span, illegal CELL/public storage, parameter escaping its function | `test_xir_stages`, `test_xir_generics`, `test_xir_checked` |
| Definition-first generics | Copy/store/read/return under ordinary T; declared Sendable entails ARRAY<T> Sendable; closed specialization remaps every nested use | An unused invalid generic body; implicit arithmetic/display authority; unconstrained Sendable; non-storable element; parameter remaining after specialization/seal | `test_xir_generics`, `test_xir_source_generics`, `test_xir_source_admission` |
| Packet and atomic versions | New packet round-trip, owned lifetime after input destruction, no-parser consumer | Old/mixed versions, recomputed-digest malformed graph/span, wrong payload lengths, trailing bytes, budget limits, decode OOM | `test_xir_checked`, `test_xir_checked_allocations`, `test_xir_packet_vm` |
| Runtime identity | Same-Program pure arrays enter another Instance by value without alias pollution | Same-u32 different-element arenas; same-u32 different callable signatures; same-structure foreign arena; nested foreign values; preserved prior result/epoch on rejection | `test_xir_program_vm`, `test_xir_program_native`, `test_xir_calls`, `test_xir_call_native` |
| Transitive authority | Same-Instance function arrays and nested arrays call successfully; pure/empty values obey their admitted boundaries | Foreign/stopped functions wrapped in ARRAY, CELL-held ARRAY or captures; native return/action/inbox bypass attempts; bounded work/scratch exhaustion without partial publication | `test_xir_program_vm`, `test_xir_program_native`, shared `xir_function_cases.h`, `test_xir_calls`, `test_xir_call_native` |
| Metadata/code lifetime | Escaped ARRAY<string> survives actual artifact/Program destruction and code-release observation; ARRAY<fn> keeps only the required callable code lease and releases Instance slots | Borrowed descriptor UAF; arena→Program lease cycle; double release; drop after failed partial construction; stale invocation after stop | `test_xir_values`, `test_xir_value_allocations`, `test_xir_program_vm`, `test_xir_program_native` |
| Nonrecursive release and failure | Deep finite nested arrays release on the common worklist; final backing/element/arena/domain/code counters reach the expected baseline | Allocation/retain failure at every new owner transition, partial element cleanup, saturation, remaining physical allocations in no-cycle cases | `test_xir_value_allocations`, `test_xir_checked_allocations`, `test_xir_source_allocations` |
| Complete source program | Library returns `["red","blue"]`; copy, index replacement and `push(b[0])`; independent VM/native stdout `red\nblue\n2\ngreen\nblue\ngreen\n3\n`; generic ARRAY<string>/ARRAY<i64> instantiations | Wrong element/index type, readonly mutation, missing empty-literal context, illegal generic operations; failure and ownership assertions must not depend only on backend agreement | `test_xir_source`, `test_xir_source_native`, `test_xir_source_mixed`, `test_xir_source_generics` |

The complete-source row also requires the separate array syntax/value/mutation
contract to be frozen; this contract does not invent its index-fault timing or
unimplemented `const T`/move semantics. Windows rebuilds, targeted assertions,
relevant regression/sanitizer/contract gates, and physical release evidence are
required before qualification. No macOS execution or upstream-reference test
execution is claimed here.


## Logical places and owned Array operations

The exact opcode, role, effect and layout authority is `xir-array-values.md`;
the backing/rollback authority is `xir-array-storage.md`. The obligations below
apply to that same family; they do not establish a second representation or
declare the family implemented.

The Array declaration is a value type with one ordinary copyable/storable element
parameter. Array literals, explicit type annotations, logical copies, index get
and set, read-only len/get, ref set/push, parameters and returns are the first
admitted family. Other methods, struct fields and views retain their separate
qualification obligations. Array storage does not require comparison, hashing or
display capabilities. Source binding must use the governed declaration identity;
a user function named len or method named push is not an intrinsic by spelling.

CELL_PLACE and SLOT_PLACE project a typed logical place without a payload, retain,
address, or loan. They cannot be copied, returned, captured, passed as ordinary
values, merged in PHI, or included in owned cleanup offsets. The existing local
place retains its real payload and cleanup. The relevant cell/frame/instance root
must dominate and remain alive until the access. Resolving a projection never
grants another module's mutable-slot permission.

ARRAY_PUSH takes (place, element) and returns unit. ARRAY_SET takes an exact
three-element owned operand-table range (place, i64 index, element), returns unit,
and has no successor payload. ARRAY_GET takes (Array value or readable place,
i64 index) and returns an owned element. ARRAY_LEN borrows the same receiver
forms and returns i64. ARRAY_NEW uses the owned operand table for element values,
including canonical empty zero/zero. Range partition, dominance and budget rules
apply before lowering, as they do to existing calls and output.

Bind a mutation's root place once, evaluate index and all arguments/RHS once in
source order, then resolve the current contents and begin exclusive access. If
the RHS rebinds that same root, the store addresses the new current Array, with
its current length. Do not retain a raw element address across RHS execution or
suspension. Index reads preserve the receiver's pre-index logical view. A borrowed
place optimization requires proof of equivalent evaluation and failure order;
the first source form is restricted to non-failing literal/plain-i64-name indexes.
A synchronous getter may retain the receiver transiently, copy the element, then
drop that receiver before returning: no full Array SSA owner remains to force
every subsequent append into COW. Eliminating a potentially failing retain requires
an independent proof or an equivalent check in the same observable order.

For mutation, validate types, arena/gate authority, bounds and length/capacity
arithmetic; prepare all fallible element ownership; then prepare shared detachment
or unique growth; commit only after the last possible failure. Replacement releases
the old owner after publication. Unique capacity-sufficient writes do not allocate
or copy the whole array. Relocating existing exclusive owners is not a logical copy.
Self-aliasing inputs are owned before reallocating storage. Rollback preserves
the destination's pre-commit logical value and other copies, while earlier RHS
side effects remain. Assignment's returned T is independently owned.

Bounds are i64 0 <= index < length and map to E0430 in the panic/fault channel,
with the signed index and observed length preserved. They must not be reported as
NUMERIC_RANGE, BAD_STATE, business throw or allocation failure. The Call14 and
Program9 detail representation, action verification and sticky propagation are
owned by `xir-resumable-calls.md` and `xir-program-instance.md`; both VM and native
must use that same boundary. Allocation, retain-limit and admission-work failures
retain their distinct existing channels.

Initial cell/slot publication may retain the initial Array SSA owner until frame
exit and therefore force an initial detach. Qualification reports that cost;
it must not hide repeated per-iteration whole-array copies. Existing nonrecursive
drop queues and allocation domains are reused. Required independent source golden:
red\nblue\n2\ngreen\nblue\ngreen\n3\n, from a two-module string Array copy/set/push
program in VM and native. Counts, allocation-fault sweeps, escaped results, stopped
function elements, two instances and final physical-zero checks remain separate
required evidence, not implied by the golden output.


The following existing assertion owners must be extended for the responsibilities
above. These registrations do not declare the new family implemented or passing.

verification-test: test_xir_stages
verification-test: test_xir_checked
verification-test: test_xir_checked_allocations
verification-test: test_xir_generics
verification-test: test_xir_values
verification-test: test_xir_value_allocations
verification-test: test_xir_program_vm
verification-test: test_xir_program_native
verification-test: test_xir_calls
verification-test: test_xir_call_native
verification-test: test_xir_source
verification-test: test_xir_source_native
verification-test: test_xir_source_mixed
verification-test: test_xir_source_generics
verification-test: test_xir_source_admission
verification-test: test_xir_source_allocations
verification-test: test_xir_packet_vm
