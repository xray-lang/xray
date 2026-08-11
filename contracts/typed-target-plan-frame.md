# Typed TargetPlan frame contract

The typed frame is a runtime-only consumer of an immutable, independently
verified TargetPlan. It accepts exactly TargetPlan schema 4 with the complete
scalar, aggregate, and direct-local call family mask. A schema or required
family change must update this boundary atomically; an older or partial plan
is never interpreted through compatibility logic.

The current frame executor is deliberately narrower than the accepted plan.
It allocates and accesses only the selected function's packed, trivial scalar
slots. Every slot access repeats the stable slot identity, physical size,
alignment, register representation, and memory representation from the plan.
Aggregate, rooted, owned, caller-storage, coroutine, and adapter execution is
not implemented here and remains fail closed.

The frame retains the plan, binds its exact fingerprint, keeps initialization
and poison state outside the untagged byte arena, enforces hard arena/slot/total
allocation budgets, and makes cleanup terminal. No runtime type tag, source
type inference, legacy bytecode frame, or AOT value-plan fallback may authorize
an access.

Evidence:

- `test_typed_frame` proves exact schema/family/fingerprint rejection, packed
  access identity, initialization/poison/cleanup state, and allocation budgets.
- `test_typed_frame_runtime_archive` proves the public header and symbols link
  from the runtime-only archive without compiler or AOT ownership.

anchor-sha256: src/plan/target/xr_target_plan.h d246e244c5c143567eba0ea36b131e60fcd9e610f2de44ce9560804e380cc3a2
anchor-sha256: src/vm/xr_typed_frame.h 5dca10be2c3c733535bbef4ba631ec1dacd12eb6b2e160b1468281ff9e76b11e
anchor-sha256: src/vm/xr_typed_frame.c 33f664147e01d361aec024451bd28da1518ad096c1a99f15aa416e288654f278
anchor-sha256: tests/unit/vm/test_typed_frame.c 8e060669f55b27cf072edd0a83c8a1304b7c9700a286fa09ad720aff21dbd816
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c 41baa3b05e9b34aec70e1ca10e30c6ae2d50ec0f6e34fd8e27023d2026704b1b
