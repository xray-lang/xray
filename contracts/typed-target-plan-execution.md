# Typed TargetPlan scalar execution contract

TargetPlan schema 39 may carry a canonical per-function instruction table and
an exact per-call-site dynamic-entry expectation table.
Instruction authority is separate from the production AOT family mask: a
verified plan can remain a complete AOT plan while exposing no typed execution
family. A function with zero instruction rows is execution unavailable, never
an empty successful program. The production builder emits a complete group
only for a verified, capture-free signed-`i64` function whose declared
parameters are all exact signed `i64`, whose blocks are all returns, plain
jumps, or two-way branches, and whose operations are entirely in the supported
family. Every other function emits zero rows; no partial group or fallback is
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

The second executable family adds only non-suspending SOURCE_EXPORT calls whose
parameters and result are exact signed `i64`, whose ownership is trivial, and
whose adapter is identity. `CALL_ENTRY_I64` names a dense expectation row, not
a process pointer. That row binds the canonical callee ABI fingerprint, target
profile, identity-adapter fingerprint, call row, and parameter count. The ABI
fingerprint has its own entry domain and is never reused from the call-site
fingerprint. INDIRECT_CALLABLE, native/hosted adapters, owned or object values,
and suspending calls remain unavailable.

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
canonical rows. XTP schema 36 preserves the bounded sequential compact stream
introduced by v34; its directory entry carries the expanded row count, compact byte
length, `COMPACT` flag, and zero row size. Canonical ULEB128 and signed ZigZag
payloads plus the format-only superinstruction registry are the sole wire
authority. Materialization expands every token back to the same primitive
TargetPlan rows before verification; the decoded cache and dispatcher consume
only those rows and never observe a wire token. Text dump and diff use the same
sequential iterator rather than random row-size access. There is no v33
reader, translated row, alternate stream, or compatibility path.
The internal scalar dispatcher accepts one immutable request containing only a
verified plan, its exact fingerprint, a derived nonzero function execution
family, a positional signed-`i64` argument vector, exact typed-frame slot
identities, an optional runtime-only debug session, and an optional immutable
decoded cache for that exact plan object. A dynamic family request additionally
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
truncated, padded, or zero filled. It has no legacy VM opcode, `XrValue`, AOT,
or generated-C fallback.

The dispatcher carries an instruction pointer that starts at the group's first
row and moves only where a row says, repeating the verifier's target bound so a
table that changed underneath cannot move it out of the group. A verified
program may loop forever, since no static proof could forbid that without also
forbidding ordinary loops, so executed rows per call are capped by a fixed
budget rather than a wall clock. Exhausting the budget stops the call with its
own status, distinct from every status that reports an unacceptable plan or
call, so a plan can never hang the caller that ran it and the same call refused
once is refused every time.

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

TargetPlan schema 39 is a hard cutover from v38 and every earlier TargetPlan
schema. It requires SemanticPlan schema 34 and its compact 225-row Xi operation registry;
the retired non-lowerable bounds-guard opcode is neither reserved nor
translated. Schema 34 changed the instruction opcode carrier to an unsigned
16-bit stable ID while the
canonical instruction row remains exactly 32 bytes by shrinking its reserved
tail to one byte. The generated target instruction registry is the only opcode
authority consumed by the builder, verifier, artifact renderer, and dispatcher.
XTP schema 39 preserves the compact instruction stream introduced by v34,
appends the exact 144-byte entry-expectation section after all prior tables,
and preserves the exact lifecycle root-map, root-slot, and cleanup rows.
Unknown, reordered, mismatched, or absent rows fail closed. No v38 or earlier
row or artifact is widened, translated, or accepted by a compatibility reader.

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
Schema 17 added an exact borrowed `Slice<byte>` view of String storage.
The view binds the frozen Semantic intrinsic and source root to the exact
`xray-target-string-byte-slice-view-v1` call identity; it grants no generic
String method or slice construction authority. Schema 18 adds a sealed
`Iterator<rune>.hasNext` bool recipe whose receiver is the exact prior
`String.runes` result and whose fixed runtime helper preserves pending-error
polling. C emission schema 30 adds the separate native-rune `next` recipe only
when the receiver has that same unique frozen `String.runes` producer, plus
the exact native-u32 `rune.toUInt32` recipe only when its receiver is the
unique exact `.next()` result in the same function, and the exact native-bool
`rune.isWhitespace` recipe under the same receiver proof. None grants general
Iterator or Rune dispatch. It separately adds the exact owned String
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
closure initializer, shared-slot dominance, unique canonical local child and
signature, and that every use is operand zero of `XI_GO` for that same child.
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

anchor-sha256: src/plan/target/xr_target_plan.h 16f1be1bfc85a85eeb663254f08e5f3e0c2c56430ea8f702870cb8cac3303487
anchor-sha256: src/plan/target/xr_target_plan.c 174515da0c7b620914fda50cc19a74cec472a00d9e668be4e5d587a58ccf8bcf
anchor-sha256: src/plan/target/xr_target_builder.c 481ce816c3615c140497e2d868c59a9e8435f404b01e2fee649b5fcbad36a9dc
anchor-sha256: src/plan/target/xr_target_instruction_verify.h 1b2e9809094bf2596a637ce88dffb3d83954009d502409d7cb7a3422711edb14
anchor-sha256: src/plan/target/xr_target_instruction_verify.c 3b1a3de8b8cfff310c382b9dcac658d60c4b727b306c9c96665baf55c6904f74
anchor-sha256: src/plan/target/xr_target_verify.c 96539b22fa959f382a48549ae800b060480a59e59ea7adcab94a0a5335093ef2
anchor-sha256: src/plan/format/xr_xtp_schema.h e45c595103f4427dcdcf202b47220ada1b403ec8b4080bf8a78435ff2b9f6ea9
anchor-sha256: src/plan/format/xr_xtp_decode.c 9ccebe5d3887a58cdb8746861edeee2e6cc2128b028dccfc2d1387c0127bb014
anchor-sha256: src/plan/format/xr_xtp_row_fields.h 8cd7f26e5a92ab6a525a400278ff3f5af2b7608c1d36845413739939606030c4
anchor-sha256: src/plan/format/xr_xtp_rows.c fae35d76de38320ebb882a4ede41e63f5e7f7e4de506f2a945d918d58d18b45e
anchor-sha256: src/plan/format/xr_xtp_encode.c c5131d9c1ec60d2d19729d21d95dfc013194d92fe88a49b0d9ad6fed63ab1a9b
anchor-sha256: src/plan/format/xr_xtp_instruction_stream.h 39a81bcf5b337b7fdbf4aafaa4eb8a6ba575d4a1853f6f86c8dca6d0d2e0579a
anchor-sha256: src/plan/format/xr_xtp_instruction_stream.c d2c219ad22c0f22193abe31436ba5466f7d60b3c4511628a7fa14a0fb2a98773
anchor-sha256: src/plan/format/xr_xtp_text.h 63367e2a75cc5e1511d1980cd82f579863cfd86a97cbf47d892f4c945d4ca0e1
anchor-sha256: src/plan/format/xr_xtp_text.c 329d1430c8ad00d5b7c05c69adb3ed5405b5b9b6ea49cc7ea3155f561df00113
anchor-sha256: xisa/target/xtp_super_ops.def 20968dd05c20d4caa85172fb2fc8cc051b74a1c6dcf93534368ce3ca7e491f88
anchor-sha256: src/plan/target/xr_xtp_materialize.c 638e5cb5fb73d8979a0c8f35f240800ac00d588ac4bad26889d0284391914eb4
anchor-sha256: src/vm/xr_typed_dispatch.h 8c764cfb9f22e8c58ea75b0862b496996cd09bc6fc1e86d6fcc1ef5f6398cdb4
anchor-sha256: src/vm/xr_typed_dispatch.c c79296a7d62cd0ae42f41e36e67dead23ac3b24095504cbba7ed52fe2a60fe9c
anchor-sha256: src/vm/xr_vm_decoded_cache.h b8dd666865e181f77203aff6b65217f3d1b5d3b413419c831d896a2e31902e23
anchor-sha256: src/vm/xr_vm_decoded_cache.c 2d4f14d54740e6aac0cdfb23fc7b90b075725c479751ba52a1948a658f91fa77
anchor-sha256: src/vm/xr_typed_frame.c 80ac935291096963179c8f6c58b3105835426c87b2b693d2a62e1d5c16fc913b
anchor-sha256: tests/unit/vm/test_typed_dispatch.c 0c6753b95b6a07f1bfff18d3294f1cc76348f60252663feb11aef72ddcf52a1a
anchor-sha256: tests/unit/vm/test_vm_decoded_cache.c 1f7e032e521c9cdf3cbe8e3b435a6e4c2e9113a8f838e5ac935209589213f183
anchor-sha256: tests/unit/plan/test_xtp_format.c 5d2caf3f32b031b6970114123f5ad37fb31961100c6614c7c924456064cc0142
anchor-sha256: tests/unit/plan/test_xtp_resource_stress.c cfe41d4e83103cadb5e8eabc7a48aef121b5dc5ebdee14939e5c0f80bf955fff
anchor-sha256: tests/fuzz/fuzz_xtp_decode.c cd38c52f605f446123a65b2b0b5cb851fb730fe8354f6e37f1b3c240ee71ebdc
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c cfe7790025d9a3962ba5840594f3d6cbaf8f132e67f996292488cc25800dcf2a
anchor-sha256: scripts/check_coroutine_lifecycle_projection.py 74fdc88cea8045a258dae39f2194839ec54f3a2b8759fa56f1b537226fdbc1a2
anchor-sha256: tests/unit/ir/test_xi_cgen.c 5e72c6de3f3c5820822dd1ce15fd11bbd0c1886143ab61e525c45ab83efd3ffa
anchor-sha256: tests/unit/ir/test_xi_opt.c 81e222d43f1a76ea0cafc15af13ce23f10006c35f3d43b52c8be9d478e8ba7b4
anchor-sha256: tests/unit/runtime/test_vm_decoded_cache_runtime_archive.c 33da22f5eec9a7889b25380fa99e070c807c19580569ac081a0f0558545eb8e3
anchor-sha256: include/xray_runtime_generation.h b8d8ab25bf7945cb6837af74a2460ff52d516714b47c3331f6ce82fbc33c05d0
anchor-sha256: src/runtime/xr_module_generation.c a07ba16736dd26135e4256d01092ad7f1be71e29787ef68504cd3f61eb979305
anchor-sha256: tests/unit/runtime/test_runtime_generation.c 98a80d0e5d24ffafaca415fd5c07abde8f560a239e76b6b2d321b629e55fd355
anchor-sha256: CMakeLists.txt 3a1431c97e9111b7e3ef6d3c9fe8c01f3ee497bc7b1e83b3bc1a61d2a399e4e2
anchor-sha256: xisa/target/vm_ops.def 359a60da3aa91aa5fd8b0569228c29b9590fe5b1e11d77c936b3ef2b36dff6e9
anchor-sha256: tools/xisagen/xisagen.py 7de42418fd37c55d742b185596c8eec5f709c2de0898e7c9c1b519c0a20d98d3
anchor-sha256: src/plan/target/xr_target_entry_abi.h 80cd119cbc095ddfddbf95ff5085fbaa23659256feb8d18a36e43416013747ea
anchor-sha256: src/plan/target/xr_target_entry_abi.c cb5cd57a0b8f3bbfe2123a07f583da997d7d2989e5158fd241406b96ce433b12
anchor-sha256: src/plan/target/xr_target_instruction_gen.h bc1422b787b4899738489be4ff807caa23060bc15d710409e3071742c4fb6554
anchor-sha256: src/vm/xr_vm_dynamic_entry.h e365e02d0596394df881026895d127ac54ade9d7bccf5ff272ad6f704a88becf
anchor-sha256: src/vm/xr_vm_ops.def d686e48d8be34cadac9f5c647687d1f02653e4614fbc0b07acfd29991b89d905
anchor-sha256: src/runtime/xr_dynamic_entry_runtime.h bfef11f302f449287eb807856c9258c42269cfbb5df041d55d96035a3f391f1a
anchor-sha256: src/runtime/xr_dynamic_entry_runtime.c 27ff412fb778fd0b9ce603e52c44e39763c42bc12e1c099f2c0a747c014299a4
anchor-sha256: tests/unit/runtime/test_dynamic_entry_runtime.c 233b626fd132326e2bc6946fdaed8686e1280f7ebee98fb0f4e9d9acbad317c2
anchor-sha256: tests/unit/CMakeLists.txt 6f771eaf34fed4987a80bbc23ca6d514a1af35fc58d68788eaf38a1e0fd7f241
