# Typed TargetPlan frame contract

The typed frame is a runtime-only consumer of an immutable, independently
verified TargetPlan. It accepts exactly TargetPlan schema 6 with the complete
scalar, aggregate, direct-local call, and closure-storage family mask. Schema
6 is a breaking hard cutover: schema 5 and a plan missing the new family bit
are rejected rather than reinterpreted. A schema or required
family change must update this boundary atomically; an older or partial plan
is never interpreted through compatibility logic.

The frame allocator is deliberately narrower than the accepted plan. It
allocates and accesses only the selected function's packed, trivial scalar
slots. Every slot access repeats the stable slot identity, physical size,
alignment, register representation, and memory representation from the plan.
The closure-storage family may contain a dynamic/owned/tagged outer `XrValue`
slot for an exact no-capture heap closure, but it does not describe the closure
object body, allocation, root map, root slot, or cleanup. Such a slot remains
outside this trivial frame allocator; Semantic ownership and the existing AOT
closure lifetime path retain those responsibilities.

The separate scalar dispatcher may use this frame only when an independently
verified, non-empty function instruction group grants its exact execution
family. Zero instruction rows mean execution unavailable. Aggregate, rooted,
owned, caller-storage, coroutine, and adapter execution is not implemented
here and remains fail closed.

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
  that the internal scalar dispatcher is present without activating it.

anchor-sha256: src/plan/target/xr_target_plan.h f0f6ab6ba863d8d53754ff936e92988faba08cb140db31c75a69ae66575e085c
anchor-sha256: src/vm/xr_typed_frame.h 43c80c98cb33d420f2ad269c2c04dbccdf2f039286e44fb17dd2a6260297617e
anchor-sha256: src/vm/xr_typed_frame.c f0a3c7ea24cc7b712ac8de2923e92ac8bbb5ddc85006878b147ab9d506fd6ac6
anchor-sha256: tests/unit/vm/test_typed_frame.c 8e060669f55b27cf072edd0a83c8a1304b7c9700a286fa09ad720aff21dbd816
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c 433d3581cbf29716cc2865214de9a6e1b64cc25e9d6a32fec870f6f6b5b06f40
