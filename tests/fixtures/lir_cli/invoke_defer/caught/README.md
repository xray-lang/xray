# Typed catches after cleanup

Run the product cases with `run_lir_invoke_try_assert.py --fixtures tests/fixtures/lir_cli/invoke_defer/caught`.

- `scope_call` exercises success and business failure at the same call site. Normal scope exit runs one cleanup. Failure runs that cleanup before the typed catch reads the original payload.
- `nested_scope_call` checks the order across two scopes. The normal path resumes inside the outer scope after the inner cleanup. The failure path runs both cleanups before the handler and skips the rest of both scopes.
- `scope_throw` constructs the enum at a local throw and reads its payload after cleanup.
- `state_after_error_cleanup` checks the distinct live scalar state for two failure predecessors and the value written by the cleanup through its captured binding.
- `callee_may_panic` checks that the lowering still rejects a call with a real dynamic division under an active cleanup.

Clean Windows Release `fe9e52416` accepted the original scope, nesting, direct throw and division inputs in Source. The VM, C emission and native build then reported active-cleanup lowering refusals at those cases.
