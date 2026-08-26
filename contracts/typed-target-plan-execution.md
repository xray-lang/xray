# Typed TargetPlan execution contract

TargetPlan schema 48 may carry a canonical per-function instruction table and
an exact per-call-site dynamic-entry expectation table.
Instruction authority is separate from the production AOT family mask: a
verified plan can remain a complete AOT plan while exposing no typed execution
family. A function with zero instruction rows is execution unavailable, never
an empty successful program. The production builder emits a complete group
only for either a verified, capture-free signed-`i64` function whose declared
parameters are all exact signed `i64`, whose blocks are all returns, plain
jumps, or two-way branches, and whose operations are entirely in the supported
family, or the exact source-class tagged `Array.push` call family described
below. Every other function emits zero rows; no partial group or fallback is
allowed.

The bounded leaf-value aggregate direct-call family is one exact executable
family within schema 48, not a second aggregate-only plan. SemanticPlan 42
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

The bounded two-module scalar graph is one additional executable schema-48
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
authority. Schema 5 resolves the caller and callee global function rows through
their owning semantic partitions, records their stable program-function symbol
identities, and binds the unique global `CALL_DIRECT_I64` instruction to the
`PROGRAM_DIRECT` call. Freeze and independent verification re-derive those
facts from the same TargetPlan. A committed program-direct row cannot become a
refusal, legacy resolution, or per-module executable plan: missing or
contradictory evidence fails the consumer. This is lower translation authority,
not native execution authority. The independent transient C-emission binding
then consumes that same verified program TargetPlan: it matches each partition
to exactly one Xi module through its PSC row and attached SemanticPlan, joins
global functions and the direct call to unique Xi nodes through PSC program-row
indexes, and derives deterministic C symbols from stable function identities.
Missing or duplicate modules, functions, or calls fail closed. This binding is
not a second executable plan and does not authorize generated C until product
CGen and bundle orchestration consume it.

The graph XTP route preserves the same executable VM authority and AOT lower
authority. Schema 48 appends exact graph and partition sections, mutually
excludes ordinary and graph directory shapes, binds the header semantic
fingerprint to the full canonical module set, and materializes only through that
set plus the exact target profile.
`MODULE_PARTITIONS` is a fixed 208-byte row format with at most 256 rows;
`PROGRAM_GRAPHS` is a fixed 340-byte row format with at most one row. Round-trip
is byte-identical, while wrong count/order, duplicate/missing modules, re-signed
graph or partition mutations, and ordinary/graph substitution fail closed. The
ordinary runtime loader accepts no graph plan. The source AOT product builds and
verifies the graph plan, builds and independently verifies its global AOT
direct-call binding, publishes deterministic module summaries, then stops with
`XR_TARGET_1001` before legacy per-module TargetPlan preparation, C emission,
or binary publication. Native AOT and product publication remain unavailable
until the bundle and emitter consume this same verified program plan and its
global rows end to end; the VM does not bypass that product fence.

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

The managed executable family is deliberately one exact operation: a
source-class `Array<T>.push(T)` Target call whose receiver is `BORROW`, whose
element is `CONSUME`, whose storage is `TAGGED`/`DYN_VALUE`, and whose result is
the Unit-valued void side effect. Its complete instruction group is two
required parameter rows, `ARRAY_PUSH_TAGGED`, and `RETURN_UNIT`. Per-operand
ownership, the memory-write/may-error effects, the exact Target call row and
argument rows, source-class element type, selector identity, slot identities,
and dense row shape are independently rederived by the Target verifier. The
builder and verifier consume SemanticPlan/TargetPlan authority directly; the
VM never reads an AOT CEmission recipe and never derives an executable answer
from a selector spelling.

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
canonical rows. XTP schema 48 preserves the bounded sequential compact stream
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

TargetPlan schema 48 is a hard cutover from v47 and every earlier TargetPlan
schema. It requires SemanticPlan schema 42 and its exact generated Xi operation registry.
The generated builtin receiver registry and stable method-symbol registry are
the sole authority for exact `Map<K,V>.entriesIterator()` calls and their
bounded `Iterator<(K,V)>.hasNext()`/`next()` continuations; selector spelling
is diagnostic only, and an `Iterator<unknown>` result fails closed. Schema 34
changed the instruction opcode carrier to an unsigned
16-bit stable ID while the
canonical instruction row remains exactly 32 bytes by shrinking its reserved
tail to one byte. The generated target instruction registry is the only opcode
authority consumed by the builder, verifier, artifact renderer, and dispatcher.
XTP schema 48 preserves the compact instruction stream introduced by v34,
appends the exact 144-byte entry-expectation section after all prior tables,
widens each coroutine state with its function-local resume-instruction
authority, and preserves the exact lifecycle root-map, root-slot, and cleanup
rows. The generated registry appends `SUSPEND` as opcode 28 and the exact
managed push rows `PARAM_DYN_BORROW`, `PARAM_DYN_OWNED`,
`ARRAY_PUSH_TAGGED`, and `RETURN_UNIT` as opcodes 29 through 32 without
renumbering any prior opcode. A suspend row names both the exact coroutine
state and that state's independently frozen resume instruction. Unknown,
duplicated, redirected, mismatched, or absent rows fail closed. No v39 or
earlier row or artifact is widened, translated, or accepted by a compatibility
reader.

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
Rune producer. The exact native-bool `rune.isWhitespace` recipe remains under
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
The C emission projection schema 30 mechanically spells all verified dynamic
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
It also projects an exact raw-pointer TargetPlan binding without consulting a
live Xi type. When and only when the same semantic `LOCAL_ADDR` value is bound
by a SOURCE_EXPORT `ref` argument row, the projection adds one C pointer level;
the C consumer cannot infer that level from an import name, callee type, or
legacy representation plan.
For the sealed `StringBuilder()` call it owns the zero-operand
`xrt_strbuf_new` materialization recipe. Sync and coroutine CGen have no
name-based fallback for that constructor. For the exact String byte-slice view,
it owns the `xr_span_t` representation, source semantic value, and fixed
`xrt_span_from_string_bytes` recipe. CGen cannot recover these from selector
text, aliases, mutable Xi types, or legacy representation state.
For an exact payload-bearing ADT-enum constructor it owns the immutable layout
ID, discriminant, type/member spellings, namespace receiver identity, and
ordered payload semantic identities. CGen emits the aggregate-box recipe only
from that verified projection and cannot reconstruct type, selector, or arity
from Xi.

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
- `test_target_plan` proves the production source-class tagged `Array.push`
  group, exact DYN representation and per-operand ownership, both generated
  providers, the mandatory runtime-kernel capability, and success ownership
  transfer. A missing kernel is refused before the frame can acquire the
  element; the runtime-only VM archive therefore contains no hidden object
  implementation or unresolved hosted-runtime symbol. The test mutates
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
  freezes 208-byte module-partition rows with a 256-row limit and 340-byte
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
  verified schema-48 TargetPlan, uses global rows with exact module partitions
  and a `PROGRAM_DIRECT`/`CALL_DIRECT_I64` edge, rejects independently mutated
  or re-signed inner authority, executes its graph-owned entry through both VM
  providers in cold and exact-generation-cache modes, rejects a wrong
  generation, a foreign same-fingerprint plan, a last-byte GCI mutation, and
  mutated graph or partition rows, and rejects the producer as a second root.
  It round-trips and executes graph XTP byte-identically and derives one
  independently verified AOT lower binding from the global function, symbol,
  call, argument, and instruction rows, rejects altered binding facts, and keeps
  the ordinary runtime loader fail closed.
- `run_module_summary_determinism.py` proves cold, warm, dependency-edit, and
  dependency-revert products reach a verified program TargetPlan before cache
  publication, then stop at the explicit same-plan VM/AOT execution fence and
  publish no binary.

anchor-sha256: src/plan/target/xr_target_plan.h e78f4e0216169aa2d1acbf9db0df80b338e6ecdb20c3a0b0dd7ef3dae8bf774b
anchor-sha256: src/plan/target/xr_target_plan.c 3248f07e1b02eca3eb224a6009a03ff3a8ced6d2074932ac8338268ecc0ed885
anchor-sha256: src/plan/target/xr_target_plan_internal.h 98b93650f62e5cfae811bbd8e357eccc5adbc316631a35de580358bfc11a038b
anchor-sha256: src/plan/target/xr_target_builder.h 4d3d604216a064eb6be4a787afda9ee168f057dc640b5c6065f63f2829e6d05e
anchor-sha256: src/plan/target/xr_target_builder.c 7d5f82fef73cb858412d4593c40847faaadb285b424614bb39825fcdbeb1e17c
anchor-sha256: src/plan/target/xr_target_instruction_verify.h 1900ed05c513bd35071a58f2d31768ef74be09248b32e2c9e23d39fcc3db1c1a
anchor-sha256: src/plan/target/xr_target_instruction_verify.c 1f3e4dbb008d837120fca30d1e476dbc78569bd4a427b2e5c613ee6bb482245a
anchor-sha256: src/plan/target/xr_target_verify.c 931eec0f2ed22268e9371b2a8065f5f0b79d266260277394a4e501aef0825ba0
anchor-sha256: src/plan/format/xr_xtp_schema.h 0bee55fe5f79f9fffb91a5c7c03032c91ce59ddb7eb3ccdf197fb5ade619f282
anchor-sha256: src/plan/format/xr_xtp_internal.h 35ac710feb01cabdd9de87b17a481aa73847984f8c4e26354d6902344879058f
anchor-sha256: src/plan/format/xr_xtp_decode.c 9ccebe5d3887a58cdb8746861edeee2e6cc2128b028dccfc2d1387c0127bb014
anchor-sha256: src/plan/format/xr_xtp_row_fields.h 4464709ecaf6f74067c2cc4ce58b3449c684960d88ed744ed803fb8c53a65474
anchor-sha256: src/plan/format/xr_xtp_rows.c 0c3408c70bb44b1049849be5a43ca4989187eebfd10781ea5f371625a35f0789
anchor-sha256: src/plan/format/xr_xtp_encode.c 4db5c593d44645c7d142f938973ccb199ba2a9d0c21dbfc539f17e05c4bac74e
anchor-sha256: src/plan/format/xr_xtp_instruction_stream.h 39a81bcf5b337b7fdbf4aafaa4eb8a6ba575d4a1853f6f86c8dca6d0d2e0579a
anchor-sha256: src/plan/format/xr_xtp_instruction_stream.c d2c219ad22c0f22193abe31436ba5466f7d60b3c4511628a7fa14a0fb2a98773
anchor-sha256: src/plan/format/xr_xtp_text.h 63367e2a75cc5e1511d1980cd82f579863cfd86a97cbf47d892f4c945d4ca0e1
anchor-sha256: src/plan/format/xr_xtp_text.c 794c85faec54254597eb2cc989b3d0a761e794988105d6bdc18aa19d82ac4162
anchor-sha256: xisa/target/xtp_super_ops.def 20968dd05c20d4caa85172fb2fc8cc051b74a1c6dcf93534368ce3ca7e491f88
anchor-sha256: src/plan/target/xr_xtp_materialize.c 6079934e95208abe3b7b7251b4c4b59275c61776f20de0d0873b70617899a62a
anchor-sha256: src/vm/xr_typed_dispatch.h acd1095b3a2d9e5607d007992b68b714d2e22fe394bfa8543611ea061ab40f43
anchor-sha256: src/vm/xr_typed_dispatch.c 1fa2d59ce6df94d4d983c2b0fb7e3af48ad30a2dcceb7526bd147f57f52c714f
anchor-sha256: src/vm/xr_vm_decoded_cache.h 55ac6ffaab71ac0e77a3db5e10ad326057d0052f4ae3b9722029c8ea06c49cf0
anchor-sha256: src/vm/xr_vm_decoded_cache.c f1f420b39d78f39e372b3378425809fb6c7049bad84aa02f84df5e542cfd83de
anchor-sha256: src/vm/xr_typed_frame.c 749f45bf957f82be3142e9aa9565b7bf9020b0f29ff494709bb4c5a900edea53
anchor-sha256: tests/unit/vm/test_typed_dispatch.c 3556b4e30fe1464e82b1fd8c4a23a3ba04530a1ca918ef055454cf84a531884e
anchor-sha256: tests/unit/vm/test_vm_decoded_cache.c 576c9e443c711070aff3ab58efbaac42266f89be6be64d49f84889dc9668723c
anchor-sha256: tests/unit/plan/test_xtp_format.c e994e527df3931be8ad94a395345f8f11d34805cd4cfe603193d1c4bfa2c5c8b
anchor-sha256: tests/unit/plan/test_xtp_resource_stress.c 48957cbd5b000fb267af4e5ac456223161afccc8c0e9a5b12102a75a236d7124
anchor-sha256: tests/aot/run_module_summary_determinism.py ab111483920e1059375226da9d9cf84fb96e2474c0608418083aff403cf85d87
anchor-sha256: tests/unit/frontend/test_xa_program_semantic_closure.c 39966f66260186d74864a1e41fd8afbd4283cffd34763ee4af448d81c0b32f4e
anchor-sha256: tests/fuzz/fuzz_xtp_decode.c 8ef332c992bb8e44a2dbe06bd5463458ff84df41d9088d0596ace17e5e806d94
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c 3f49976a53aa6422da074107bedd4e0afd428fc76018bc4c44f144a8bc33a61e
anchor-sha256: scripts/check_coroutine_lifecycle_projection.py 74fdc88cea8045a258dae39f2194839ec54f3a2b8759fa56f1b537226fdbc1a2
anchor-sha256: tests/unit/ir/test_xi_cgen.c c5ba83e1522437c77a2a35fa9ae13981d2ddf5e7741225466e5037453b5f02c4
anchor-sha256: tests/unit/ir/test_xi_opt.c 96b1ceb9789cd6b7742bcf29757087e2a27c144cf1107eaa70c0547295086beb
anchor-sha256: tests/unit/runtime/test_vm_decoded_cache_runtime_archive.c 8e8a3b987ae81542254495a889b838d7a325a9a2e06d5c80092bc9db92373aa5
anchor-sha256: include/xray_runtime_generation.h e2540f1ff42e095c1a7e5a27387a74fbb26d778ead89846acc502b4b542da631
anchor-sha256: src/runtime/xr_module_generation.c 5f8d48759b9d366e2686137489b0384fb2fa0495ec87254daba22f77404fff5f
anchor-sha256: tests/unit/runtime/test_runtime_generation.c f4422072f94b01c4411b4677cb62ba72304553753c9225074d981b6b20f43fa3
anchor-sha256: CMakeLists.txt a6aa0e036427fc6ff50bd718db5f627fcebe87f6ac2742b5aabd0905ab5f2458
anchor-sha256: xisa/target/vm_ops.def 573b1beea387c7b5df58de4fdad39e61417ea51ff6d346417bda7f36e698bc15
anchor-sha256: tools/xisagen/xisagen.py ca1bfeb87944eff4eebf4d478b514a8bd6ea9fc0adef95d1589d97d9116c923e
anchor-sha256: src/plan/target/xr_target_entry_abi.h 80cd119cbc095ddfddbf95ff5085fbaa23659256feb8d18a36e43416013747ea
anchor-sha256: src/plan/target/xr_target_entry_abi.c cb5cd57a0b8f3bbfe2123a07f583da997d7d2989e5158fd241406b96ce433b12
anchor-sha256: src/plan/target/xr_target_instruction_gen.h f26247e8b421e8ca8cfc44c49fcafdf92b4ae6cfa863fa65e7eea73d1d801aa5
anchor-sha256: src/vm/xr_vm_dynamic_entry.h 50a175071a41a521e11fa672b7f17663b73dda321fd6ab9703196324e642dfda
anchor-sha256: src/vm/xr_vm_entry_adapter.h 260bca5ab4abcef7cc679f5674e92c0b2c8e95fa04444b73cb1e3f8584b61544
anchor-sha256: src/vm/xr_vm_entry_adapter.c a18c76b33fa1a35b0b2b756d6eab77de5b7876f58b65bf6c5605a7596e701547
anchor-sha256: xisa/target/vm_entry_adapters.def db46c172fa847c54cb24d477404f00d74db9996b99be9fa357a3ce0864a9ddb9
anchor-sha256: src/vm/xr_vm_ops.def b0d669c575245640be14cbf4fae7d5d494d566d8942fd943fb133f60a65f87f8
anchor-sha256: src/shared/xr_array_push_status.h 2d092c8b4b91fd7f8c09bb90f72bd0408d4eabedf2d235b5643bd0b1c81dbe04
anchor-sha256: src/runtime/object/xarray.h 65c5272371925c830c046d08247fee289464b4fed2453b9546815367a41d6ff5
anchor-sha256: src/runtime/object/xarray.c 102a52b887e0e777a79da3c78be8e025e1b2826f0a01cd4b0aee5cbe431c6416
anchor-sha256: src/vm/xvm_dispatch_collection.inc.c 522f67fa27199b54d2c20942ff344e0b1a657635e28a4e45084737d603a635eb
anchor-sha256: src/vm/xvm_dispatch_struct.inc.c 903196c49a385fdec0d96f168c62fa86fabdfa079523ca4fd504bd5badb491ee
anchor-sha256: tests/unit/object/test_xarray.c cfc4a90f4ee19215b036b488f054c9c0590acbb68f4a053d349cfc53c39e25cf
anchor-sha256: tests/unit/plan/test_target_plan.c f9328d8928498f48a88091b0a7b412f5119d4c126a44e3dc3ff896fbfab3fcef
anchor-sha256: src/runtime/xr_dynamic_entry_runtime.h 84d4d2c4feacd955ec13ed949379c8a23ca1a966c37221a5e8ec04126c1c55dc
anchor-sha256: src/runtime/xr_dynamic_entry_runtime.c 48ec9d693c6bc32c8d08933006363d1a530518c29950886fa2537c3f0a65b456
anchor-sha256: tests/unit/runtime/test_dynamic_entry_runtime.c ad03d77d583441969756a1f2d1be2cde504624c0cd4ed26ddae96a50c8a79ed5
anchor-sha256: tests/unit/CMakeLists.txt 405e5d564669aeb8e1ad1ac31e14613a8530fd89b63247940a6d38748bdd1ba8
anchor-sha256: src/aot/xaot_boundary.h 465de1d73d5ec9cb3819fc9506405a5116567a54a048eeef38fca697f5cf8ca7
anchor-sha256: src/aot/xaot_boundary.c 15778b2d1dc0ae7bf54386a8a09e9b8ffe60eb6bc02bf26c5de7a600cd0f81b9
anchor-sha256: src/aot/xaot_bundle.c 80415f8dab7b601ded70e012c4cd1432974a6647ee9b4f79ba77768ad68a6bb3
anchor-sha256: src/aot/xaot_callable.c 96f90380791063480f5bf26ffb7039946c16f759eb00fd65b40b648f0fc7c661
anchor-sha256: src/aot/xaot_driver.c 5971d1257d23c791ac25fb7ab1dfbe5da9c3f8b60c831bc00b8a81c86f865406
anchor-sha256: src/aot/xaot_prepare.c f85fcea7d10fa6bc05e51b3b56731fe5b49a7ca6dbfbfd42d4be4ede7c7385e2
anchor-sha256: src/aot/xaot_verify.c fa8bbbeb5af03dded414f768b7ff71f5cbc4d0585cf3be1fb51036caf89ed402
anchor-sha256: src/aot/xr_target_aggregate_c_projection.h af24dca6237c439faebee2def632939985efe161c59578b4d4323c7e60441311
anchor-sha256: src/aot/xr_target_aggregate_c_projection.c d1b876cf1600462e2fe42a9479e19da490300b672a1f14ce08c44468c3aca22b
anchor-sha256: src/aot/emit_c/xr_c_emission_plan.c e0afec5542188381add3de02f74fab76d983a70da33d69b17e206d847e49eff5
anchor-sha256: src/aot/emit_c/xr_c_program_emission.h a7d5194433358ddfccb1d69eeda9778308ce78b63d57ae402299b0efb0f393a2
anchor-sha256: src/aot/emit_c/xr_c_program_emission.c 037a09b24bc4ba417d0e59966da396916fe5f2071e838cbb6782953d2c851591
anchor-sha256: src/aot/xi_cgen.c 252cb973f45aab4b41c5dc76ec039b924487b7162b1fadbabd0242ea38fe79ba
anchor-sha256: src/aot/xi_cgen_abi_helpers.inc.c c58bde5be23a2b2509a369632eb2e830015e644495079f7fa5d7e283d2cfaab9
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c 283d4f75e19ace0f068d040c12a4633726038dd6c4eafffc35df0bba9acdca15
anchor-sha256: src/aot/xi_cgen_program_entry.inc.c ebc90392a842b12a97794a7bf3e0ea862e8340c58dde8ff215d65f6e75c7ffd3
anchor-sha256: src/aot/xi_cgen_struct_helpers.inc.c affab668a66bcba68f5a0fade570bfe78824f7cc1f4ed2f1b13042cf4c727d2d
anchor-sha256: tests/unit/ir/test_xi_program_semantic.c 3ea53772f142cfcf2a40f0e7dfa52607183049f6d1412b175bc4b41f5d304101
anchor-sha256: tests/unit/aot/test_xaot_driver.c 825195ce7d4ad636b2138fa95a783ceb03c2df6e51d7c411ae707f3b201d6a41
