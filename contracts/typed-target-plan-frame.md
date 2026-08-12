# Typed TargetPlan frame contract

The typed frame is a runtime-only consumer of an immutable, independently
verified TargetPlan. It accepts exactly TargetPlan schema 14 with the complete
scalar, aggregate, direct-local call, closure-storage, minimal coroutine
state-call, String-literal-storage, direct-local-callee-storage, and
Channel-allocation-storage, Channel-receive-storage, and
direct-local-GO-callee-storage family mask. Schema 14 is a breaking hard
cutover: schema 13 and earlier and a plan missing any required family fact are rejected
rather than reinterpreted. A schema or required family change must update this
boundary atomically; an older or partial plan is never interpreted through
compatibility logic.

The frame allocator is deliberately narrower than the accepted plan. It
allocates and accesses only the selected function's packed, trivial scalar
slots. Every slot access repeats the stable slot identity, physical size,
alignment, register representation, and memory representation from the plan.
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
The direct-local-callee-storage family describes only the borrowed dynamic
outer `XrValue` token loaded from one shared slot whose every use is the callee
of the same frozen direct-local call target. It grants no closure allocation,
body, root map, cleanup, or general indirect-call authority and remains outside
the trivial frame allocator.
The direct-local-GO-callee-storage family similarly describes only a borrowed
dynamic outer callable token produced by one canonical shared-slot
initializer and used exclusively as operand zero of exact `XI_GO` sites for
one canonical local child. It grants no GO result/task object, callable body,
allocation, root map, cleanup, argument storage, or execution path and remains
outside the trivial frame allocator.
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
SOURCE_EXPORT rows bind only an exact dependency public wrapper and its
coroutine state/result relation. They deliberately contain no local callee
index or cross-module argument/frame storage, so this typed frame grants them
no execution path.

The separate scalar dispatcher may use this frame only when an independently
verified, non-empty function instruction group grants its exact execution
family. Zero instruction rows mean execution unavailable. The coroutine
state-call family proves only frozen state/resume/direct-call/result
relations; it contains no child-frame, spill, root, cleanup, drop, cancel, or
action authority. Aggregate, rooted, owned, caller-storage, coroutine, and
adapter execution is not implemented here and remains fail closed.

The plan also admits one sealed non-static call descriptor: an exact,
non-super, non-suspending `Channel.close()` operation reconstructed from
frozen SemanticPlan receiver, selector, arity, and Unit-result facts. The
receiver is only the dispatch target. The row has no callee function,
argument row, receiver slot, caller storage, or general method-call authority,
and this frame contract grants it no execution path.

The frame retains the plan, binds its exact fingerprint, keeps initialization
and poison state outside the untagged byte arena, enforces hard arena/slot/total
allocation budgets, and makes cleanup terminal. Frame creation recomputes the
target-content fingerprint without consulting SemanticPlan. No runtime type
tag, source type inference, legacy bytecode frame, or AOT value-plan fallback
may authorize an access.

Evidence:

- `test_typed_frame` proves exact schema/family/fingerprint rejection, packed
  access identity, initialization/poison/cleanup state, and allocation budgets.
- `test_typed_frame_runtime_archive` proves the public header and symbols link
  from the runtime-only archive without compiler or AOT ownership, and proves
  that the scalar dispatcher is present without activating it.
- `test_runtime_generation` proves that only a sole-function, nonempty scalar
  instruction plan may activate and consume this frame through the bounded
  generation executor; rooted, call, adapter, and coroutine plans remain
  unavailable.
- The runtime artifact archive gate separately proves activation only through
  the exact XSM/XTP sole-function generation route.

anchor-sha256: src/plan/target/xr_target_plan.h 21f02d03a1dc54b1078f37d82b2eb4ea4c80debfb0f3a3c174cea8f30910d4b7
anchor-sha256: src/vm/xr_typed_frame.h b9776f1b65e625fcd8d343d657773d8d3ad11d875dfe340b6e44cb754778f9ad
anchor-sha256: src/vm/xr_typed_frame.c f0a3c7ea24cc7b712ac8de2923e92ac8bbb5ddc85006878b147ab9d506fd6ac6
anchor-sha256: tests/unit/vm/test_typed_frame.c 8e060669f55b27cf072edd0a83c8a1304b7c9700a286fa09ad720aff21dbd816
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c ec524851384230dc1cd076b9a3e7356d259fde9fb294fc13576ac10b42348470
anchor-sha256: tests/unit/runtime/test_runtime_generation.c 993a338ba5dd2f0ed7a88f4aa830e697700361d88acd2b6ef36f35bcafc270a7
