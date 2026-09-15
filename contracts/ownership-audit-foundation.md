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
   The sanitizer lane also requires every selected source probe to finish with
   a successful exit. Timeout, signal termination, unsupported-source rejection
   and other nonzero exits fail the lane even without a race warning. Failed
   probe output and stable source paths are retained in the lane log; remaining
   probes still execute to collect independent failures.
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

## Verification

verification-test: test_ownership_audit
verification-test: test_typed_lifecycle_audit
verification-test: check_ownership_audit_record_no_alloc
verification-test: check_ownership_audit_release_boundary
verification-test: test_tsan_focused_runner
