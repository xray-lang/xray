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
   `SCALAR_I64_CLOSED` instruction group, the exact typed-frame schema
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
   than executing against implicit zeros. A sole function whose verified rows
   loop without reaching a return is stopped by the executor's step budget and
   reported as a program fault, so it can neither hang the runtime nor be read
   as a verification failure. A verified, eligible program that
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
9. The artifact runtime facade adds no execution, verification, or naming
   authority. A runtime owns exactly one generation authority and refuses to be
   destroyed while it holds a loaded module. A module load requires both exact
   artifact images: the semantic image is the authority the target image must
   bind, neither is inferred from the other, and success means the generation
   reached ACTIVE under the same PREPARE gate above. A load that cannot reach
   ACTIVE unwinds through rollback, retirement, and unload, releases both
   artifacts, and returns no handle, so no module ever denotes a generation that
   cannot run. Unload drives DRAINING, RETIRED, and UNLOADED through the same
   gates, so an in-flight pin refuses it rather than being torn out.
10. Export names are a semantic-artifact fact. The TargetPlan carries dense
    numeric tables and no spelling, so lookup reads the verified source export
    table the plan retains and matches an exact name in it. It never resolves an
    unpublished internal function name, and it independently requires a healthy
    ACTIVE generation, a plan-bounded function index, and the exact installed
    scalar i64 execution family. A resolved handle is a module-owned loan that
    the module invalidates at unload; the caller never frees it.
11. Publishing a source export requires source-namespace shared storage, and
    clause 5 admits only a sole scalar i64 function with no storage authority,
    so the two are mutually exclusive at this boundary: a module this runtime
    can load publishes no export, and a module that publishes one cannot load.
    Lookup and call are therefore written against the real verified tables and
    fail closed for that structural reason rather than being stubbed. Calling
    binds the generation's exact plan fingerprint, holds an `INFLIGHT_CALL` pin
    for the whole call, releases it on every path, and refuses any argument
    count or value kind the verified rows do not declare. There is no name
    guess, no implicit entry, and no result reported that was not executed.

anchor-sha256: include/xray_runtime_generation.h b8d8ab25bf7945cb6837af74a2460ff52d516714b47c3331f6ce82fbc33c05d0
anchor-sha256: src/runtime/xr_module_generation_internal.h 427d2d23bfa8991dd8d20463169fb9bb23d7486a38d5274cb5d84b089a14a96a
anchor-sha256: src/runtime/xr_module_generation.c f3fe95413105fbb79fb40b5a0a6f718179b997ad4b823a90baae94a045ba103a
anchor-sha256: src/runtime/xr_module_generation_verify.c 0f146f9f8526f83d84157febadde7cb92327f24186fb6d8d45138968ecaaf4bd
anchor-sha256: src/vm/xr_typed_dispatch.h 30b893c4f791e6b99a87cf46194c982b63972072675d2bfbc329ab55fcba1b25
anchor-sha256: src/vm/xr_typed_dispatch.c b89abe0916835904f3ea7bc7394e7a8eab23d2f2c37bf3a711d44941858e0591
anchor-sha256: src/vm/xr_typed_frame.h b51b7f45110ccd0f05a1b6595a4a960eec1606d2bae6dbd21e95633fed2b0151
anchor-sha256: src/vm/xr_typed_frame.c f0a3c7ea24cc7b712ac8de2923e92ac8bbb5ddc85006878b147ab9d506fd6ac6
anchor-sha256: contracts/target-machine/diagnostic-codes.toml f4cea43f422ccd0a5e336922eca0965d234f40bb935aef6360bc5418ac51da9a
anchor-sha256: tests/unit/runtime/test_runtime_generation.c 42bfb35e761bf2a0d187e35c1cc28a2173caa43e919bd9cb471a4415896edef1
anchor-sha256: tests/unit/runtime/test_runtime_generation_archive.c 6824d75bad49bbd7dce591994ab2368ec58537cd1f82ebfa654445bda828b41b
anchor-sha256: scripts/target_machine_retired_runtime_symbols.py 3db52d4670d4d76a640d91709f5a6fdd091511ac421ca6326c34ed3b8739d4f7
anchor-sha256: tests/install/run_installed_runtime_symbol_tests.py 0ab2232c6731a2366bbd270f838d2a9fd33c1480bfe4d75de8c9ededbadfda51
anchor-sha256: tests/install/run_install_public_surface_tests.py 1bbef0d66f5d0d78dbe41848497b678c02c88fc9663f296d9863571fe20eb38e
anchor-sha256: include/xray_runtime_api.h d9e9f189616f11ca1ed4ffc489fa5ee020a16970e9bc65d3f0218d822347e010
anchor-sha256: src/runtime/xr_runtime_api.c 8695699c4b87403a19e01a11e85545f3f180934589f8cc5f3b633ac60d9b7da9
anchor-sha256: tests/unit/runtime/test_runtime_api_archive.c 225e5777c21a94fcbff21619eef26956c7fad284370c7cbb7091eacebf9817c8
