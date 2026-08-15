# Typed TargetPlan frame contract

The typed frame is a runtime-only consumer of an immutable, independently
verified TargetPlan. It accepts exactly TargetPlan schema 32 with the complete
required family closure the production builder completes, and nothing else: the
accepted mask is that whole closure rather than a hand-kept subset of it, so a
family added to the closure cannot leave this boundary silently rejecting every
plan the builder emits.
Schema 31 is a breaking hard cutover: schema 30 and earlier and a plan missing
any required family fact are rejected
rather than reinterpreted. A schema or required family change must update this
boundary atomically; an older or partial plan is never interpreted through
compatibility logic.

The frame allocator validates only the selected function's packed slots; an
unrelated representation in another function cannot make an otherwise exact
frame unavailable. It transports complete object representations for every
scalar width, floating-point value, Rune, unit-enum ordinal, trivial aggregate,
and trivial raw pointer. Rooted, owned, borrowed, View, object-reference,
code-reference, dynamic, and vector slots remain fail closed. Every access
repeats the stable slot identity, physical size, alignment, register
representation, and memory representation from the plan, and both
representations must be storage-compatible with the slot's exact root and
ownership facts.

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
materialization contract. It adds no object body, allocation, root-map,
root-slot, cleanup, tuple, or general owned-String authority and remains
outside the trivial frame allocator.
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
one canonical local child. It grants no GO result/task object, callable body,
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
knowledge of blocks or edges. The frame's supported family mask is the exact
completed closure the production builder emits, so a plan built for any other
closure is refused rather than executed against a frame that never saw one of
its families. The coroutine
state-call family proves only frozen state/resume/direct-call/result
relations; it contains no child-frame, spill, root, cleanup, drop, cancel, or
action authority. Aggregate, rooted, owned, caller-storage, coroutine, and
adapter execution is not implemented here and remains fail closed;
representation transport alone does not grant any such execution family.

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
slot-state bytes, and their checked exact total. A Release frame carries zero
slot-state metadata bytes. The report does not estimate allocator bookkeeping,
fragmentation, or any storage outside the frame allocation it owns.

Evidence:

- `test_typed_frame` proves exact schema/family/fingerprint rejection, the
  scalar/enum/aggregate/trivial-raw-pointer transport matrix, unaligned caller
  buffers, exact-size and guard-byte boundaries, fail-closed rooted, owned,
  borrowed, View, object/code-reference, dynamic, and vector slots,
  frozen-arena rejection after retained-plan geometry corruption,
  initialization/poison/cleanup state, allocation budgets, the exact footprint
  sum, and its total-limit boundary. Schema-only representation fixtures do not
  claim production-builder reachability.
- `test_typed_frame_runtime_archive` proves the public header and symbols link
  from the runtime-only archive without compiler or AOT ownership, proves the
  footprint and exact slot transport symbols are present there, and proves that
  the scalar dispatcher is present without activating it.
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

anchor-sha256: src/plan/target/xr_target_plan.h 75d5c291efec7caafc4c16ec109e80eee8e982310cdefbd3e49ad6d96eac5b05
anchor-sha256: src/vm/xr_typed_frame.h 43c04fe662cab6f2d79eca9171450d71a43805864af34e90a1f69d11f31427fe
anchor-sha256: src/vm/xr_typed_frame.c 898f5a49db5ce3676e8f21a1835812034d05fe646e2b21931ada4b571fb391fc
anchor-sha256: src/vm/xr_typed_dispatch.c ef9ad355401e587b4d27f3189f01c0b0e7779145e0aa1089a70a3dfca9d9147a
anchor-sha256: scripts/check_typed_call_staging.py 70224976eb831b98465bd1f719f2e66bd81fdf59503a4c929a2542b2081e8655
anchor-sha256: tests/benchmarks/target-machine/typed_target_vm/benchmark.c a59c3711715175cbf009341e15720bff4457d8435e37e3252145ef49cc8fe7d1
anchor-sha256: tests/benchmarks/target-machine/typed_target_vm/run.py e9e057b890df32005e054290a3425f239c1b3c594432a044a04bd6a12dc6dd14
anchor-sha256: tests/unit/vm/test_typed_frame.c 8f2acc619749a086bf3be97ef9799ea6abef2c45151113b13a87b9a3609f8101
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c 0f9fadd7713412a5f7aa808f7461348751270b5210d1feee085b09d5cac47ead
anchor-sha256: tests/unit/runtime/test_runtime_generation.c 42bfb35e761bf2a0d187e35c1cc28a2173caa43e919bd9cb471a4415896edef1
