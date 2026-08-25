# Typed TargetPlan execution contract

TargetPlan schema 46 may carry a canonical per-function instruction table and
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
canonical rows. XTP schema 42 preserves the bounded sequential compact stream
introduced by v34; its directory entry carries the expanded row count, compact byte
length, `COMPACT` flag, and zero row size. Canonical ULEB128 and signed ZigZag
payloads plus the format-only superinstruction registry are the sole wire
authority. Materialization expands every token back to the same primitive
TargetPlan rows before verification; the decoded cache and dispatcher consume
only those rows and never observe a wire token. Text dump and diff use the same
sequential iterator rather than random row-size access. There is no v33
reader, translated row, alternate stream, or compatibility path.
The internal typed dispatcher has two disjoint request carriers. The scalar
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
requires the same plan pointer, schema, recorded fingerprint, and caller-required
fingerprint before consuming any row; it never treats a miss or mismatch as
authority to build or execute. Neither path inspects SemanticPlan or Xi. The verified rows are the only signature it honours: the
argument count must equal the number of parameter rows, and a shorter,
longer, or absent vector is rejected before the frame exists rather than
truncated, padded, or zero filled.

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
shared concurrently. Cached and uncached dispatch use the same generated
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

TargetPlan schema 46 is a hard cutover from v45 and every earlier TargetPlan
schema. It requires SemanticPlan schema 39 and its compact 225-row Xi operation registry.
The generated builtin receiver registry and stable method-symbol registry are
the sole authority for exact `Map<K,V>.entriesIterator()` calls and their
bounded `Iterator<(K,V)>.hasNext()`/`next()` continuations; selector spelling
is diagnostic only, and an `Iterator<unknown>` result fails closed. Schema 34
changed the instruction opcode carrier to an unsigned
16-bit stable ID while the
canonical instruction row remains exactly 32 bytes by shrinking its reserved
tail to one byte. The generated target instruction registry is the only opcode
authority consumed by the builder, verifier, artifact renderer, and dispatcher.
XTP schema 42 preserves the compact instruction stream introduced by v34,
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
- `test_xarray` proves the shared status-returning push kernel accepts the exact
  tagged carrier and leaves receiver shape and caller ownership unchanged on
  invalid receiver, slice, and typed-storage failures.
- `test_xtp_format` proves fixed compact KAT bytes and digest, primitive and
  each registered super-pair encoding, byte-exact canonical re-encoding,
  primitive/super row equivalence, directory field mutations, every stream
  byte mutation, and fail-closed noncanonical/truncated/overflow/unknown/count/
  trailing forms while exercising the public XSM/XTP generation route. It
  also round-trips the exact lifecycle root/cleanup rows and rejects mutation
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

anchor-sha256: src/plan/target/xr_target_plan.h 824f17196a96d034c3bb1a4cce36d9c11333d178ac8aa6bea95bbb7d29492cb0
anchor-sha256: src/plan/target/xr_target_plan.c 9e7140184ca04bab125144d1f4559ac87a42172eefcf22eb434f33ac8101edff
anchor-sha256: src/plan/target/xr_target_builder.c 94aad1858c8813c8183cdc5a69e0a896deb804e1870161c242cecb894be64da6
anchor-sha256: src/plan/target/xr_target_instruction_verify.h 1900ed05c513bd35071a58f2d31768ef74be09248b32e2c9e23d39fcc3db1c1a
anchor-sha256: src/plan/target/xr_target_instruction_verify.c 93d4b68bee1aade849af677e8ee0d5fd2160bc068646a1db184052435acd9a26
anchor-sha256: src/plan/target/xr_target_verify.c 90b44fa2b4f8839550e722a8679c6dddf50eeb5801c41a4f85b5a8c595d7d385
anchor-sha256: src/plan/format/xr_xtp_schema.h a53c1f1c5481784af7636d49e554e92d11055ad701e6727ad5961a4dfdf2100b
anchor-sha256: src/plan/format/xr_xtp_decode.c 9ccebe5d3887a58cdb8746861edeee2e6cc2128b028dccfc2d1387c0127bb014
anchor-sha256: src/plan/format/xr_xtp_row_fields.h 84e5b18d06b0a44e25708b80e0f19ff70918d0babd988d0d9ea7260fcb842f29
anchor-sha256: src/plan/format/xr_xtp_rows.c 7e2c7c25d880a3f0d38abf7a48e63f9918c78eaf68e2734b9d0d68bb575abbcd
anchor-sha256: src/plan/format/xr_xtp_encode.c c5131d9c1ec60d2d19729d21d95dfc013194d92fe88a49b0d9ad6fed63ab1a9b
anchor-sha256: src/plan/format/xr_xtp_instruction_stream.h 39a81bcf5b337b7fdbf4aafaa4eb8a6ba575d4a1853f6f86c8dca6d0d2e0579a
anchor-sha256: src/plan/format/xr_xtp_instruction_stream.c d2c219ad22c0f22193abe31436ba5466f7d60b3c4511628a7fa14a0fb2a98773
anchor-sha256: src/plan/format/xr_xtp_text.h 63367e2a75cc5e1511d1980cd82f579863cfd86a97cbf47d892f4c945d4ca0e1
anchor-sha256: src/plan/format/xr_xtp_text.c 794c85faec54254597eb2cc989b3d0a761e794988105d6bdc18aa19d82ac4162
anchor-sha256: xisa/target/xtp_super_ops.def 20968dd05c20d4caa85172fb2fc8cc051b74a1c6dcf93534368ce3ca7e491f88
anchor-sha256: src/plan/target/xr_xtp_materialize.c 638e5cb5fb73d8979a0c8f35f240800ac00d588ac4bad26889d0284391914eb4
anchor-sha256: src/vm/xr_typed_dispatch.h 96c324021b0f4a8cd5375ab1a155eb7a69db84f6246f6872994a9ec0c4397f75
anchor-sha256: src/vm/xr_typed_dispatch.c d7b818d5dc454879bf2e31098e9844ebde8c71a0402ae1606d34aa5cd0658f94
anchor-sha256: src/vm/xr_vm_decoded_cache.h b8dd666865e181f77203aff6b65217f3d1b5d3b413419c831d896a2e31902e23
anchor-sha256: src/vm/xr_vm_decoded_cache.c 216b764f20711e5612c653d11b25651aedd9afae20ec066e588cc69e60f05c21
anchor-sha256: src/vm/xr_typed_frame.c f7cbfdc3dc805bcfdf9c8a75e2a8293c9770f9873ba036402959980959ae13c9
anchor-sha256: tests/unit/vm/test_typed_dispatch.c edd1f5f7caacf8bc5b7babc31a21d10934d2cb96d3523eb38d8cc20102366ac6
anchor-sha256: tests/unit/vm/test_vm_decoded_cache.c 1f7e032e521c9cdf3cbe8e3b435a6e4c2e9113a8f838e5ac935209589213f183
anchor-sha256: tests/unit/plan/test_xtp_format.c de758e4a6073060c81e26a37a05d8e34597068d6408b12b756a5ac5acbe6003a
anchor-sha256: tests/unit/plan/test_xtp_resource_stress.c cfe41d4e83103cadb5e8eabc7a48aef121b5dc5ebdee14939e5c0f80bf955fff
anchor-sha256: tests/fuzz/fuzz_xtp_decode.c 8ef332c992bb8e44a2dbe06bd5463458ff84df41d9088d0596ace17e5e806d94
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c 3f49976a53aa6422da074107bedd4e0afd428fc76018bc4c44f144a8bc33a61e
anchor-sha256: scripts/check_coroutine_lifecycle_projection.py 74fdc88cea8045a258dae39f2194839ec54f3a2b8759fa56f1b537226fdbc1a2
anchor-sha256: tests/unit/ir/test_xi_cgen.c 1f23fba75359f65bfc14fe7a6fa2734a85fd5866d9d3e550a45eae46dbcbe964
anchor-sha256: tests/unit/ir/test_xi_opt.c 96b1ceb9789cd6b7742bcf29757087e2a27c144cf1107eaa70c0547295086beb
anchor-sha256: tests/unit/runtime/test_vm_decoded_cache_runtime_archive.c 33da22f5eec9a7889b25380fa99e070c807c19580569ac081a0f0558545eb8e3
anchor-sha256: include/xray_runtime_generation.h ac482c4a6b3526a5060554b2f27ef13a1258c9807529f5e0cc914710bf890d91
anchor-sha256: src/runtime/xr_module_generation.c eb3581c48d3a59230a9b4db38a87ada54785aeb366e8fb7151f4246f3da38974
anchor-sha256: tests/unit/runtime/test_runtime_generation.c 314197abf0562d4ec4954bae52c3504fc9215d6d6aeb9b69b93eb3124c719895
anchor-sha256: CMakeLists.txt 3d6094ae7aa2beba9a3c2cd75281d2b014ceab36a6b72572acf95525ace372eb
anchor-sha256: xisa/target/vm_ops.def 36dc261bb48b5908d445998a432711b359552f9a6ada71e199fb100f7b7e9304
anchor-sha256: tools/xisagen/xisagen.py 9bc7501baa050d17830576325831de9331d0f8644566254b2b953047d37f7821
anchor-sha256: src/plan/target/xr_target_entry_abi.h 80cd119cbc095ddfddbf95ff5085fbaa23659256feb8d18a36e43416013747ea
anchor-sha256: src/plan/target/xr_target_entry_abi.c cb5cd57a0b8f3bbfe2123a07f583da997d7d2989e5158fd241406b96ce433b12
anchor-sha256: src/plan/target/xr_target_instruction_gen.h 04c9ca0ca30fff53152c64744e4d884b3eec3c0156c299a8d52be39abd50bc08
anchor-sha256: src/vm/xr_vm_dynamic_entry.h 50a175071a41a521e11fa672b7f17663b73dda321fd6ab9703196324e642dfda
anchor-sha256: src/vm/xr_vm_entry_adapter.h 260bca5ab4abcef7cc679f5674e92c0b2c8e95fa04444b73cb1e3f8584b61544
anchor-sha256: src/vm/xr_vm_entry_adapter.c a18c76b33fa1a35b0b2b756d6eab77de5b7876f58b65bf6c5605a7596e701547
anchor-sha256: xisa/target/vm_entry_adapters.def db46c172fa847c54cb24d477404f00d74db9996b99be9fa357a3ce0864a9ddb9
anchor-sha256: src/vm/xr_vm_ops.def 29f593ee6c27b514b3c31499095f973e2eda82ce7c6f7cc3485da210fec4efc5
anchor-sha256: src/shared/xr_array_push_status.h 2d092c8b4b91fd7f8c09bb90f72bd0408d4eabedf2d235b5643bd0b1c81dbe04
anchor-sha256: src/runtime/object/xarray.h 65c5272371925c830c046d08247fee289464b4fed2453b9546815367a41d6ff5
anchor-sha256: src/runtime/object/xarray.c 102a52b887e0e777a79da3c78be8e025e1b2826f0a01cd4b0aee5cbe431c6416
anchor-sha256: src/vm/xvm_dispatch_collection.inc.c 522f67fa27199b54d2c20942ff344e0b1a657635e28a4e45084737d603a635eb
anchor-sha256: src/vm/xvm_dispatch_struct.inc.c 903196c49a385fdec0d96f168c62fa86fabdfa079523ca4fd504bd5badb491ee
anchor-sha256: tests/unit/object/test_xarray.c cfc4a90f4ee19215b036b488f054c9c0590acbb68f4a053d349cfc53c39e25cf
anchor-sha256: tests/unit/plan/test_target_plan.c 7c2f2591eb000ff7ba25accb8a0ea6d4390173a280ef5c0e45f429309153b4d8
anchor-sha256: src/runtime/xr_dynamic_entry_runtime.h 84d4d2c4feacd955ec13ed949379c8a23ca1a966c37221a5e8ec04126c1c55dc
anchor-sha256: src/runtime/xr_dynamic_entry_runtime.c 8915b084f4a1de025340c94a2132d6d257c8b2ff3273faf3fd200bd2d5f6580f
anchor-sha256: tests/unit/runtime/test_dynamic_entry_runtime.c ad03d77d583441969756a1f2d1be2cde504624c0cd4ed26ddae96a50c8a79ed5
anchor-sha256: tests/unit/CMakeLists.txt 8563b7ae28a6b53c98e2ade41df8ebaa11e412d6004310c44cf3e14aacc20e79
