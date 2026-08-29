# Typed TargetPlan frame contract

The typed frame is a runtime-only consumer of an immutable, independently
verified TargetPlan. It accepts exactly TargetPlan schema 56 with the complete
required family closure the production builder completes, and nothing else: the
accepted mask is that whole closure rather than a hand-kept subset of it, so a
family added to the closure cannot leave this boundary silently rejecting every
plan the builder emits.
Schema 40 is a breaking hard cutover: schema 39 and earlier and a plan missing
any required family fact are rejected
rather than reinterpreted. A schema or required family change must update this
boundary atomically; an older or partial plan is never interpreted through
compatibility logic.

TargetPlan schema 56 additionally permits one bounded program graph whose
function, slot, value, instruction, call, argument, debug, layout, extent, and
capability rows are globally indexed. Canonical module partitions provide
pointer-free ranges and a local SemanticPlan owner index; they are not separate
plans or independently executable frames. The existing typed frame continues
to select one global function and its exact global slot range. For the exact
two-module scalar graph, the dispatcher admits only the verified graph entry as
the external root; `PROGRAM_DIRECT`/`CALL_DIRECT_I64` creates the callee frame
from the same verified plan and copies its exact argument through global slot
rows. No second plan or independently executable partition exists, and the VM
does not translate local indexes, stitch per-module fingerprints, or expose the
producer as another entry.

The bounded `XR_TARGET_EXECUTION_LEAF_AGGREGATE_I64X2` family transports one
complete trivial aggregate as two positional signed-`i64` fields. Its exact
native layout is 16-byte size and 8-byte alignment, with fields at offsets 0 and
8. Argument transfer, callee parameters, aggregate construction, field reads,
return, and caller storage all use the exact slots and representations in the
verified plan. The frame creates no tag, root, cleanup, place, or aggregate
reference for that value and does not infer layout from a C type, Xi node, or
source name. Every other aggregate execution family remains unavailable.

The frame allocator validates only the selected function's packed slots; an
unrelated representation in another function cannot make an otherwise exact
frame unavailable. It transports complete object representations for every
scalar width, floating-point value, Rune, unit-enum ordinal, trivial aggregate,
and trivial raw pointer. It also accepts an exact immutable String literal as
a no-lifecycle prerequisite carrier, one exact owned dynamic String whose
coroutine root and release lifecycle is complete, and the two exact short-lived
DYN_VALUE parameters of the managed source-class tagged `Array.push` family.
The push receiver is borrowed transport and never enters the lifecycle ledger;
its element is an owned carrier governed by the executable `CONSUME` operand.
Every other rooted, owned, borrowed, View, object-reference, code-reference,
dynamic, and vector slot remains fail closed. Every access
repeats the stable slot identity, physical size, alignment, register
representation, and memory representation from the plan, and both
representations must be storage-compatible with the slot's exact root and
ownership facts.

A schema-36 dynamic SOURCE_EXPORT function may also retain namespace-resolution
slots in its verified TargetPlan function layout. Those slots are not operands,
results, or call arguments of the generated instruction group. The frame keeps
them uninitialized and refuses every access to them; it does not pretend to
transport, root, or clean them. Only the exact trivial signed-`i64` slots proved
reachable from generated rows are materialized by this narrow family.

This byte transport is not a second lifecycle authority. It never forms an
unaligned typed pointer to arena storage, dereferences a carried reference,
guesses a tag from caller bytes, or retains/releases an object. View ownership,
object roots, borrows, cleanup, and pointer provenance remain governed by the
independently verified plan and executor. Object/code reference rows are
schema-valid but not currently emitted by the production builder and remain
unavailable to this frame until their lifecycle contracts and executor
operations are independently frozen.
The closure-storage family may contain a dynamic/owned/tagged outer `XrValue`
slot for an exact no-capture heap closure, but it does not describe the closure
object body, allocation, root map, root slot, or cleanup. Such a slot remains
outside this trivial frame allocator; Semantic ownership and the existing AOT
closure lifetime path retain those responsibilities.
The String-literal-storage family likewise describes only a dynamic/owned/
tagged outer value backed by the separate runtime String-literal
materialization contract. An exact String constant with complete
`BORROWED_STATIC` return provenance may be copied as an immutable prerequisite
carrier without lifecycle metadata. It adds no object body, allocation,
root-map, root-slot, cleanup, tuple, or general owned-String authority.
The array-member-result-storage family describes only the dynamic/owned/tagged
outer value of an array member that hands back its own receiver. That result is
a second name for a container the array-allocation family already describes, so
this family adds no element storage, object body, allocation, root map, root
slot, cleanup, or layout of its own, and its slot stays outside the trivial
frame allocator.
The direct-local-callee-storage family describes only the borrowed dynamic
outer `XrValue` token loaded from one shared slot whose every use is the callee
of the same frozen direct-local call target. The target is the unique canonical
child of the first lexical shared-slot owner: either the caller itself or the
module root whose entry-prefix initializer precedes activation. This includes
root-owned sibling helpers but rejects an arbitrary ancestor, duplicate store,
or unrelated sibling. It grants no closure allocation, body, root map, cleanup,
or general indirect-call authority and remains outside the trivial frame
allocator.
The direct-local-GO-callee-storage family similarly describes only a borrowed
dynamic outer callable token produced by one canonical shared-slot
initializer and used exclusively as operand zero of exact `XI_GO` sites for
one canonical local child. The slot index names one entry of the single
module-wide shared table whatever function reads it, so the initializer is
looked up by that index alone and has to sit in the scope owning the slot,
which is the frozen child's parent. Only a store the loading function makes
itself carries dominance; a store in an enclosing owner is instead required
to be that scope's entry-prefix initializer before any activation. This
covers a module-level helper started from a nested function and rejects a
store in an unrelated scope. It grants no GO result/task object, callable body,
allocation, root map, cleanup, argument storage, or execution path and remains
outside the trivial frame allocator.
The direct-local-GO-task-result-storage family owns the complementary outer
carrier: the exact `Task<T>` returned by that proved GO. It is a borrowed
dynamic/tagged temporary because the executor owns the task object. It grants
no object body, allocation, root slot, cleanup, scheduling, await result, or
coroutine execution path and remains outside the trivial frame allocator.
The Channel-allocation-storage family describes the dynamic outer `XrValue`
returned by an exact frozen `XI_CHAN_NEW` as owned, and its exact identity-copy
aliases as borrowed. It does not describe the Channel object body, allocation
execution, root map, root slot, cleanup, transfer plan, or general object
storage and remains outside the trivial frame allocator.
The Channel-receive-storage family describes only the trivial scalar result
slot for an exact `XI_CHAN_TRY_RECV` whose Channel allocation identity and
element type have been independently frozen. It adds no Channel object layout,
receive scheduling, ownership transfer, aggregate payload, tuple payload,
root, or cleanup authority.
The direct-local-unit-enum-argument-storage family describes only a trivial
signed 64-bit ordinal whose source enum declaration, unit shape, nominal layout,
and exact direct-local parameter/argument relation are independently frozen.
It grants no payload enum, boxing, allocation, root, cleanup, dispatch, or
general enum inference authority.
The source-class-object-storage family describes only the owned dynamic outer
`XrValue` of an exact frozen class allocation, named through the plan's own
source-class table rather than through its erased result type. It grants no
class object body, field or method table, construction, root, cleanup, or
member lookup, and remains outside the trivial frame allocator.
The source-class-receiver-storage family describes only the outer dynamic
`XrValue` of the instance a constructor is entered with, named through the
function the plan records as the constructor of one frozen declaration rather
than through its own type row, which names no declaration at all. Its slot is a
parameter role because the value is bound on entry rather than computed, and
its ownership is the parameter's own recorded ownership. It grants no class
body, field or method table, allocation, root, cleanup, or member lookup, and
remains outside the trivial frame allocator.
The SOURCE-import-storage family describes only borrowed dynamic outer
`XrValue` tokens in exact namespace-receiver or named-export-callee
import/store/load chains consumed by a SOURCE_EXPORT call. It grants no imported
module object body, allocation, root, cleanup, dependency activation, or
cross-module frame and remains outside the trivial frame allocator.
SOURCE_EXPORT rows bind an exact dependency public wrapper, coroutine
state/result relation, and dense argument contracts. `ref` arguments bind a
caller place and one additional C pointer level, but no local callee slot or
executable cross-module frame is introduced, so this typed frame grants them no
execution path.
The ADT-enum-storage family describes only exact source-backed enum parameters,
direct-local enum returns, and payload-bearing constructor results as outer
tagged values. Its exact declaration, member, layout, discriminant, payload
order, receiver, and ownership come from the frozen plans; it grants no enum
object body, guessed type/name/arity, root, cleanup, or generic dispatch and
remains outside the trivial frame allocator.

The separate scalar dispatcher may use this frame only when an independently
verified, non-empty function instruction group grants its exact execution
family. Zero instruction rows mean execution unavailable. A trivial
signed-`i64` parameter-role slot is an ordinary uninitialized slot of this
frame at creation: the dispatcher fills it through the verified parameter row
that names its argument ordinal, so no slot is ever live before a store and
the frame gains no argument-passing authority of its own. A slot the
dispatcher writes on one path and reads on another is proved defined by the
instruction group's own control-flow judgement before the frame is created, so
the frame keeps its single rule that a load requires a prior store and gains no
authority to choose blocks or edges. An uncached dispatcher asks the frame to
reconstruct the next instruction and block entry from the verified rows. After
an exact immutable decoded cache has already been bound to the same plan, the
dispatcher may instead pass that cache's instruction and block-entry identities
through the frame's O(1) decoded-entry operation. A program-graph cache must
also bind the exact runtime generation, program fingerprint, canonical
module-set fingerprint, and GCI before any frame entry. That operation only
updates
frame context; it cannot build a cache, choose a successor, or make an invalid
plan executable. The frame's supported family mask is the exact
completed closure the production builder emits, so a plan built for any other
closure is refused rather than executed against a frame that never saw one of
its families. One-shot scalar dispatch independently requires zero root-map,
cleanup, and coroutine rows for the selected function before frame creation,
and repeats zero lifecycle bytes before every destruction. Persistent scalar
coroutine dispatch instead requires a nonempty exact coroutine partition and
zero root-map/cleanup rows. Its frame may enter the resume block only after the
current `SUSPEND` row binds the same state and resume consumes that binding.
The state row separately freezes the function-local resume instruction, so a
row immediate cannot redirect the frame to another block. The permission is
one-shot: entering early, entering a different instruction, resuming a
different state, or consuming the same resume twice is rejected. The same
packed frame remains materializable while suspended and is reused until normal
return, error, cancellation, or destruction; no legacy value stack or retained
bytecode frame is attached. The broader coroutine state-call family still
proves frozen state/resume/direct-call/result relations. A
separate exact lifecycle relation may additionally name one String-concat
producer, suspend state, owner, root, and normal release. TargetPlan reconstructs
that relation as one root map/root slot plus normal and `CANCEL|EXIT` release
cleanups. The frame accepts such a managed slot only when the complete relation
is present: store activates ownership, state bind and root visit require it,
resume clears the bound state, and a status-returning cleanup executor must
succeed before bytes are zeroed and ownership becomes released. Executor
failure preserves the active bytes for retry. Destruction takes an owning
pointer and refuses an active slot without changing or losing that pointer.
It also refuses to clean or free a parent while a child remains linked; only
successful child cleanup severs that link, so no live child is silently
detached and made unreachable.
Lifecycle admission, state binding, root visitation, and frame-level cleanup
execution
consume only the selected verified function's `root_begin/root_count` or
`cleanup_begin/cleanup_count` partition with checked bounds. They never scan
another function's rows; the regression fixture adds 8,192 unrelated root and
cleanup rows while preserving the target function's exact result. The
registered mutation gate rejects a return to either global-table scan.

Managed tagged push is not admitted through the scalar or coroutine lifecycle
families. Its owned parameter becomes active only after the exact caller
carrier is stored. Immediately before the shared kernel, the executor performs
the one permitted `ACTIVE` to `TRANSFERRED` move. Kernel success commits that
transfer with no later fallible lifecycle step. Kernel failure performs the
single exact reverse transition to `ACTIVE`, after which dispatch returns the
same carrier to the caller; a different value, slot, function, or execution
family cannot use either transition. Frame destruction refuses an active owner
and accepts the transferred success state, so no success or failure path can
silently duplicate or lose the element owner.

The production lifecycle executor is a separate runtime-only consumer of that
exact frame contract. Initialization independently requires an intact verified
plan, exact native hosted TargetProfile, and the selected function's complete
partition: every admitted owner is named by one exact root slot, one normal
`RELEASE` row, and one state-matching `CANCEL|EXIT` `RELEASE` row. A dynamic
carrier tag only validates the representation after the plan has selected the
owner; no tag, legacy heap selector, mutable Xi type, name, or fallback can
create cleanup authority. Normal exit consumes the normal row. Cancel consumes
the terminal row through its `CANCEL` bit, while error and return consume the
same existing terminal row through its `EXIT` bit. This mapping adds no new
schema row or error-specific cleanup authority.

Before dereferencing the opaque carrier address, the executor requires the
allocation owner to resolve that exact address. It then validates zero-required
padding, the native object-reference tag and flags, and the canonical `XrString`
prefix before using the canonical object-header release operation. A successful
last release is handed to the required infallible allocation-owner reclaimer.
Only after release and any reclamation succeed may the optional observer receive
an event. That event carries the verified slot stable identity, not an ownership
certificate identity. The diagnostic-only ownership adapter independently
reconstructs the canonical certificate owner and release-event identities from
the verified SemanticPlan certificate and the selected TargetPlan
root/slot/cleanup facts, requires the exact native String target authority, and
can compare either logical-only release evidence or logical plus local physical
RC evidence. Normal and terminal cleanup rows realize the same certificate
release transition; the exit kind remains an independently checked observation.
Any failure before physical release leaves the frame lifecycle active and
preserves its carrier bytes for retry. Observer rejection occurs only after the
irreversible release and reclamation have succeeded, so the executor commits
the released carrier and reports audit rejection without making it retryable.
A successful release likewise zeroes the carrier and makes a second attempt
exact-once inactive.

The scalar dispatcher remains unchanged and still rejects every selected
function with lifecycle rows. This executor does not execute String concat,
length, yield/resume instructions, child continuations, scheduling, arbitrary
owned types, panic unwinding, or a general coroutine instruction stream. The
separate managed-value entry executes only the exact tagged push group and
grants no general managed instruction-stream authority.

The plan also admits one sealed non-static call descriptor: an exact,
non-super, non-suspending `Channel.close()` operation reconstructed from
frozen SemanticPlan receiver, selector, arity, and Unit-result facts. The
receiver is only the dispatch target. The row has no callee function,
argument row, receiver slot, caller storage, or general method-call authority,
and this frame contract grants it no execution path.
It also admits one sealed zero-argument `StringBuilder()` constructor call
descriptor. The row binds the exact Semantic allocation identity to an owned
dynamic result slot and grants no generic builtin dispatch, object-body
layout, cleanup, or typed execution path.

The frame retains the plan, binds its exact fingerprint, freezes the actual
arena size, alignment, and selected slot range, keeps initialization and poison
state outside the untagged byte arena, enforces hard arena/slot/total allocation
budgets, and makes cleanup terminal. Frame creation recomputes the
target-content fingerprint without consulting SemanticPlan. Each load/store
uses checked offset-plus-size arithmetic against the frozen arena and actual
allocation before forming a byte pointer, requires the complete exact slot
size, and copies through `memcpy`. No runtime type tag, source type inference,
legacy bytecode frame, or AOT value-plan fallback may authorize an access.
The read-only memory-footprint query reports the fixed frame object, the exact
packed-plan arena allocation, its bounded alignment padding, optional audit
slot-state bytes, sparse lifecycle metadata, and their checked exact total.
Lifecycle metadata contains only sorted global slot IDs and states for slots
with the complete managed contract. A function with no such slot allocates no
lifecycle arrays, reports zero lifecycle bytes, and its scalar load/store path
does not read lifecycle state. A Release frame also carries zero audit
slot-state metadata bytes. The report does not estimate allocator bookkeeping,
fragmentation, or any storage outside the frame allocation it owns.

Evidence:

- `test_typed_frame` proves exact schema/family/fingerprint rejection, the
  scalar/enum/aggregate/trivial-raw-pointer transport matrix, unaligned caller
  buffers, exact-size and guard-byte boundaries, fail-closed rooted, owned,
  borrowed, View, object/code-reference, dynamic, and vector slots,
  frozen-arena rejection after retained-plan geometry corruption,
  initialization/poison/cleanup state, allocation budgets, the exact footprint
  sum, and its total-limit boundary. It also proves an exact managed slot at a
  nonzero local index with ordinary scalar slots on both sides; sparse lookup,
  root visitation, resume, normal release, cancel release, failed-cleanup retry,
  exact-once success, active-owner destruction refusal, and zero lifecycle
  metadata for ordinary scalar frames. The same fixture proves that 8,192
  root and cleanup rows belonging to another function do not enter target
  bind, visit, or cleanup work. Schema-only representation fixtures do
  not claim production-builder reachability.
- `test_target_plan` integrates the exact managed push group with both generated
  dispatcher providers and proves borrowed receiver preservation, owned
  element transfer on success, exact restoration on each failure, and refusal
  before ownership movement for invalid carrier categories.
- `test_typed_frame_runtime_archive` proves the public header and symbols link
  from the runtime-only archive without compiler or AOT ownership, proves the
  footprint, exact slot transport, trace, profile, TargetPlan debug control,
  and materialization symbols
  are present there, and proves that the scalar dispatcher is present without
  activating it.
- `test_typed_lifecycle` builds the production owned-String concat lifecycle with
  an exact native hosted TargetProfile and proves normal, error, cancel, and
  return release; last and non-last exact-once behavior; resolver failure with
  unchanged bytes followed by successful retry; zero-padding rejection;
  post-reclamation observer ordering; duplicate-terminal-row rejection; and
  fail-closed admission of a non-native TargetProfile, a mismatched root slot,
  and a scalar function with no lifecycle contract.
  `test_typed_frame_runtime_archive` additionally proves that this executor's
  public symbols are present in the runtime-only archive.
- `test_typed_lifecycle_audit` independently joins that exact TargetPlan
  partition to the SemanticPlan ownership certificate and proves canonical
  owner/event reconstruction, logical event acceptance on normal, error,
  cancel, and return exits, optional physical RC comparison, last and non-last
  releases, post-reclaimer ordering, resolver retry, exact-once double-call
  rejection, and fail-closed slot, target-plan, certificate, and observer
  mutation. The adapter remains absent from release VM/runtime archives and
  does not claim other owner kinds or lifecycle transitions.
- `typed_target_vm_performance_gate` directly times the verified scalar
  dispatcher, one exact adapter-free `CALL_DIRECT_I64`, and packed frame
  allocation on Windows Release. The call fixture proves that argument
  staging reads the immutable `XrTargetCallArgumentRecord` rows directly and
  reports zero runtime generic-argument-array bytes; the anchored dispatcher
  source and `typed_target_vm_call_staging_contract` keep that claim fail
  closed by rejecting generic value carriers or runtime argument allocation.
  The gate consumes the frozen
  target-machine warmup, sample-count, CPU, power-policy, and variation rules;
  records all three lanes as baselines until numeric typed-executor budgets are
  frozen; and gates zero Release slot-state metadata plus the exact bounded
  footprint independently of timing.
- `test_runtime_generation` proves that only a sole-function, nonempty scalar
  instruction plan may activate and consume this frame through the bounded
  generation executor; rooted, call, adapter, and coroutine plans remain
  unavailable.
- The runtime artifact archive gate separately proves activation only through
  the exact XSM/XTP sole-function generation route.
- Dynamic `CALL_ENTRY` treats child-frame disposal and opaque lease retirement
  as independent all-exit obligations. The runtime-owned context consumes the
  resolution even when immediate retirement is deferred, so a fallible frame
  cleanup cannot make the generation pin stack-local or unreachable.

anchor-sha256: src/plan/target/xr_target_plan.h 794940f065e2564f3c2c08fdb05fcea82a33076a6261f75d669ee9b0e448ddcb
anchor-sha256: src/vm/xr_typed_frame.h 1a139fbf8e4dfe08169fa67186c889c79665639f28674f5ecf53babd4f83120c
anchor-sha256: src/vm/xr_typed_frame.c 749f45bf957f82be3142e9aa9565b7bf9020b0f29ff494709bb4c5a900edea53
anchor-sha256: src/vm/xr_typed_dispatch.c 1a1e3482e9ca79eb0c6896c410210fafec6ca3358b95911119b1d6720f5bb1be
anchor-sha256: scripts/check_typed_call_staging.py 2d98ea1490d028149e705a25519a94ded9ed19153afe66929cadc0c47d45acba
anchor-sha256: tests/benchmarks/target-machine/typed_target_vm/benchmark.c 26ce3e68c82fae8c1f035ea4daff3da8bd909146ca359d7e7b56983bed68b8a3
anchor-sha256: tests/benchmarks/target-machine/typed_target_vm/run.py 1e63120e1b93825e3103489317a2202d78b383135505c2215f39b22b94972041
anchor-sha256: src/vm/xr_typed_lifecycle.h 46c945304a46a967dcf6cd95ef51cfa1a4d6fe74c3ca10084a3baab97492167a
anchor-sha256: src/vm/xr_typed_lifecycle.c 8b89bc15c8fa9e5e926cc939a09203d81a63a549f6f8bb1f6570671b7e02995d
anchor-sha256: src/vm/audit/xr_typed_lifecycle_audit.h 85dab081a8f11822651cc686648d97ca228cdb97a2c406fc3aca9fce70d1fbdb
anchor-sha256: src/vm/audit/xr_typed_lifecycle_audit.c 291e460b9004adfe43491e1f1c0054bac1cf2af8e137917ebb4e2760868573c3
anchor-sha256: tests/unit/vm/test_typed_frame.c 900c5800d9fce2a12188425f19c83648749c13b7d612c2e8fc7ef3d7f046e2da
anchor-sha256: tests/unit/runtime/test_typed_lifecycle_audit.c 25ea03d2d62ab5086f2f9634bf1a819d3eec577a60f327d26df60c671fbe8ce5
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c 3f49976a53aa6422da074107bedd4e0afd428fc76018bc4c44f144a8bc33a61e
anchor-sha256: tests/unit/runtime/test_runtime_generation.c 0332f8c2423f606919ffab298a69ee14b863c4f5e590b5d51aa3c39f9344f14c
anchor-sha256: src/vm/xr_vm_dynamic_entry.h 50a175071a41a521e11fa672b7f17663b73dda321fd6ab9703196324e642dfda
anchor-sha256: tests/unit/runtime/test_dynamic_entry_runtime.c 1c09d9175acb0870f42515071a88cdf6beb1f28becdb179fd9b3435945bed680
anchor-sha256: scripts/check_coroutine_lifecycle_projection.py 74fdc88cea8045a258dae39f2194839ec54f3a2b8759fa56f1b537226fdbc1a2
