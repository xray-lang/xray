# Static cleanup

Each directory is a separate profile for `run_lir_invoke_try_assert.py`. Pass the directory with `--fixtures` and select individual cases with `--case`.

## Synchronous exits

`sync/eager_immutable` is the smallest executable witness. Its immutable copy retains the earlier value even though the mutable binding changes before return. `sync/normal_return` checks late reads of a mutable binding and LIFO order.

`sync/shared_exit` crosses the same two registrations on normal and business-error paths. It distinguishes leaving the inner block from returning from the function. `sync/business_propagation` checks cleanup in two functions and the original error at the root. `sync/per_iteration` checks one cleanup per iteration across continue, fallthrough and break.

## Panic unwinding

`panic/assertion_panic` requires the callee's cleanup before the business failure becomes E0446 and the caller's cleanup during panic unwinding. A later registration remains unexecuted.

## Source rejection

`source/forbidden_escape` pins E0387 for a business error that escapes a cleanup body.

Clean Windows Release `f24409284` accepts the six executable cases in Source and stops in VM, C emission and native build at `CLEANUP_REGISTER`. All four entry commands preserve E0387 on the Source rejection. The runtime byte oracles in the synchronous and panic profiles await execution support.
