# Typed TargetPlan frame contract

The typed frame is a runtime-only consumer of an immutable, independently
verified TargetPlan. It accepts exactly TargetPlan schema 5 with the complete
scalar, aggregate, and direct-local call family mask. A schema or required
family change must update this boundary atomically; an older or partial plan
is never interpreted through compatibility logic.

The frame allocator is deliberately narrower than the accepted plan. It
allocates and accesses only the selected function's packed, trivial scalar
slots. Every slot access repeats the stable slot identity, physical size,
alignment, register representation, and memory representation from the plan.
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

anchor-sha256: src/plan/target/xr_target_plan.h 966d08684c5c4db79ecd86180d6a5c590bb23a7ae5dd364e87807836bd7278e4
anchor-sha256: src/vm/xr_typed_frame.h ca926ca9c3dd6753e6554d8ca794ed8eba1af8138137d4d0b1bd07ee2745c85e
anchor-sha256: src/vm/xr_typed_frame.c f0a3c7ea24cc7b712ac8de2923e92ac8bbb5ddc85006878b147ab9d506fd6ac6
anchor-sha256: tests/unit/vm/test_typed_frame.c 8e060669f55b27cf072edd0a83c8a1304b7c9700a286fa09ad720aff21dbd816
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c f3e4f5d2a83f6efbd01e634013a7f213d998d3065e93f6c0419ddae0cd393616
