# Runtime module generation lifecycle contract

This contract freezes the runtime-owned authority for loaded TargetPlan
generations and one deliberately narrow sole-function scalar executor. It does
not claim that an export resolver, call engine, root scanner, or general typed
executor is installed. Its sole-function scalar route is the only governed
product activation path.

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
   TargetPlan schema 19 preserves SOURCE-namespace storage in the exact required
   family mask. Those borrowed dynamic rows remain storage/call authority and
   therefore continue to make this sole-scalar PREPARE route fail closed.
   Its sealed StringBuilder constructor call likewise remains non-executable
   at this scalar-only runtime boundary.
6. A healthy ACTIVE eligible generation alone may execute its sole function.
   Execution first acquires an `INFLIGHT_CALL` pin, binds the retained plan's
   exact generation identity fingerprint, and then uses only the verified
   scalar instruction table and typed frame. Every success and failure path
   releases the pin. DRAINING rejects new execution while permitting an
   already-started call to release its pin, and retirement still requires all
   pins to reach zero. This route passes no arguments, so a sole function whose
   verified rows declare parameters fails closed with `XR_EXEC_5004` rather
   than executing against implicit zeros. A verified, eligible program that
   divides by zero is a program fault, not an authority failure: it reports
   `XR_EXEC_5009`, yields no result, releases its pin, and leaves the
   generation ACTIVE, so it can never be read as a verification or identity
   problem. There is no compatibility VM, XRC
   translation, generic tagged-frame execution, AOT/CGen fallback, or guessed
   export entry.
7. The independent verifier re-derives immutable plan/native identity, stable
   generation fingerprint, state/poison invariants, counter sums, per-kind and
   global budgets. For every READY, ACTIVE, or DRAINING generation it separately
   reconstructs the same plan-specific scalar eligibility without calling the
   production PREPARE helper. Its transition model rejects identity mutation,
   skipped/repeated states, mismatched pins, illegal rollback, and revision
   mutation.
8. The standalone public header and lifecycle/scalar-execution symbols are
   shipped by the Core component and link from `xray_vm` without
   compiler builders, encoders, analyzer objects, or Xi implementation symbols.
   Installed runtime code can derive authority from exact XSM and materialize a
   matching XTP, so its archive gate owns the XSM/XTP-to-sole-scalar success
   route. It still has no general resolver, export publication, calls, roots,
   source/CLI entry, or product end-to-end claim.

anchor-sha256: include/xray_runtime_generation.h b8d8ab25bf7945cb6837af74a2460ff52d516714b47c3331f6ce82fbc33c05d0
anchor-sha256: src/runtime/xr_module_generation_internal.h 427d2d23bfa8991dd8d20463169fb9bb23d7486a38d5274cb5d84b089a14a96a
anchor-sha256: src/runtime/xr_module_generation.c 6bf5ac5976ed0d6784d2c28531608299d500a521dd433877403fa23d80f75ec4
anchor-sha256: src/runtime/xr_module_generation_verify.c 4028ff528eeba7bb8db3c902901a36f45c085f5e2ecf54245bcc6a96de58e12d
anchor-sha256: src/vm/xr_typed_dispatch.h 429949f7960b431fcbb0b14cd10ca5e720ef4deeb1f0d54d725bb435091c1c42
anchor-sha256: src/vm/xr_typed_dispatch.c 13e610cad27b9be04aae0dede6624f59545e1931317c555457ca32a1ea691019
anchor-sha256: src/vm/xr_typed_frame.h 19d1f77e23efdec170fb60c617a496be341c9579deaa4953a17e669c6f59fb4a
anchor-sha256: src/vm/xr_typed_frame.c f0a3c7ea24cc7b712ac8de2923e92ac8bbb5ddc85006878b147ab9d506fd6ac6
anchor-sha256: contracts/target-machine/diagnostic-codes.toml f4cea43f422ccd0a5e336922eca0965d234f40bb935aef6360bc5418ac51da9a
anchor-sha256: tests/unit/runtime/test_runtime_generation.c 6a877b4078e8b4bc9d1fcd3e432df7b5df4af6053ba8b6b11700686b37f4b2d5
anchor-sha256: tests/unit/runtime/test_runtime_generation_archive.c 6824d75bad49bbd7dce591994ab2368ec58537cd1f82ebfa654445bda828b41b
anchor-sha256: scripts/target_machine_retired_runtime_symbols.py 3db52d4670d4d76a640d91709f5a6fdd091511ac421ca6326c34ed3b8739d4f7
anchor-sha256: tests/install/run_installed_runtime_symbol_tests.py 42b13fdd86da1191714c9f5d2c4981bd8f40a338c9e0a5aa7d0276843f2fc867
anchor-sha256: tests/install/run_install_public_surface_tests.py 031ddc106e70ab5c0936793dc84985dbb22176c2989e19bcf7ed28dc625c8b60
