# Runtime module generation lifecycle contract

This contract freezes the runtime-owned authority for loaded TargetPlan
generations and one deliberately narrow sole-function scalar executor. It does
not claim that an export resolver, call engine, root scanner, general typed
executor, or product activation route is installed.

1. A generation authority requires explicit, nonzero hard limits for loaded
   generations, total pins, per-generation pins, and every pin kind. The same
   runtime mutex serializes generation allocation, pin acquisition/release,
   retirement, unload, and authority destruction, so concurrent attempts
   cannot oversubscribe a checked budget.
2. Loading accepts only an independently verified immutable TargetPlan. The
   runtime assigns a monotonic nonzero generation number and derives a stable
   SHA-256 generation identity from the plan schema, completed family mask,
   exact capability mask, semantic/profile/plan fingerprints, and the native
   runtime/provider/object-header fingerprints. No pointer, path, host name,
   legacy bytecode, or caller-authored machine fact participates in authority.
3. The normal state order is exactly `LOADING -> VERIFIED -> READY -> ACTIVE ->
   DRAINING -> RETIRED -> UNLOADED`. A healthy ACTIVE generation alone may
   acquire pins. ACTIVE and DRAINING may release matched pins. Retirement and
   unload require every generic, in-flight call, callback, destructor, and
   static-root pin to be zero.
4. Poisoning requires a nonzero stable diagnostic fingerprint and cannot be
   overwritten by a conflicting fingerprint. Rollback is an explicit failure
   branch: a pre-activation generation becomes RETIRED with zero pins, ACTIVE
   becomes DRAINING, and DRAINING remains drainable. A poisoned or rolling-back
   generation cannot become READY or ACTIVE.
5. `xr_runtime_generation_activation_available()` means only that the bounded
   sole-function scalar executor is installed. PREPARE independently requires
   exactly one canonical function 0, a nonempty complete
   `SCALAR_I64_STRAIGHT_LINE` instruction group, the exact typed-frame schema
   and family mask, and no storage, allocation, extent-operand, call,
   call-argument, root-map, root-slot, cleanup, adapter, or coroutine execution
   authority. Any other verified plan remains VERIFIED and fails PREPARE with
   `XR_EXEC_5004`; it is not partially activated.
6. A healthy ACTIVE eligible generation alone may execute its sole function.
   Execution first acquires an `INFLIGHT_CALL` pin, binds the retained plan's
   exact generation identity fingerprint, and then uses only the verified
   scalar instruction table and typed frame. Every success and failure path
   releases the pin. DRAINING rejects new execution while permitting an
   already-started call to release its pin, and retirement still requires all
   pins to reach zero. There is no compatibility VM, XRC translation, generic
   tagged-frame execution, AOT/CGen fallback, or guessed export entry.
7. The independent verifier re-derives immutable plan/native identity, stable
   generation fingerprint, state/poison invariants, counter sums, per-kind and
   global budgets. For every READY, ACTIVE, or DRAINING generation it separately
   reconstructs the same plan-specific scalar eligibility without calling the
   production PREPARE helper. Its transition model rejects identity mutation,
   skipped/repeated states, mismatched pins, illegal rollback, and revision
   mutation.
8. The standalone public header and lifecycle/scalar-execution symbols are
   shipped by the Core component and link from `xray_vm_runtime` without
   compiler builders, encoders, analyzer objects, or Xi implementation symbols.
   The installed artifact-authority loader remains unavailable, so this is not
   an installed XTP-to-execution, CLI, export-publishing, or product end-to-end
   claim.

anchor-sha256: include/xray_runtime_generation.h b8d8ab25bf7945cb6837af74a2460ff52d516714b47c3331f6ce82fbc33c05d0
anchor-sha256: src/runtime/xr_module_generation_internal.h 427d2d23bfa8991dd8d20463169fb9bb23d7486a38d5274cb5d84b089a14a96a
anchor-sha256: src/runtime/xr_module_generation.c d02e74f29b281d354ea03b339205e457e62266c06bd4b1afb6692d90a2c0e1d7
anchor-sha256: src/runtime/xr_module_generation_verify.c 4028ff528eeba7bb8db3c902901a36f45c085f5e2ecf54245bcc6a96de58e12d
anchor-sha256: src/vm/xr_typed_dispatch.h f72964091ac427130a3ff00c6d051cf85a3edd6ae174846984e0c1d506adeecd
anchor-sha256: src/vm/xr_typed_dispatch.c 3fd358dc6b4aaa5ce0ff2f039a1b62e0420bede465473c8cfc2da51980e94945
anchor-sha256: src/vm/xr_typed_frame.h 2cd4328e776257047b496a8a49eaf8464d2d3ea527bb8fdfb0a5784c92c9f4ed
anchor-sha256: src/vm/xr_typed_frame.c f0a3c7ea24cc7b712ac8de2923e92ac8bbb5ddc85006878b147ab9d506fd6ac6
anchor-sha256: contracts/target-machine/diagnostic-codes.toml 54ee3affe064aabfb89fe1cc75ef5642837fd14b6e7e575a0badb4fc670efd74
anchor-sha256: tests/unit/runtime/test_runtime_generation.c 993a338ba5dd2f0ed7a88f4aa830e697700361d88acd2b6ef36f35bcafc270a7
anchor-sha256: tests/unit/runtime/test_runtime_generation_archive.c 6824d75bad49bbd7dce591994ab2368ec58537cd1f82ebfa654445bda828b41b
anchor-sha256: tests/install/run_installed_runtime_symbol_tests.py f81c404c1128f9ff470974956ab84898ad63df5b9a52e476f59111551adce476
anchor-sha256: tests/install/run_install_public_surface_tests.py 9ef54ccfd07d2bf8c40d2ca5065248e7667899a291a3e9cbaba74aafd067ef47
