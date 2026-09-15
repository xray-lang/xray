# Typed TargetPlan execution contract

TargetPlan schema 60 may carry a canonical per-function instruction table and
an exact per-call-site dynamic-entry expectation table.
Instruction authority is separate from the production AOT family mask: a
verified plan can remain a complete AOT plan while exposing no typed execution
family. A function with zero instruction rows is execution unavailable, never
an empty successful program. The production builder emits a complete group
only for either a verified, capture-free signed-`i64` function whose declared
parameters are all exact signed `i64`, whose blocks are all returns, plain
jumps, or two-way branches, and whose operations are entirely in the supported
family, or the exact managed tagged `Array.push` call family described
below. Every other function emits zero rows; no partial group or fallback is
allowed.

Schema 54 additionally admits one source-backed signed-`i64` overflow-predicate
program family. Three function-qualified predicate rows bind the exact PSC and
SemanticPlan program-call, caller, builtin, callsite, operation, result,
receiver, argument, and stable `ADD`/`SUB`/`MUL` kind facts. Each typed
`I64_OVERFLOW_PREDICATE` instruction carries only the dense predicate-row ID;
the kind is not duplicated in the opcode or reconstructed from a selector.
The independent verifier resolves the target function once, then proves every
row and instruction in a single bounded pass. Both generated VM providers
mechanically call the shared arithmetic core and produce an `I1` intermediate
inside an otherwise signed-`i64` entry. XTP54 serializes the same 112-byte rows
in their own section and rejects XTP53 and earlier schemas. At this checkpoint
C emission remains fail closed for this family until a separately verified
C-emission recipe consumes these rows; the old method-name emitter is not an
authorized product fallback.

The bounded leaf-value aggregate direct-call family is one exact executable
family within schema 58, not a second aggregate-only plan. SemanticPlan 46
program bindings project one target-profile-derived aggregate layout,
caller/callee value and slot rows, one `VALUE/READ` argument, and a caller-storage
result with no adapter or ownership transfer. The callee group is exactly
`PARAM_AGGREGATE`, two `AGGREGATE_GET_I64` rows,
`AGGREGATE_MAKE_I64X2`, and `RETURN_AGGREGATE`; the caller group is exactly two
scalar constants, `AGGREGATE_MAKE_I64X2`, `CALL_DIRECT_AGGREGATE`, and
`RETURN_AGGREGATE`. The independent verifier reconstructs the program type,
field, function, call, layout, value, slot, and instruction joins without a
legacy shape helper, Xi inference, source-name switch, or alternate execution
path. A missing, duplicate, or mutated join makes the entire family execution
unavailable.

Typed VM and AOT consume the same independently verified TargetPlan object and
fingerprint for this family. The VM transports complete positional `i64x2`
values through the exact caller, callee, and caller-storage slots and executes
both generated providers from the shared opcode registry. AOT reaches the same
function and call authority through the frozen PSC function row and SemanticPlan
program bindings, then mechanically projects the exact 16-byte, 8-byte-aligned
native value and direct call. For covered functions and calls, legacy AOT ABI,
value-representation, boundary-step, place/aggregate-reference, name/body
resolver, and runtime-call owners are zero or unreachable; invalid authority
cannot fall back to them. This boundary covers only the exact leaf
`Pair<i64,i64>`-shaped direct-local family and grants no general struct,
container, coroutine, cross-module, or W4 execution authority.

Schema 54 carries one disjoint executable W4 family for the exact PSC7
pointer-free tuple6 product. Its carrier is exactly 48 bytes with 8-byte
alignment and field offsets 0, 8, 16, 24, 32, and 40; ordinal 2 is `u8` and
the remaining fields are `i64`. Two zero-argument callers each use one
`CALL_DIRECT_AGGREGATE` caller-storage edge to the common callee, project all
six ordinals once, and reconstruct all six ordinals once. Both typed VM
providers mechanically consume the verified layout, field, representation,
slot, call, and instruction rows. The dedicated one-shot hosted-fragment
emitter first verifies the complete PSC/Xi/SemanticPlan-to-TargetPlan function
join inside the same call, derives symbols from stable function identities into
a private non-escaping binding, and then emits C mechanically from that binding
and the verified TargetPlan rows. This Xi-bound one-shot emitter is a separate
AOT translation unit and header from the pointer-free aggregate projection.
Only the pointer-free projection belongs to the installed compiler archive;
its archive-link probe calls that projection directly, so a reintroduced Xi or
frontend dependency fails the installed product closure instead of being
silently satisfied by the development core. Generic aggregate projection and
the legacy per-module CEmissionPlan reject every product provenance, including
damaged product type bindings. The ordinary executable artifact route remains
fail-closed because this family does not yet define one unique process entry.
Managed tuple, tagged place/aggregate-reference, dynamic, boxed, name-derived,
and legacy leaf-aggregate routes are forbidden for covered product functions.

The bounded two-module scalar graph is one additional executable schema-58
TargetPlan family. Its sole program-graph row and two
canonical module partitions index global TargetPlan tables and bind the complete
ordered SemanticPlan module set, aggregate semantic fingerprint, entry and
producer identities, exact cross-partition callsite, resolver, argument, and
parameter identities. The cross-partition call uses only
`PROGRAM_DIRECT`/`CALL_DIRECT_I64`; `CALL_MODULE`, per-module TargetPlans, and
cross-plan fingerprint stitching do not exist. Every local semantic index is
interpreted only through its owning partition. Independent verification
reconstructs the complete graph, global row ownership, exact scalar physical
representations, slots, instructions, call and argument policy, debug facts,
capability union, and all inner fingerprints. A hostile mutation remains
invalid after related row and plan fingerprints are recomputed. Typed VM
accepts only the graph-owned entry target as an external root and follows its
`CALL_DIRECT_I64` edge by creating the callee frame from the same plan and
global row namespace. It does not translate local indexes, construct a
  producer plan, or expose the producer as a second root. Cold execution repeats
  independent TargetPlan verification; warm execution requires the one decoded
  cache owned by the exact runtime generation. That cache retains the same plan
  object and binds its intact plan and program fingerprints, freshly recomputed
  canonical module-set fingerprint, 16-byte GCI, exact generation identity, and
  verified global graph rows. A wrong generation, foreign same-fingerprint
  plan, or mutated graph/partition fails closed rather than becoming a miss,
  fallback, or second executable plan.

The AOT direct-call refinement is the first lower consumer of this graph
authority. Schema 6 resolves the caller and callee global function rows through
their owning semantic partitions, records their stable program-function symbol
identities, and binds the unique global `CALL_DIRECT_I64` instruction to the
`PROGRAM_DIRECT` call. Freeze and independent verification re-derive those
facts from the same TargetPlan. A committed program-direct row cannot become a
refusal, legacy resolution, or per-module executable plan: missing or
contradictory evidence fails the consumer. The schema-3 program C-emission
binding then consumes that same verified program TargetPlan. It matches every
partition to exactly one Xi module through its PSC row and attached
SemanticPlan, joins the global functions and direct call to unique Xi nodes
through PSC program-row indexes, and independently projects the complete value
and function-ABI rows for the caller, callee, and every module initializer.
Caller and callee C symbols derive only from stable program-function identities.
Each initializer symbol identity is the domain-separated hash of its canonical
TargetPlan partition `module_identity` and exact SemanticFunctionRecord stable
ID; the module-local initializer ID alone is not a global symbol, and no module
or function name enters the derivation. Missing, duplicate, foreign, or mutated module,
function, value, ABI, call, argument, instruction, or carrier authority fails
closed. The binding is not a second executable plan: product CGen mechanically
consumes its verified views and cannot reconstruct an answer from TargetPlan
machine rows, module names, local semantic indexes, per-module C-emission plans,
or legacy `XaotFuncAbi` state.

For this exact graph, required module initializers remain lifecycle roots, but
the caller is the sole ordinary product-body root and the callee is never
seeded independently. The callee is reachable only through the verified
program-direct edge. The call's exact
`GET_SHARED` callee carrier is independently proved to belong to the caller,
resolve the producer through the frozen dependency/resolver/export join, occupy
the exact shared slot, and have the call's operand-zero use as its only use.
That proved carrier is mechanically elided from both C materialization and the
legacy shared/static reachability graph; it cannot preserve the callee after a
missing or corrupted direct edge. Generated caller C therefore calls the
stable producer symbol directly and contains no module-name, shared-slot, or
runtime import/export lookup.

The graph XTP route preserves the same executable VM authority and AOT lower
authority. Current schema 60 retains the exact graph and partition sections, mutually
excludes ordinary and graph directory shapes, binds the header semantic
fingerprint to the full canonical module set, and materializes only through that
set plus the exact target profile.
`MODULE_PARTITIONS` is a fixed 216-byte row format with at most 256 rows;
`PROGRAM_GRAPHS` is a fixed 340-byte row format with at most one row. Round-trip
is byte-identical, while wrong count/order, duplicate/missing modules, re-signed
graph or partition mutations, and ordinary/graph substitution fail closed. The
ordinary single-module runtime loader accepts no graph plan. The distinct public
program facade admits only the exact two-partition/two-function/one-call/
one-argument direct-`i64` graph and binds the verified Program TargetPlan to the
same live manifest, decoded cache, program fingerprint, module-set fingerprint,
and GCI. The source AOT product builds
and verifies the graph plan, builds and independently verifies its global AOT
direct-call binding and schema-3 program C-emission binding, publishes
deterministic module summaries, and emits and links the covered graph without
constructing per-module TargetPlans. Bounded source-AOT
`execution=cgen-ready` is reported only after the program C-emission binding is
installed successfully. The cold, warm,
dependency-edit, and dependency-revert native oracles execute results
42/42/43/42 from real generated C and require the kept caller body to contain
the canonical direct symbol call and no legacy module/name/shared lookup. This
is a bounded source-product lane for the exact two-module scalar graph; it does
not make the ordinary installed loader graph-aware, admit another graph shape,
authorize dynamic reload or concurrent unload, or claim general `PRODUCT_ACTIVE`.

The scalar representation boundary separately recognizes an exact unaliased
SemanticPlan `Ptr` or `MutPtr` as TargetPlan `RAW_PTR`: its size and alignment
come from the frozen target profile, while ownership is trivial, roots are
absent, and zero is null. Production construction and independent verification
both derive these facts from the frozen SemanticPlan type identity. No mutable
Xi type, value name, or legacy plan is an authority for this representation.

The closed execution family is a signed `i64` program consisting
of constants, parameter bindings, copies, wrapping addition, wrapping
subtraction, wrapping multiplication, bitwise and, or, exclusive or, wrapping
negation, bitwise complement, masked left shift, masked arithmetic right shift,
truncating division, remainder, unconditional jumps, conditional branches, and
returns. A non-empty function group must form an exact table partition in
canonical function and dense row order. Its independent verifier requires every
referenced slot to belong to that function and have identical trivial
signed-`i64` register and memory representations. It also proves single
assignment, canonical arity and unused fields, and that a computation row is
never the last row of a group. Unknown or unsupported instructions fail closed.

The managed executable family is deliberately one exact operation: an
`Array<T>.push(T)` Target call for the exact String, frozen source-class, or
compiler-owned Array element roster. Its receiver is `BORROW`, its element is
`CONSUME`, its storage is `TAGGED`/`DYN_VALUE`, and its result is the
Unit-valued void side effect. Its complete instruction group is two
required parameter rows, `ARRAY_PUSH_TAGGED`, and `RETURN_UNIT`. Per-operand
ownership, the memory-write/may-error effects, the exact Target call row and
argument rows, managed element type, selector identity, slot identities,
and dense row shape are independently rederived by the Target verifier. The
builder and verifier consume SemanticPlan/TargetPlan authority directly; the
VM never reads an AOT CEmission recipe and never derives an executable answer
from a selector spelling.

Generic shared-value and owner-forward layout intents do not invent Array
storage. When the exact SemanticPlan value type is `Array<T>`, production
construction projects the element storage from that semantic type and the
independent verifier reconstructs the same judgement. `NONE`, a stale tagged
producer hint, or a different element family is invalid even when adjacent
rows are re-fingerprinted. This layout rule grants no additional typed
instruction family; execution remains unavailable until a complete family
owns the corresponding rows.

An Array receiver borrowed from a source-class field has one exact outer
carrier authority. SemanticPlan must prove a unique `XI_LOAD_FIELD` result and
an exact Array row; its receiver must name one frozen source class either
directly or through the exact `ref Class` parameter-to-`XI_PLACE_LOAD` chain.
TargetPlan binds that field result as a borrowed dynamic temporary and its
verifier independently re-derives the same class, place-load, field-load,
Array, and unique-definition facts. This does not grant generic instance field
storage, infer a field declaration, or move element-layout ownership out of
TargetPlan.

The second executable family adds only non-suspending SOURCE_EXPORT calls whose
parameters and result are exact signed `i64`, whose ownership is trivial, and
whose adapter is identity. `CALL_ENTRY_I64` names a dense expectation row, not
a process pointer. That row binds the canonical callee ABI fingerprint, target
profile, identity-adapter fingerprint, call row, and parameter count. The ABI
fingerprint has its own entry domain and is never reused from the call-site
fingerprint. Runtime resolution may bind that same frozen identity adapter to
either the typed VM executor or one process-local exact-i64 native executor.
The native pointer is absent from the artifact and the dispatcher consumes a
frozen adapter binding copied only after the entry cell has acquired the exact
generation pin. This is the scalar identity subset of hosted VM/AOT calls:
native aggregates, Buffer, INDIRECT_CALLABLE, owned or object values,
non-identity adapters, and suspending calls remain unavailable.

Control flow is proved from the rows, never declared alongside them. There is
no target-level block table: a basic block begins at the group's first row and
after every terminator, so every block ends in a terminator by construction and
the group's last row must be one. A jump target is a row index relative to the
group's first row, and it is accepted only when it is exactly the first row of
a block of that same group, which is what forbids an edge into the middle of a
block. Both edges of a branch are carried explicitly in its immediate, so
neither depends on row adjacency, and a jump must leave the unused half of its
immediate zero so it cannot hide a second edge. Every block must be reachable
from the entry; an unreachable block is refused rather than carried, because
the intersection over its absent predecessors would read as "everything is
defined".

Once a function has several blocks, where a value is defined and where it is
read are separated by control flow, so use after definition is proved by a
definite-assignment fixed point rather than by row order: a slot is defined on
entry to a block only when it is defined on leaving every predecessor, the
entry block starts with nothing defined, and the fixed point is iterated in
reverse postorder and refused if it has not settled. Reading a value that only
one arm of a branch defines is therefore rejected even though the row defining
it exists in the group. This one judgement is what the production builder
admits against, so a group can never be emitted that verification would then
refuse. Blocks per function are capped so the proof's per-block slot bitmaps
are bounded rather than trimmed.

A conditional branch tests an ordinary defined `i64` slot against zero and
takes its first edge when the slot is not zero. There is no boolean slot,
machine representation, or comparison row in this family, so a condition needs
no proof the slot proof does not already give it, and every `i64` value selects
an edge. A block whose condition is not exact signed `i64` leaves the whole
function unavailable rather than being coerced.

A shift row takes its count modulo 64, which is the language rule and the
same shared shift owner the bytecode VM, the AOT runtime, and constant folding
already consume, so the executor cannot diverge from them and can never reach
C's undefined shift. There is no immediate shift form: the verifier rejects a
non-zero immediate on a shift row, so a count always arrives through a defined
exact-`i64` slot of that function and is masked on the way in. No further
static range proof exists or is needed, because the language leaves no `i64`
count undefined. The right shift is arithmetic; only exact signed `i64` rows
are admitted at all, so an unsigned shift is unavailable rather than silently
zero filling.

Division and remainder are the only rows with an error edge, and the edge
belongs to the executor. A divisor is a runtime slot value, so no static proof
can exclude a zero; the verifier proves the row shape and rejects a non-zero
immediate, so there is no immediate divisor form that could carry a zero the
executor never inspects. On a zero divisor the program stops with a status
that names the operator, distinct from every status that reports an
unacceptable plan or call, and leaves no result, so a caller can raise the
exact panic the language names for each. The other case C leaves undefined is
defined rather than refused: `INT64_MIN` divided by `-1` wraps to `INT64_MIN`
and `INT64_MIN` modulo `-1` is zero, through the same shared helpers the
bytecode VM, the AOT runtime, and constant folding use. Division truncates
toward zero and a remainder takes the dividend's sign. Only exact signed `i64`
rows are admitted, so an unsigned division is unavailable rather than reading a
`u64` bit pattern as a negative dividend.

A parameter row is a definition, not a computation: its immediate is the
incoming argument ordinal and its result slot is the function's parameter
slot. Nothing is implicitly live at entry, so reading a parameter is proved by
the same use-after-definition rule as any other value. The builder commits a
group only when the frozen parameter record and the operation agree on
ordinal, function, exact type, and SSA value, and only when every declared
parameter is bound exactly once. The independent verifier separately proves
that argument ordinals are unique and dense from zero, that only a parameter
row may define a parameter-role slot, and that the count of parameter rows
equals the number of parameter-role slots the function frame declares. The
executable family caps parameters at 64 so that density is proved without
allocating.

Instruction rows participate in the TargetPlan fingerprint as exact 32-byte
canonical rows. XTP schema 60 preserves the bounded sequential compact stream
introduced by v34; its directory entry carries the expanded row count, compact byte
length, `COMPACT` flag, and zero row size. Canonical ULEB128 and signed ZigZag
payloads plus the format-only superinstruction registry are the sole wire
authority. Materialization expands every token back to the same primitive
TargetPlan rows before verification; the decoded cache and dispatcher consume
only those rows and never observe a wire token. Text dump and diff use the same
sequential iterator rather than random row-size access. There is no v33
reader, translated row, alternate stream, or compatibility path.
The internal typed dispatcher has three disjoint request carriers. The scalar
entry accepts a verified plan, its exact fingerprint, a derived nonzero scalar
execution family, a positional signed-`i64` argument vector, exact typed-frame
slot identities, an optional runtime-only debug session, and an optional
immutable decoded cache for that exact plan object. A dynamic-call scalar
request additionally
carries the exact caller generation identity and its generation-owned dynamic
entry context. Generation identity is execution authority; a debug session may
check and observe it or stop at an exact TargetPlan debug fact, but can never
supply or alter it. The context validates its exact
immutable owner plan and generation before any frame, trace, cache, or pin side
effect. The same request must also select
exactly one generated executor provider: the generated switch or the generated
function table. Zero, an unknown provider, an unknown opcode, or any mismatch
between the opcode's complete canonical contract and its provider binding fails
closed. There is no default provider, alternate execution entry point, or
compatibility wrapper. Both providers are expanded from the same dense
`xisa/target/vm_ops.def` authority and call the same handlers; the function
table uses only standard C function pointers and sequential initialization so
MSVC and the other supported C11 toolchains consume the same generated rows.
Without a cache it independently
recomputes target-content integrity and instruction validity. With a cache it
requires the same plan pointer, schema, recorded fingerprint, and
caller-required fingerprint before consuming any row. A program-graph cache
additionally requires the complete same generation identity and re-derives the
canonical module-set fingerprint and GCI from the retained intact plan; it never
embeds a fixed partition-count array. It never treats a miss or mismatch as
authority to build or execute. Neither path inspects Xi. The verified rows are
the only signature it honours: the
argument count must equal the number of parameter rows, and a shorter,
longer, or absent vector is rejected before the frame exists rather than
truncated, padded, or zero filled.

The leaf-aggregate entry accepts only
`XR_TARGET_EXECUTION_LEAF_AGGREGATE_I64X2`, the exact verified plan and
fingerprint, a complete positional aggregate argument vector, one result
carrier, and exactly one generated executor provider. It derives every slot and
call edge from the plan, never from Xi or SemanticPlan, and rejects a missing or
extra argument, wrong fingerprint, wrong result slot, unknown provider, or any
noncanonical instruction group before publishing a result.

The managed entry accepts only the exact tagged push execution family and a
mutable two-element `XrValue` vector. The receiver remains borrowed. The
element owner moves into the typed frame, then into the shared array kernel;
success leaves the caller argument cleared and the array owning the exact
carrier. Every rejection or kernel failure restores that exact carrier to the
caller and preserves array length, capacity, and contents. The shared kernel
returns an explicit status and rejects a non-array receiver, slice mutation,
or incompatible typed storage before mutation; allocation failure is a
separate status and restores the caller owner. Neither entry has a legacy VM
opcode, AOT, generated-C, or name-derived fallback.

The dispatcher carries an instruction pointer that starts at the group's first
row and moves only where a row says, repeating the verifier's target bound so a
table that changed underneath cannot move it out of the group. A verified
program may loop forever, since no static proof could forbid that without also
forbidding ordinary loops, so executed rows per call are capped by a fixed
budget rather than a wall clock. Exhausting the budget stops the call with its
own status, distinct from every status that reports an unacceptable plan or
call, so a plan can never hang the caller that ran it and the same call refused
once is refused every time.

The coroutine entry point is a separate persistent runtime object rather than
an alternate interpreter. It accepts only the generated scalar-`i64`
coroutine family with at least one exact state row and zero root or cleanup
rows. Creation retains one verified TargetPlan, builds one immutable decoded
cache, allocates one packed typed frame, and copies the exact positional
arguments once. `SUSPEND` binds the state named by its row and returns without
destroying or replacing that frame. Resume first consumes the one-shot
continuation authority stored in the state row, enters that exact decoded block
entry, and continues with the same frame, decoded cache, global step budget,
and provider. Scalar values that are live across suspension remain in their
verified packed slots; no save/restore value array or tagged copy is built.
Entering the continuation before bind+resume, resuming a different
state, reusing a consumed continuation, or targeting a non-block row fails
closed. Normal return, program error, and cancellation move the object to a
terminal state; the sole owning `free(**)` operation then releases its frame,
cache, argument copy, and retained plan and clears the caller's slot. A failed
create preserves a nonempty owner slot rather than overwriting its sole handle.
The coroutine object is single-owner and its APIs cannot be invoked
concurrently. The one-shot scalar API rejects this family;
there is no hidden auto-resume, legacy retained stack, bytecode translator, or
source fallback. Dynamic entries, debug sessions, generation bindings,
managed roots/cleanup, yielded values, child suspension, native helpers, and
scheduler policy remain unavailable at this boundary instead of being guessed.

The decoded cache may be published only after full TargetPlan and instruction
verification. Per function it copies the immutable instruction rows, generated
opcode contract binding, decoded control targets, parameter count, and the
verified basic-block partition and successors. It does not select or duplicate
representation, call, adapter, allocation, root, cleanup, or ownership policy.
Function, row, block, and byte totals have fixed hard ceilings checked before
expensive construction and again against the exact allocation shape. Published
storage is read-only, retains the exact plan for its whole lifetime, and may be
shared concurrently. The generation-owned program cache stores one graph row
and canonical identity evidence, not copied per-module executable plans or a
fixed partition array. Exact lookup rechecks TargetPlan integrity, the current
graph and partition shape, the canonical module set, GCI, and the full
generation identity before exposing a decoded row. Cached and uncached dispatch
use the same generated
handlers, frame operations, call-depth and step counters, error statuses, and
result publication. Provider selection is in-process execution policy rather
than serialized plan authority, so it changes no TargetPlan or XTP schema and
does not select representation, calls, ownership, or cleanup semantics.
The native direct-call path has an exact hard depth budget of 128 frames. The
limit is enforced before allocating or linking the child frame, and both
generated providers return the dedicated call-depth status at the boundary
instead of consuming the host thread stack.

The runtime dynamic cache is one generation-owned slot per expectation row.
Its slot stores only a retained runtime handle, a stable registry-row reference,
and the observed per-key atomic epoch; artifact bytes contain no raw pointer.
A warm cache lookup takes only that slot's mutex and an acquire-load of the row
epoch: it does not scan the registry or acquire the generation-authority mutex,
and an unrelated export mutation does not invalidate it. The independently
required entry acquisition then owns the generation pin. Every miss scans under the
registry authority, retains a candidate, rechecks the complete expectation,
acquires an entry-cell call token, and independently re-verifies the callee
instruction program. No failure is negatively cached, so a miss or stale slot
can never make an invalid plan legal. Replacement and unpublication serialize
through the registry mutation gate; stable tombstone rows keep epoch references
valid until authority teardown. Every normal, program-error, trace-error, and
budget exit releases exactly one generation pin. Token release is transactional:
failure restores LIVE and preserves the lease for retry instead of orphaning a
pin. Hard site and byte budgets bound both registry and cache storage.

TargetPlan schema 60 is a hard cutover from schema 59 and every earlier TargetPlan
schema. It requires SemanticPlan schema 49, program-provenance schema 5, and
its exact generated Xi operation registry. Schema 54 preserves the PSC7 leaf
value-product family whose canonical Xi construct/project proof
has the exact x64 48-byte/8-byte layout and ordinal-bound target rows; it does
not widen the existing 16-byte/8-byte Pair family. It also assigns the exact
direct-local borrowed `ref i64` call row its scalar-ref v1 meaning, including
stable identity, caller/callee scalar slots, scalar call storage, ownership,
transfer, and addressability. It additionally makes the caller's exact
`LOCAL_ADDR` value a `RAW_PTR` row instead of accepting the schema-51 builder's
incorrect subject-`I64` row. This changes the valid artifact set even though no
wire row changes size, so v51 and every earlier artifact are rejected rather
than reinterpreted.
The direct-local tagged-ref family separately binds an addressable borrowed
`XrValue *` boundary for exact Arrays, StringBuilder values and source-class
instances. Its caller storage is either the dynamic value named by one exact
`LOCAL_ADDR` or the borrowed raw-pointer parameter named by one exact
parameter-origin operand. In the latter form TargetPlan and CEmission preserve
the raw pointer through the nested call; they do not reinterpret the parameter
as a dynamic value or take its address again. Builder, independent verifier and
C projection each reject a missing parameter identity, wrong origin, duplicate
parameter row, representation drift, ownership drift or nonlocal alias. This
is not general pointer forwarding and grants no SOURCE_EXPORT or ref-result
execution authority.
The generated builtin receiver registry and stable method-symbol registry are
the sole authority for exact `Map<K,V>.entriesIterator()` calls and their
bounded `Iterator<(K,V)>.hasNext()`/`next()` continuations; selector spelling
is diagnostic only, and an `Iterator<unknown>` result fails closed. Schema 34
changed the instruction opcode carrier to an unsigned
16-bit stable ID while the
canonical instruction row remains exactly 32 bytes by shrinking its reserved
tail to one byte. The generated target instruction registry is the only opcode
authority consumed by the builder, verifier, artifact renderer, and dispatcher.
XTP schema 60 preserves the compact instruction stream introduced by v34,
appends the exact 144-byte entry-expectation section after all prior tables,
widens each coroutine state with its function-local resume-instruction
authority, and preserves the exact lifecycle root-map, root-slot, and cleanup
rows. The generated registry appends `SUSPEND` as opcode 28 and the exact
managed push rows `PARAM_DYN_BORROW`, `PARAM_DYN_OWNED`,
`ARRAY_PUSH_TAGGED`, and `RETURN_UNIT` as opcodes 29 through 32 without
renumbering any prior opcode. Stable opcodes 38 through 42 remain `CONST_U8`,
`VALUE_PRODUCT_INIT`, `VALUE_PRODUCT_SET_I64`, `VALUE_PRODUCT_SET_U8`, and
`VALUE_PRODUCT_GET_U8`. Schema 54 preserves their exact typed execution: the
generated switch and function-table
providers execute the complete 48-byte carrier only after independent
program verification grants the tuple6 execution-family mask. The same
verified rows bind the hosted-fragment AOT/C emitter; legacy managed tuple,
tagged place/aggregate-reference, name, and boxed paths reject this family.
Schema 54 also appends `CALL_NATIVE_LEAF_I64` as opcode 44 and extends each
call row with a generated numeric native-leaf kind plus a stable native-callee
identity. SemanticPlan 44 admits only an exact direct `XI_IMPORT_REF`/`XI_CALL`
shape selected by the generated standard-library registry; the first admitted
leaf is the zero-argument signed-`i64` process-id query. Target construction and
independent verification rejoin that semantic operation, registry entry,
result representation, call row, and one typed instruction. Both VM providers
dispatch the numeric leaf through the runtime-neutral scalar provider. AOT
first excludes the row from the direct-local family, then projects the same
verified numeric leaf to its portable C scalar helper. Neither backend may use
module/member spelling, a native-module factory, the tagged wrapper, or a
legacy dispatch table to recover a covered leaf. Missing, duplicate, unknown,
wrong-identity, wrong-shape, or live-Xi-drifted authority fails closed. No v53
artifact is widened or translated into this authority.
A suspend row names both the exact coroutine
state and that state's independently frozen resume instruction. Unknown,
duplicated, redirected, mismatched, or absent rows fail closed. No v50 or
earlier artifact is widened, translated, or accepted by a compatibility reader.

Schema 30 is a hard cutover from v29 and all earlier schemas. Every CEmission
recipe argument now freezes its ordered logical semantic identity separately
from the exact source semantic identity consumed by C. Exact String concat
operands remain tagged, while an exact non-null, non-aggregate `u64` operand is
admitted only through matching TargetPlan U64 register and memory rows and the
direct `xrt_strpart_init_u64` recipe. CGen must additionally prove the exact
post-freeze AOT BOX adapter before eliding its logical tagged local. There is no
v29 reader, translated row, tagged fallback, or selector/type-derived repair.
Schema 30 also preserves the
exact semantic field-name identity of every named aggregate and all existing
ADT-enum, Array-intrinsic, String-runes, and Iterator-rune authorities while
replacing the narrower source-namespace family with exact SOURCE-import
storage and dense SOURCE_EXPORT call-argument rows. Named value aggregates
continue to derive their C spelling from frozen SemanticPlan/TargetPlan
authority without mutable Xi type, name, or arity inference. The source-backed
managed value-aggregate boundary is narrower: one borrowed direct-local `READ`
argument may carry an inline aggregate whose recursive field graph contains
only scalar/unit-enum fields and managed String or payload-enum leaves. Its
Target layout freezes transitive root cardinality plus distinct clone/drop IDs
derived from the SemanticPlan schema, complete generation fingerprint, and
semantic type ID. Independent verification re-derives the graph and both IDs.
Because the boundary borrows, the call executes no clone/drop and creates no
cleanup; missing, stale-generation, swapped, or mutated lifecycle authority is
rejected rather than repaired by AOT, VM, or runtime inference. It grants no
general struct ABI, owning transfer, class/container/view/nullable field, or
second lifecycle planner. The source-backed
ADT-enum family provides
payload-bearing constructor dispatch. The family binds declaration, member,
nominal layout, discriminant, ordered payload types, namespace receiver,
ownership, and direct-local argument/return relations. Call-argument rows bind
distinct caller and callee physical representations while requiring the same
tagged ABI. It grants no mutable Xi type/name/arity inference, generic method
dispatch, object-body, root, cleanup, or fallback boxing authority.
Schema 17 added an exact borrowed `Slice<u8>` view of String storage.
The view binds the frozen Semantic intrinsic and source root to the exact
`xray-target-string-byte-slice-view-v1` call identity; it grants no generic
String method or slice construction authority. Schema 18 adds a sealed
`Iterator<rune>.hasNext` bool recipe whose receiver is the exact prior
`String.runes` result and whose fixed runtime helper preserves pending-error
polling. C emission schema 30 adds the separate native-rune `next` recipe only
when the receiver has that same unique frozen `String.runes` producer, plus
the exact native-u32 `rune.toUInt32` recipe for any exact canonical Rune SSA
receiver. TargetPlan independently reconstructs the exact method, type,
ownership, receiver identity, native-Rune binding, and dedicated call row;
refinement then re-proves that frozen binding instead of demanding a particular
Rune producer. The owned one-rune String result of exact `rune.toString`
admits only the same-function unique required Rune parameter or the frozen
`String.runes` iterator producer; parameter function range, ordinal, type,
mode, ownership, transfer, and value identity are independently checked. The
exact native-bool `rune.isWhitespace` recipe remains under
the iterator receiver proof. None grants general Iterator or Rune dispatch. It
separately adds the exact owned String
range-slice recipe only for a frozen required String parameter or exact String
literal receiver plus two ordered i64 bounds; this grants no generic String
dispatch. C emission schema 30 also projects the first non-empty cleanup
family: one exact explicit release of the fresh owner produced by a frozen
String concatenation. The Target cleanup row binds the release operation and
owned dynamic slot; the C row binds that same operation and slot to
`xrt_release`. A missing, extra, reordered, or mutated row fails closed, and
the C generator may not infer this authority from selector spelling, live
types, or arity. TargetPlan schema 36 expands this cleanup family for
an exact String concat owner live across one coroutine state. It binds the
dynamic owned slot into that state's `SUSPEND|CANCEL|EXIT` root map, preserves
the normal explicit-release cleanup, and adds one `CANCEL|EXIT` release keyed
by the state operation. C emission schema 30 continues to project only the
normal cleanup row to `xrt_release`; it does not claim coroutine execution.
A missing, extra, reordered, or mutated root or cleanup row fails closed, and
neither the C generator nor the typed frame may infer this authority from
selector spelling, live types, or arity. Target builder and independent
verifier each construct one sorted lifecycle projection and merge it with
operation traversal; all source rows, state-by-release pairs, and sort work are
checked against the common work ceiling before allocation. The registered
projection gate rejects nested entity/operation/release scans. Schema 16 added a sealed
`StringBuilder()` constructor call whose
identity binds the exact Semantic allocation ID and whose result is an owned
dynamic slot. It grants no generic builtin call authority. Its
schema-58 `StringBuilder.clear()` family is a separate exact receiver-alias
method authority: the frozen `clear` symbol, one exact `StringBuilder`
receiver, zero call arguments, and the receiver-alias result must all match.
The result slot follows the verified borrowed-or-owned SemanticPlan return
facts. It has no semantic call-target row, no inferred argument row, and no
generic method fallback. Its
`Channel.close()` receiver is a dispatch target,
not a call argument or authorized frame slot, and it grants no general method
ABI or typed execution path. The String-literal family's
dynamic/owned/tagged row describes only an exact frozen String literal's outer
value and grants no object allocation, root-map, cleanup, tuple, or general
owned-String authority. The closure-storage family's dynamic/owned/tagged row describes
only the outer `XrValue` storage of an exact no-capture heap closure. It grants
no typed instruction, callable-body, allocation, root-map, root-slot, or
cleanup execution authority; the scalar dispatcher continues to reject it.
The direct-local-callee-storage row is a borrowed dynamic outer `XrValue`
token. Independent builder, Target verifier, and AOT materialization verifier
prove that every use is operand zero of the same frozen direct-local call and
that the live shared slot names the unique canonical child of the first lexical
slot owner. A caller-local store must dominate the load; the only parent-scope
exception is a unique module-root entry-prefix initializer before any
activation-shaped operation. Thus a root-owned sibling helper is exact without
turning arbitrary shared values into call authority. It grants no closure
allocation, body, root, cleanup, or indirect-call authority.
The direct-local-GO-callee-storage row is a distinct borrowed dynamic outer
`XrValue` token. Three independent reconstructions prove one canonical
closure initializer, its ownership of the named shared slot, unique canonical
local child and signature, and that every use is operand zero of `XI_GO` for
that same child. Shared slots form one module-wide table, so the initializer
is identified by slot index alone and must live in the scope owning that
slot -- the child's own parent. A caller-local store must dominate the load;
an owning-scope store must instead be that scope's entry-prefix initializer
before any activation, which is what makes a module-level helper started from
a nested function exact.
It grants no GO task-result, child-task object, closure allocation/body, root,
cleanup, argument storage, or coroutine execution authority.
The direct-local-GO-task-result-storage row binds that separate result to a
borrowed dynamic/tagged temporary only after the shared Task nominal judgement
and the independently reconstructed GO callee identity both succeed. `AWAIT`
consumes the same tagged carrier without a representation adapter. The runtime
executor still owns the task object, so the row grants no allocation, root
slot, ARC cleanup, scheduling, child body, or executable coroutine authority.
The Channel-allocation-storage family binds an exact frozen `XI_CHAN_NEW`
result to owned dynamic outer `XrValue` storage and exact identity-copy aliases
to borrowed dynamic outer storage. Independent builder, Target verifier, and
AOT materialization verifier separately reconstruct the canonical allocation
key/id, Channel element and capacity types, ownership/provenance, and copy
shape. This family supplies no Channel object-body layout, allocation
execution, root map, root slot, cleanup, transfer-plan, tuple, or general
object authority.
The Channel-receive-storage family binds only an exact frozen
`XI_CHAN_TRY_RECV` scalar result to trivial native storage after independently
proving that its receiver belongs to the Channel-allocation identity chain and
that `Channel<T>`'s sole child is the result type. It grants no receive
scheduling, Channel body, ownership-transfer, aggregate, tuple, root, or
cleanup authority.
Schema 17 also preserves SOURCE_EXPORT dispatch authority only when the caller's
verified SemanticPlan is accompanied by its exact ordered verified dependency
plan vector. The Target call row binds dependency ordinal, public export ID,
and dependency callee stable ID, while keeping the target-local callee index
absent. The matching coroutine row binds the same call and result slot. This
is public-wrapper identity and suspension authority only: it supplies no
cross-module argument slots, private/native callee, child frame, roots,
cleanup, drop, cancel, or executable call ABI. Standalone materialization and
all ungrounded method calls remain fail closed.
The dedicated SOURCE-import-storage rows cover only borrowed dynamic outer
`XrValue` tokens in either the exact namespace
`IMPORT_REF -> identity COPY* -> SET_SHARED -> GET_SHARED -> identity COPY*`
receiver chain or the exact named-export
`IMPORT_REF(member) -> SET_SHARED -> GET_SHARED` callee chain used by the same
SOURCE_EXPORT call. Identity-COPY chains are bounded,
acyclic, and same-function; every endpoint and COPY keeps its own exact slot
identity and unique expected consumer. Three independent reconstructions prove
dependency/module identity, unique shared slot, complete use sets, and receiver
binding. Each SOURCE_EXPORT argument row binds the dependency parameter stable
identity and ordinal to the caller operand, semantic value, slot, mode,
ownership, transfer, and machine representation. An exact `ref` row alone
authorizes the C projection's additional pointer level. These rows grant no
imported module object body, allocation, root, cleanup, guessed member lookup,
dependency activation, unrelated argument ABI, or cross-module frame.
The C emission projection schema 40 mechanically spells all verified dynamic
families as exact `TAGGED`/`XrValue` rows. For an exact String literal it also
owns the immutable literal bytes and the explicit String-view materialization
recipe. For exact `XI_CHAN_NEW` it owns the helper spelling and capacity
semantic-value recipe. For exact `XI_CHAN_TRY_RECV` it owns the receiver
semantic value and exact scalar unbox helper spelling. Sync and coroutine CGen
consume those recipes mechanically and have no Xi/type or legacy fallback.
The independent verifier reconstructs expected literal bytes only from the
frozen SemanticPlan bound through TargetPlan and rejects missing, extra,
reordered, wrong-kind, wrong-spelling, wrong-operand, wrong-recipe, profile, target, and
projection fingerprint mutations.
For ordinary multi-module programs, the projection API also receives the
owning SemanticPlan explicitly. Schema 40 restricts value, call-argument, and
cleanup rows to its canonical TargetPlan partition and hashes that semantic
fingerprint into the projection. A module-local value ID is never looked up in
another partition, and there is no target-only compatibility entry point.
The same partition discipline governs AOT representation refinement and live
Xi binding before CGen consumes a per-module projection.
It also projects an exact raw-pointer TargetPlan binding without consulting a
live Xi type. When and only when the same semantic `LOCAL_ADDR` value is bound
by an exact SOURCE_EXPORT `ref` argument row or the schema-58 direct-local
scalar-ref-v1 row, the projection adds one C pointer level. The latter freezes
an `int64_t *` function ABI with an `I64` pointee while its caller storage,
callee storage, and call argument machine rows remain `I64`. The C consumer
cannot infer that level from an import name, callee type, or legacy
representation plan.
For the sealed signed-`i64` overflow-predicate family, schema 40 instead owns
an exact native `uint8_t` recipe for each verified function-qualified Target
row. The recipe freezes the ordered receiver and argument semantic identities,
the add/sub/mul discriminant, and the corresponding shared arithmetic-core
helper symbol. Independent verification reconstructs those joins from
TargetPlan; AOT refinement keeps operands in `I64` and results in `I1`, and
CGen emits the helper and direct native condition with no selector read,
runtime method dispatch, boxing, or legacy fallback.
For the sealed `StringBuilder()` call it owns the zero-operand
`xrt_strbuf_new` materialization recipe. Sync and coroutine CGen have no
name-based fallback for that constructor. For the exact String byte-slice view,
it owns the `xr_span_t` representation, source semantic value, and fixed
`xrt_span_from_string_bytes` recipe. CGen cannot recover these from selector
text, aliases, mutable Xi types, or legacy representation state.
For an exact payload-bearing ADT-enum constructor it owns the immutable layout
ID, discriminant, type/member spellings, ordered nonempty payload field names,
namespace receiver identity, and declaration-ordered payload semantic
identities. CGen emits the aggregate-box recipe only from that verified
projection and cannot reconstruct type, selector, arity, field order, or a
source-to-slot permutation from Xi.

The runtime generation authority exposes this family through one public
product route: exact XSM bytes construct runtime-owned semantic and native
profile authority, which may load their matching XTP into a sole-function
scalar-i64 generation and execute function 0. PREPARE requires exactly one
canonical function, this exact nonempty execution family, the typed-frame
schema/family closure, and no storage, allocation, call, root, cleanup,
adapter, or coroutine execution authority. Execution requires healthy ACTIVE
state and a balanced in-flight-call pin. That route carries no argument
vector, so a generation whose sole function declares parameters fails closed
instead of executing against implicit zeros. This adds no public CLI, export
selection, or general typed VM instruction coverage, calls, aggregates,
ownership, exceptions, coroutines, or complete typed TargetPlan VM execution.
The control flow it does cover is jumps and two-way branches between blocks of
one function; there is no phi, no comparison row, and therefore no way to merge
a value produced by two arms, which leaves any function that needs one
unavailable.

The required `COROUTINE_STATE_CALL` family remains independent of this
dispatcher. SemanticPlan schema 32 may additionally freeze one exact
live-across owned-String owner/root/drop relation, and TargetPlan schema 36
materializes its root slot plus normal and `CANCEL|EXIT` release cleanups. The
typed frame consumes that lifecycle through explicit root/resume/cleanup
operations, but scalar dispatch proves that the selected function has zero
root-map, cleanup, and coroutine rows before creating any frame and repeats
zero lifecycle bytes before destruction. This adds no child-frame, scheduler,
arbitrary spill type, or general coroutine execution authority.

Generator preprocessing caches only immutable lexical content and lexical error
reasons keyed by the exact source string, with at most 1024 retained entries.
Diagnostic context is attached by the current caller after lookup. Neither
file identity, include resolution, governed bindings, semantic validation nor
successful proof publication is a cache result. Changing source content requires
a new lookup; eviction may repeat work but cannot alter acceptance. The
generator self-test checks cross-context reuse, changed-content invalidation,
exact error context and exit status, capacity, and recomputation after eviction.

Terminal-selector discovery may likewise cache only immutable per-function route
evidence keyed by the function's exact parsed metadata and text. Every caller
still recomposes the complete source census and reruns the governed binding and
acceptance rules; a cached PASS/FAIL verdict is forbidden. The default generator
self-test remains exhaustive: all sixteen suites and all ninety-two registered
lowering negative cases execute, and the case manifest rejects an omitted,
duplicated, or unregistered case. `test-fast` is a separate edit-feedback profile
which runs the real positive baseline, canonical-output stability/staleness checks, all other
non-Ninja suites, and an exact cross-category subset of four real lowering
mutations. Its JSON record
names the profile and selected cases. The fast profile cannot replace the
exhaustive test in periodic or release qualification, and neither profile may
change acceptance semantics, cache a validation verdict, or weaken failure retry.

Xi lowering build scheduling uses a separate exact input fingerprint, not a
cached validation result. A broad depfile watches the governed include
namespaces, compile closure, and every AOT discovery source. When one of those
paths changes, a descriptor-bound scan captures the closure and discovery set
twice, rejects concurrent or unsafe path substitution, and atomically publishes
the fingerprint plus its completed-scan stamp. The full semantic validator
depends on that fingerprint and its exact file inputs, so an unrelated content
edit may leave the semantic proof intact while an addition, removal, include
shadow, projection change, or actual consumer change invalidates it. A scan
failure publishes no completed stamp. The fingerprint is only a scheduling key:
it grants no lowering acceptance and cannot replace the exhaustive validator or
either generator self-test profile.

Evidence:

- `test_typed_dispatch` proves zero-row rejection, wrapping scalar execution,
  production construction and unsupported-function omission,
  SemanticPlan-independent execution, exact XTP roundtrip execution,
  fingerprint/content rejection, prior-schema rejection, and fail-closed mutation of
  opcode, def-use, row identity, function identity, and return structure. It
  also proves that a two-parameter function emits dense argument ordinals,
  that both arguments reach the executed program in the right positions, and
  that a short, long, or absent argument vector is refused. It proves the
  shift rows against written-out expectations rather than against the shift
  helper itself: sign-replicating right shift, counts of 64 and 67 selecting
  the same shift as 0 and 3, a left shift into the sign bit, and a swapped
  argument pair changing the answer. It rejects an immediate on a shift row,
  a shift missing its count operand, and a count read before its definition.
  It proves the division rows the same way: truncation toward zero, a
  remainder carrying the dividend's sign, both `INT64_MIN` by `-1` edges, and
  a zero divisor in either operand position stopping the program with the
  status for that operator and no result, after the verifier admitted the row.
  It rejects an immediate on a division row, a division missing its divisor
  operand, and a divisor read before its definition.
  It proves control flow on programs the production builder emits from several
  blocks. A branch program returns opposite differences from its two arms, so
  taking the wrong edge negates the answer; zero and only zero takes the second
  edge, and a negative condition and one whose low 32 bits are zero both count
  as nonzero. A jump program chains three blocks, each reading a value the
  previous block defined. It refuses an edge past the end of the group, an edge
  back into the middle of a block, an edge whose only fault is landing on a row
  that does not begin a block, a branch whose edges leave the other arm
  unreachable, a jump carrying a second edge in the unused half of its
  immediate, a jump carrying an operand, a condition read before its
  definition, a last row that is not a terminator, a jump over the block whose
  value the next block reads, and one arm reading the value the other arm
  defines. Weakening the definite-assignment, block-entry, or reachability rule
  in turn makes exactly those refusals stop firing, so each assertion is
  attributable to its own rule. It also proves that a program whose last row
  jumps backward verifies and then stops on the executor's step budget instead
  of running forever. Every executable KAT runs independently through the
  generated switch and generated function table and requires identical value,
  error, termination, step-budget, and call-depth outcomes. The generated
  contract KAT covers every current opcode and refuses unknown providers,
  unknown opcodes, and complete-contract mismatches.
- `test_target_plan` proves the production managed tagged `Array.push` group
  for frozen source-class and nested Array elements, exact DYN representation
  and per-operand ownership, both generated providers, the mandatory
  runtime-kernel capability, and success ownership transfer. The nested case
  also refuses an instance carrier before an Array element can enter the
  frame. A missing kernel is refused before the frame can acquire the element;
  the runtime-only VM archive therefore contains no hidden object implementation
  or unresolved hosted-runtime symbol. The test mutates
  parameter ownership, operand slots, call identity, Target call
  ownership/storage/type relations, and instruction shape independently.
  Invalid receivers, slices, and typed storage mismatches prove fail-closed
  carrier restoration without array
  mutation.
- `test_xi_program_semantic` proves the source-backed leaf aggregate instruction
  groups, unique declaration-owned aggregate type identity, and exact
  layout/value/slot/call joins; it proves same-plan and same-fingerprint parity
  through both typed VM providers and the AOT boundary, plus fail-closed field,
  layout, call, slot, instruction, PSC identity, type-modifier, foreign-shape,
  foreign-source, role-specific return/parameter/construction/call-result/call-argument,
  and builtin/scalar-carrier mutations. It invokes the independent instruction verifier
  directly to reject zero groups and caller-only or callee-only coverage, and
  proves that valid leaf authority with a noncanonical SemanticPlan operation
  shape is rejected by the builder rather than published as an empty executable
  family.
- `test_xaot_driver` proves that the source-backed leaf aggregate functions use
  TargetPlan ABI ownership, publish no covered legacy value plans, emit the
  direct native aggregate call and field operations, and contain no covered
  `XrValue`, `XR_TAG_PLACE`, `XR_TAG_AGG_REF`, dynamic lookup, runtime-call, or
  legacy entry-cell residue. The test builds the source through the shared-library
  AOT route so the exported root is part of the proven reachable program.
- `test_xarray` proves the shared status-returning push kernel accepts the exact
  tagged carrier and leaves receiver shape and caller ownership unchanged on
  invalid receiver, slice, and typed-storage failures.
- `test_xtp_format` proves fixed compact KAT bytes and digest, primitive and
  each registered super-pair encoding, byte-exact canonical re-encoding,
  primitive/super row equivalence, directory field mutations, every stream
  byte mutation, and fail-closed noncanonical/truncated/overflow/unknown/count/
  trailing forms while exercising the public XSM/XTP generation route. It
  freezes 216-byte module-partition rows with a 256-row limit and 340-byte
  program-graph rows with a one-row limit. It also round-trips the exact
  lifecycle root/cleanup rows and rejects mutation
  of every identity-bearing lifecycle field.
- `test_xtp_resource_stress` proves the size/performance ladder and exact wire,
  expanded-row, and decode-work accounting.
- `test_xtp_fuzz_evidence` binds the standalone decoder/verifier
  mutation matrix, the resource ladder, and the freshly built runtime identity
  into one fail-closed executable result. Missing runtime or test executable,
  zero executed mutations, a non-clean or mismatched commit, and an unlabelled
  sanitizer binary are failures. Windows ThreadSanitizer is a red unsupported
  lane, never a skip.
- `test_typed_frame_runtime_archive` proves the dispatcher and verifier link
  into the runtime-only archive; the runtime artifact archive gate separately
  proves the exact XSM/XTP sole-function product route.
- `test_xi_cgen` proves that direct Array HOF emission consumes an immutable
  CEmission recipe plus one exact native callback ABI and direct-symbol closure
  plan. MAP/FILTER/REDUCE, used/unused and empty/nonempty paths emit portable
  C11; captured callbacks, extra uses, missing/duplicate plans, altered ABI
  slots, and absent recipes fail closed without dynamic helper fallback.
- `test_runtime_generation` proves exact sole-function PREPARE, ACTIVE scalar
  execution, unsupported-plan rejection, bounded pins, drain, retirement, and
  unload without any legacy execution fallback. It also proves that a program
  which divides by zero reaches ACTIVE and is stopped at execution as a
  program fault, separate from the authority failures an unsupported plan
  gives.
- `test_vm_decoded_cache` proves exact plan-pointer/schema/fingerprint binding,
  generated contract and block metadata, hard row/block/byte budgets,
  fail-closed construction from a corrupted plan, concurrent read-only
  execution, and cached/uncached parity across both providers for results,
  argument errors, divide-by-zero, and the exact step-limit edge. Its runtime-archive companion
  proves the cache surface links without compiler builders.
- `test_dynamic_entry_runtime` proves that dynamic call success, program error,
  trace rejection, and step exhaustion retire exactly the pin acquired for the
  resolution. Its injected production-retire refusal proves that context schema
  3 consumes the stack resolution, keeps the opaque lease reachable in the
  bounded authority ledger, preserves the in-flight pin, refuses unload, and
  releases the holder on the deterministic drain retry. The runtime-only archive
  test links the schema and ledger query without compiler builders.
  It also publishes an exact native-i64 executor through the same registry,
  drives it from `CALL_ENTRY_I64` through switch and function-table providers
  in cold, warm, uncached, traced, success, error, and cancellation cases, and
  proves native-to-VM invocation consumes the same frozen entry ABI and
  identity-adapter facts. Native/typed hot replacement invalidates only the
  affected site, while ABI, layout, adapter, ownership, and suspend mutations
  are rejected before cache, callback, or generation-pin side effects.
- `test_xa_program_semantic_closure` proves the bounded source graph builds one
  verified schema-58 TargetPlan, uses global rows with exact module partitions
  and a `PROGRAM_DIRECT`/`CALL_DIRECT_I64` edge, rejects independently mutated
  or re-signed inner authority, executes its graph-owned entry through both VM
  providers in cold and exact-generation-cache modes, rejects a wrong
  generation, a foreign same-fingerprint plan, a last-byte GCI mutation, and
  mutated graph or partition rows, and rejects the producer as a second root.
  It round-trips and executes graph XTP byte-identically and derives one
  independently verified AOT lower binding from the global function, symbol,
  call, argument, and instruction rows, rejects altered binding facts, and keeps
  the ordinary single-module runtime loader fail closed; the separate bounded
  public program facade consumes only the exact verified graph entry.
- `aot_program_graph_native_execution` runs the source-backed product-graph
  scenario from `run_module_summary_determinism.py`. Cold, warm,
  dependency-edit, and dependency-revert products install the same verified
  program C-emission authority, preserve cache identity/invalidation, compile
  real generated C, link and execute native results 42/42/43/42, and reject
  caller bodies containing legacy module/name/shared lookup. The paired
  frontend KAT mutates values, ABIs, graph rows, resolver/import joins, valid
  wrong shared slots, and the direct edge and requires fail-closed rejection
  with no per-module or legacy ABI fallback.


## Verification

verification-test: test_typed_dispatch
verification-test: test_vm_decoded_cache
verification-test: coroutine_lifecycle_projection_contract
