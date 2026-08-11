# Ownership audit manifest and dynamic heap foundation contract

Status: preparatory executed-path oracle frozen by task 274.

This is an independent, bounded runtime evidence kernel. Certificate-to-manifest
generation, real VM/AOT instrumentation, and full task 274 cutover remain later
integration gates; none is implied by this leaf module.

1. Static owner, ownership-transition, and lifecycle manifests are registered
   before execution. Observed events cannot create their own expectations.
   Every ownership transition is named by a stable certificate event ID and
   exactly binds owner, operation, exit/edge, kind, program point,
   state-before mask, state-after, and logical balance delta.
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
   and observed physical RC before/after telemetry are independently checked.
   Physical RC mode is a manifest obligation and cannot be selected by the
   observed event; mode changes and self-reported sticky counts fail closed.
5. Creation preallocates every table. Recording performs no allocation, growth,
   callback, destructor, or provider call. Capacity exhaustion and concurrent
   or reentrant entry poison the audit with a sticky first error. A static
   local-call-graph gate walks both record entry points and rejects allocator
   calls or new unreviewed external calls. A two-thread test handshake forces
   real overlapping entry without scheduler timing assumptions, and the TSan
   lane builds and runs that audit test on supported hosts. Allocation counters
   and repeated focused tests provide additional dynamic evidence. An event is
   validated in temporary state before its object/loan transition is committed.
6. Tracked allocations follow LIVE -> FINALIZING -> FINALIZED -> RECLAIMED.
   The destructor terminal is legal only in FINALIZING. Exact domain instances
   follow DRAINING -> ENDED; drain rejects resurrection, new borrow, positive
   logical delta, and domain transfer while allowing nonpositive cleanup such
   as cancel. Domain teardown and generation drain are independent.
7. Ownership and lifecycle diagnostic histories are separate bounded arrays.
   Their interleaving is verified online by the shared state machine; the two
   arrays alone are not a unified offline trace and expose no common sequence.
8. The audit target is excluded from release core, coroutine, and VM archives.
   Its CMake source list names only the audit implementation, so future
   production ownership sources cannot be silently excluded by a directory
   glob. Only the explicit excluded audit target and focused test compile it.
9. The public report scope is exactly "executed-path dynamic evidence, not
   formal proof". Optimized paths still require proof mapping or an audit lane
   that preserves logical observations. Evaluated-extent identity, sized-free
   bytes, provider callback identity, certificate adaptation, and real backend
   hooks remain later gates.

## Digest anchors

anchor-sha256: CMakeLists.txt e2cfddeb145978cd712ef07a661c98be2d920466d4014fbcafd26359f0802e7c
anchor-sha256: src/shared/xr_ownership_event.h 4ee731782643616d5df34ead901ae39cd995fc91113774eabbe8dd95f982d90d
anchor-sha256: src/plan/ownership/xr_ownership_certificate.h 33de50d0b6bb3a654628ffee0890fa80476e9d85234d8e751bd952e06ed08d07
anchor-sha256: src/runtime/ownership/xr_ownership_audit.h 524251f129b91b7f6de71081b9514528748ebab1264e25803b547d12a1c39309
anchor-sha256: src/runtime/ownership/xr_ownership_audit.c 89516b97cfd7a9109acc8c429e486d3743158abb1e718948a1011134076685f5
anchor-sha256: scripts/check_ownership_audit_record_no_alloc.py 00f71577e9278988a69467ffb3ef1078618cb3658ba544568742ffc9c1581f63
anchor-sha256: scripts/run_tsan_focused.py 9a59c3b7e5882551d947bb745a2bc06acf982f7d5d5cbf682dea5f08b33d2802
anchor-sha256: tests/unit/CMakeLists.txt 6fd6be77af2a7a6bbf7a18eeed0a15e8e09e67fe49b0a0d1c3bb144465724570
anchor-sha256: tests/unit/runtime/test_ownership_audit.c 98c718b64f6b840bee6172c07e0a931178ca4dd237e5bb1a88ff9f03a130f59e
