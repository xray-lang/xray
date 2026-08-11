# Ownership audit manifest and dynamic heap foundation contract

Status: preparatory executed-path oracle frozen by task 274.

This is an independent, bounded runtime evidence kernel. Certificate-to-manifest
generation, real VM/AOT instrumentation, teardown phases, and full task 274
cutover remain later integration gates; none is implied by this leaf module.

1. Static owner and transition manifests are registered before execution.
   Observed events cannot create their own expectations. Every transition is
   named by a stable certificate event ID and exactly binds owner, operation,
   exit/edge, kind, program point, state-before mask, state-after, and logical
   balance delta.
2. Dynamic instances are opened only by an explicit manifest transition and
   keyed by static owner, invocation ID, and nonzero activation epoch. Loops and
   recursion may reuse one static transition across distinct dynamic instances;
   duplicate or stale instance keys fail closed.
3. Layout, allocation origin, frame, generation, premise, destructor, and full
   domain identity are checked against independent manifest/runtime state.
   Domain changes bind the expected contract/category while capturing a
   concrete instance ID that all later observations must match exactly.
4. Borrow activations are scoped to a dynamic instance and closed slots are
   reusable. Generation pins, logical state and balance, terminal disposition,
   and optional observed physical RC before/after telemetry are independently
   checked.
5. Creation preallocates every table. Recording performs no allocation, growth,
   callback, destructor, or provider call. Capacity exhaustion and concurrent
   or reentrant entry poison the audit with a sticky first error. An event is
   validated in temporary state before its object/loan transition is committed.
6. The public report scope is exactly “executed-path dynamic evidence, not
   formal proof”. Optimized paths still require proof mapping or an audit lane
   that preserves logical observations.

## Digest anchors

anchor-sha256: src/shared/xr_ownership_event.h 4ee731782643616d5df34ead901ae39cd995fc91113774eabbe8dd95f982d90d
anchor-sha256: src/plan/ownership/xr_ownership_certificate.h 32648d285043e1cbc01b7ba9870756c59a4cacbdb2ec22d379098635dc7f23ea
anchor-sha256: src/runtime/ownership/xr_ownership_audit.h 461075ebc104f072d7ee4261870b815e437c99c43104b6ced045a637e1d5e903
anchor-sha256: src/runtime/ownership/xr_ownership_audit.c a87eeb938d90499c9e536e27040c5aa93e1662fc9523e66e147a5621f87a119e
anchor-sha256: tests/unit/runtime/test_ownership_audit.c 0f3b36fd730cc239dce754485a93307e00a39eac536f4daf6b5cf462133c4946
