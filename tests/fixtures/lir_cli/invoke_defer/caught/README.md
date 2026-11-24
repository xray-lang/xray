# Typed catches after cleanup

Run the product cases with `run_lir_invoke_try_assert.py --fixtures tests/fixtures/lir_cli/invoke_defer/caught`.

- `scope_call` exercises success and business failure at the same call site. Normal scope exit runs one cleanup. Failure runs that cleanup before the typed catch reads the original payload.
- `nested_scope_call` checks the order across two scopes. The normal path resumes inside the outer scope after the inner cleanup. The failure path runs both cleanups before the handler and skips the rest of both scopes.

The `preflight` directory holds two additional Source-checked inputs. `callee_may_panic` has a real dynamic division in the direct callee and passes zero through its ordinary parameter. Its lowering diagnostic needs to be pinned against the final implementation. `scope_throw` uses an explicit local throw to exercise the error edge without an invoke producer. Both inputs are separate from the product cases.

Clean Windows Release `fe9e52416` accepts all four inputs in Source. The VM, C emission and native build report identical active-cleanup lowering refusals. The runtime byte oracles await support for routing a typed error through the cleanup edge.
