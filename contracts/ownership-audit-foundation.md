# Ownership audit manifest and dynamic heap foundation contract

Status: preparatory executed-path oracle with one narrow typed-VM adapter.

This is an independent, bounded runtime evidence kernel. Certificate-to-manifest
generation for arbitrary owners, AOT instrumentation, and full backend cutover
remain later integration gates. The only integrated producer is the exact
managed String-concat cleanup partition described below.

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
   Its CMake source list names only the audit implementation and its typed
   lifecycle adapter, so future
   production ownership sources cannot be silently excluded by a directory
   glob. Only the explicit excluded audit target and focused tests compile them.
   A fail-closed source-graph gate rejects direct or variable-mediated archive
   inclusion, production linkage to the audit target, recursive runtime globs,
   ownership-directory globs, or loss of `EXCLUDE_FROM_ALL`.
9. The typed lifecycle adapter accepts only an intact verified SemanticPlan,
   its ownership certificate, and an intact TargetPlan with the exact native
   hosted profile and the existing one-root/one-owned-dynamic-slot/two-release
   String-concat partition. It independently reconstructs the canonical owner
   and release-event keys and IDs from producer, value, operation, block,
   successor, kind, program point, and event ordinal. Slot identity remains a
   carrier fact and is never reported as owner identity. The adapter registers
   the canonical String layout, extent, domain, and destructor, maps normal and
   terminal cleanup observations to the same certificate release obligation,
   and optionally checks local physical RC before/after. Each successful
   observation uses the configured invocation plus a new activation epoch.
   Plan, certificate, slot, operation, exit, or RC mutation poisons the adapter
   before an oracle event can be committed.
10. The public report scope is exactly "executed-path dynamic evidence, not
   formal proof". Optimized paths still require proof mapping or an audit lane
   that preserves logical observations. Evaluated-extent identity, sized-free
   bytes, provider callback identity, arbitrary certificate adaptation, and
   other backend hooks remain later gates.

## Digest anchors

anchor-sha256: CMakeLists.txt 1e6e764518dfbdfd69b2c3a30615f3523e67dc6b5abd73020f20f03375b6d24e
anchor-sha256: src/shared/xr_ownership_event.h 4ee731782643616d5df34ead901ae39cd995fc91113774eabbe8dd95f982d90d
anchor-sha256: src/plan/ownership/xr_ownership_certificate.h 33de50d0b6bb3a654628ffee0890fa80476e9d85234d8e751bd952e06ed08d07
anchor-sha256: src/runtime/ownership/xr_ownership_audit.h 524251f129b91b7f6de71081b9514528748ebab1264e25803b547d12a1c39309
anchor-sha256: src/runtime/ownership/xr_ownership_audit.c 89516b97cfd7a9109acc8c429e486d3743158abb1e718948a1011134076685f5
anchor-sha256: src/vm/audit/xr_typed_lifecycle_audit.h 85dab081a8f11822651cc686648d97ca228cdb97a2c406fc3aca9fce70d1fbdb
anchor-sha256: src/vm/audit/xr_typed_lifecycle_audit.c 291e460b9004adfe43491e1f1c0054bac1cf2af8e137917ebb4e2760868573c3
anchor-sha256: scripts/check_ownership_audit_record_no_alloc.py 00f71577e9278988a69467ffb3ef1078618cb3658ba544568742ffc9c1581f63
anchor-sha256: scripts/check_ownership_audit_release_boundary.py b89a7249c71941a6a7ca55331f75507cba96ea7d3c55d1f7dafe062b968804fb
anchor-sha256: scripts/run_tsan_focused.py f431ef1a596ca0c2708c2ed8317333b817003b18b94e4e48233ea971b3f7b098
anchor-sha256: tests/unit/CMakeLists.txt 0acb4b9b83542f9b904b32a8a1a52ee1635e9585f4137e0a65b03f8b12eaa789
anchor-sha256: tests/unit/runtime/test_ownership_audit.c 98c718b64f6b840bee6172c07e0a931178ca4dd237e5bb1a88ff9f03a130f59e
anchor-sha256: tests/unit/runtime/test_typed_lifecycle_audit.c 25ea03d2d62ab5086f2f9634bf1a819d3eec577a60f327d26df60c671fbe8ce5
