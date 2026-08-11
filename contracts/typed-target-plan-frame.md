# Typed TargetPlan frame contract

The typed frame is a runtime-only consumer of an immutable, independently
verified TargetPlan. It accepts exactly TargetPlan schema 7 with the complete
scalar, aggregate, direct-local call, closure-storage, and minimal coroutine
state-call family mask. Schema 7 is a breaking hard cutover: schema 6 and a
plan missing either new family fact are rejected rather than reinterpreted. A
schema or required family change must update this boundary atomically; an
older or partial plan is never interpreted through compatibility logic.

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
family. Zero instruction rows mean execution unavailable. The coroutine
state-call family proves only frozen state/resume/direct-call/result
relations; it contains no child-frame, spill, root, cleanup, drop, cancel, or
action authority. Aggregate, rooted, owned, caller-storage, coroutine, and
adapter execution is not implemented here and remains fail closed.

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

anchor-sha256: src/plan/target/xr_target_plan.h 8f8b9ffd674a9d087e2c27389127e8b2b45aa3e41421f2a1f5babfc98a0b7937
anchor-sha256: src/vm/xr_typed_frame.h 4d73a8fe22467d24629f954a109aa333ca027e0fd56d516bf854994af450361e
anchor-sha256: src/vm/xr_typed_frame.c f0a3c7ea24cc7b712ac8de2923e92ac8bbb5ddc85006878b147ab9d506fd6ac6
anchor-sha256: tests/unit/vm/test_typed_frame.c 8e060669f55b27cf072edd0a83c8a1304b7c9700a286fa09ad720aff21dbd816
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c f323162dff476e2782c8f522e35d0911cf9c07ba58b050d20890b80cad9b7319
