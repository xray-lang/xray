# Static cleanup

Each directory is a separate profile for `run_lir_invoke_try_assert.py`. Pass the directory with `--fixtures` and select individual cases with `--case`.

## Synchronous exits and refusals

`sync/eager_immutable` is the smallest executable witness. Its immutable copy retains the earlier value even though the mutable binding changes before return. `sync/normal_return` checks late reads of a mutable binding and LIFO order. `sync/result_snapshot` checks that cleanup can update a binding after the function has evaluated its scalar return value.

`sync/shared_exit` crosses the same two registrations on normal and business-error paths. It distinguishes leaving the inner block from returning from the function. `sync/per_iteration` checks one cleanup per iteration across continue, fallthrough and break.

`lowering/forbidden_active_panic` passes Source checking and then requires the same lowering refusal at the dynamic division in VM, C emission and native build. The commands produce no stdout, C source or executable.

`source/forbidden_escape` pins E0387 for a business error that escapes a cleanup body.

## Business error across an active call

`propagation/business_propagation` checks cleanup in two functions and the original error at the root. The caller invokes the callee while a cleanup is active.

## Panic unwinding

`panic/assertion_panic` requires the callee's cleanup before the business failure becomes E0446 and the caller's cleanup during panic unwinding. A later registration remains unexecuted.

Clean Windows Release `f24409284` stops at `CLEANUP_REGISTER` after Source checking. The synchronous profile and lowering refusal target the subsequent synchronous implementation. The propagation and panic runtime byte oracles cover later execution support.
