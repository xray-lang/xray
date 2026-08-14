# Typed TargetPlan scalar execution contract

TargetPlan schema 19 may carry a canonical per-function instruction table.
Instruction authority is separate from the production AOT family mask: a
verified plan can remain a complete AOT plan while exposing no typed execution
family. A function with zero instruction rows is execution unavailable, never
an empty successful program. The production builder emits a complete group
only for a verified, capture-free signed-`i64` function whose declared
parameters are all exact signed `i64`, whose blocks are all returns, plain
jumps, or two-way branches, and whose operations are entirely in the supported
family. Every other function emits zero rows; no partial group or fallback is
allowed.

The only supported execution family is a closed signed `i64` program consisting
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

Instruction rows participate in the TargetPlan fingerprint, the bounded XTP
section directory, the exact 32-byte row codec, and candidate materialization.
The internal scalar dispatcher accepts only an immutable verified plan, its
exact fingerprint, a derived nonzero function execution family, a positional
signed-`i64` argument vector, and exact typed-frame slot identities. It
independently recomputes the target-content fingerprint and does not inspect
SemanticPlan or Xi. The verified rows are the only signature it honours: the
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

Schema 19 is a hard cutover from v18 and all earlier schemas. It preserves all
v17 authorities while adding exact source unit-enum ordinal storage for
direct-local arguments. The family binds a payload-free source enum's stable
declaration and nominal layout to a trivial signed 64-bit target row and the
exact callee parameter/argument relation. It grants no payload enum, boxing,
allocation, root, cleanup, dispatch, or name-based enum authority.
Schema 17 added an exact borrowed `Slice<byte>` view of String storage.
The view binds the frozen Semantic intrinsic and source root to the exact
`xray-target-string-byte-slice-view-v1` call identity; it grants no generic
String method or slice construction authority. Schema 16 added a sealed
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
The dedicated SOURCE-namespace-storage rows cover only the borrowed dynamic
outer `XrValue` tokens in the exact
`IMPORT_REF -> identity COPY* -> SET_SHARED -> GET_SHARED -> identity COPY*`
chain used as SOURCE_EXPORT call receivers. Identity-COPY chains are bounded,
acyclic, and same-function; every endpoint and COPY keeps its own exact slot
identity and unique expected consumer. Three independent reconstructions prove
dependency/module identity, unique shared slot, complete use sets, and receiver
binding. They grant no imported module object body, allocation,
root, cleanup, member lookup, dependency activation, argument ABI, or
cross-module frame.
The C emission projection schema 11 mechanically spells all verified dynamic
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
For the sealed `StringBuilder()` call it owns the zero-operand
`xrt_strbuf_new` materialization recipe. Sync and coroutine CGen have no
name-based fallback for that constructor. For the exact String byte-slice view,
it owns the `xr_span_t` representation, source semantic value, and fixed
`xrt_span_from_string_bytes` recipe. CGen cannot recover these from selector
text, aliases, mutable Xi types, or legacy representation state.

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

The required `COROUTINE_STATE_CALL` family is independent of this
dispatcher. It freezes only the state/resume/direct-call/result-slot relation;
it does not activate coroutine execution or supply child-frame, spill, root,
cleanup, drop, cancel, or action authority.

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
  of running forever.
- `test_xtp_format` proves the instruction row width is part of the complete
  exact codec registry and exercises the public XSM/XTP generation route.
- `test_typed_frame_runtime_archive` proves the dispatcher and verifier link
  into the runtime-only archive; the runtime artifact archive gate separately
  proves the exact XSM/XTP sole-function product route.
- `test_runtime_generation` proves exact sole-function PREPARE, ACTIVE scalar
  execution, unsupported-plan rejection, bounded pins, drain, retirement, and
  unload without any legacy execution fallback. It also proves that a program
  which divides by zero reaches ACTIVE and is stopped at execution as a
  program fault, separate from the authority failures an unsupported plan
  gives.

anchor-sha256: src/plan/target/xr_target_plan.h cbb7527c7515c0cfecb578994751af470d63a30488684c58d8dd4ebb8224fcd6
anchor-sha256: src/plan/target/xr_target_plan.c 868089fd22c110dc090e0236a14ae8941d5d28f66f7c0cd414787050b35f1237
anchor-sha256: src/plan/target/xr_target_builder.c 4e939e7a217b06079fe7591be2e841ba6a7b5f9aa694fa8366f5812af5e3c5ab
anchor-sha256: src/plan/target/xr_target_instruction_verify.h 6099812f9cee4af8b01c5ffb422c9e359cbd95ec7ebc61c927bc051bc2bf904b
anchor-sha256: src/plan/target/xr_target_instruction_verify.c 1b7eb7533380ba84c66c7d772e8bd239e3fad261d8bbc99962a4cd02cdc70f14
anchor-sha256: src/plan/target/xr_target_verify.c 3bdeb7b62152be6df12938f27a7da029cf68692a4cf7cc07ec1785fe6eedec9b
anchor-sha256: src/plan/format/xr_xtp_schema.h 04840cf64073530619483953264b801358984d6559d7928b0b733b265ef2c668
anchor-sha256: src/plan/format/xr_xtp_rows.c 85e8842a3857fd250c68c5cc12b7aba35787461650317dacdb39eaf92da317a9
anchor-sha256: src/plan/format/xr_xtp_encode.c 8cb0983494ace434ec1d1f7389f19d4780ad82f6f88460144e04a9e28c1502bc
anchor-sha256: src/plan/target/xr_xtp_materialize.c 02de4138a0d49d1afd6143cec910cbe1061a6d84d82096d48fa4800852b98267
anchor-sha256: src/vm/xr_typed_dispatch.h 30b893c4f791e6b99a87cf46194c982b63972072675d2bfbc329ab55fcba1b25
anchor-sha256: src/vm/xr_typed_dispatch.c b89abe0916835904f3ea7bc7394e7a8eab23d2f2c37bf3a711d44941858e0591
anchor-sha256: src/vm/xr_typed_frame.c f0a3c7ea24cc7b712ac8de2923e92ac8bbb5ddc85006878b147ab9d506fd6ac6
anchor-sha256: tests/unit/vm/test_typed_dispatch.c 2a372c17d0959bb4e3b086259c54db5623449f77d2b624b7b7c347f05f18545b
anchor-sha256: tests/unit/plan/test_xtp_format.c ab7a3766a721d1aa2e6fc2ca67031e77ad1f7b44f974d5e8b532067f58705801
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c 1a8fcbe84ce64c733d0cb2614f745a1852fbd6991ef9ae8da5683b5f0eab6de5
anchor-sha256: include/xray_runtime_generation.h b8d8ab25bf7945cb6837af74a2460ff52d516714b47c3331f6ce82fbc33c05d0
anchor-sha256: src/runtime/xr_module_generation.c f3fe95413105fbb79fb40b5a0a6f718179b997ad4b823a90baae94a045ba103a
anchor-sha256: tests/unit/runtime/test_runtime_generation.c 42bfb35e761bf2a0d187e35c1cc28a2173caa43e919bd9cb471a4415896edef1
