# Runtime module generation lifecycle contract

This contract freezes the runtime-owned authority for loaded TargetPlan
generations, one deliberately narrow sole-function scalar executor, and the
canonical runtime-only entry cell that can publish an exact scalar VM or native
entry. It does not claim that dynamic calls, escaping closures, a root scanner,
hosted/FFI execution, or general typed adapters are installed. Its sole-function
scalar route remains the only governed product activation path.

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
   TargetPlan schema 34 preserves exact SOURCE-import storage and dense
   SOURCE_EXPORT argument rows together with exact ADT-enum, aggregate
   field-name, Array-intrinsic, String-runes result, and sealed Iterator-rune
   `hasNext`/`next`/`rune.toUInt32`/`rune.isWhitespace` call authority, plus exact direct-local
   borrowed `Array` parameter storage and exact String range-slice call authority.
   Those dynamic rows remain
   storage/call authority and
   therefore continue to make this sole-scalar PREPARE route fail closed.
   Its sealed StringBuilder constructor call likewise remains non-executable
   at this scalar-only runtime boundary.
   PREPARE performs the complete plan and instruction verification before it
   constructs and publishes one immutable decoded cache. READY is unreachable
   if cache allocation or any hard function/row/block/byte budget fails; a
   failed attempt leaves the generation VERIFIED and publishes nothing.
6. A healthy ACTIVE eligible generation alone may execute its sole function.
   Execution first acquires an `INFLIGHT_CALL` pin, binds the retained plan's
   exact generation identity fingerprint, requires the generation's exact
   decoded-cache plan/schema/fingerprint binding, and then uses only its
   verified instruction facts and typed frame through an explicitly selected
   generated function-table provider; there is no implicit default or fallback
   provider. Every success and failure path
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
   production PREPARE helper and requires an exact published decoded cache.
   A pre-PREPARE VERIFIED generation may have no cache; once published, the
   cache remains generation-owned through drain and retirement, while the
   in-flight pin prevents unload during reuse. UNLOAD destroys the cache before
   releasing the retained plan. Its transition model rejects identity mutation,
   skipped/repeated states, mismatched pins, illegal rollback, and revision
   mutation.
8. The standalone public header and lifecycle/scalar-execution symbols, plus the
   internal production entry-cell implementation, are shipped by the Core
   component and link from `xray_vm` without compiler builders, encoders,
   analyzer objects, or Xi implementation symbols. Installed runtime code can
   derive authority from exact XSM and materialize a matching XTP, so its archive
   gate owns the XSM/XTP-to-sole-scalar success route. This remains a runtime
   boundary only: it makes no source/CLI or product end-to-end claim.
9. The artifact runtime facade adds no independent execution, verification, or
   naming authority. A runtime owns exactly one generation authority and refuses
   to be destroyed while it holds a loaded module. A module load requires both
   exact artifact images: the semantic image is the authority the target image
   must bind, neither is inferred from the other, and success means the
   generation reached ACTIVE under the same PREPARE gate above. A load that
   cannot reach ACTIVE unwinds through rollback, retirement, and unload, releases
   both artifacts, and returns no handle, so no module ever denotes a generation
   that cannot run. `XrExport` contains only one entry cell and its exact
   expectation; there is no parallel module/function handle or second pin path.
10. Export names are a semantic-artifact fact. The TargetPlan carries dense
    numeric tables and no spelling, so lookup reads the verified source export
    table the plan retains and matches an exact name in it. It never resolves an
    unpublished internal function name, and it independently requires a healthy
    ACTIVE generation, a plan-bounded function index, and the exact installed
    scalar i64 execution family. A resolved handle is a module-owned loan that
    the module invalidates at unload; the caller never frees it.
11. Publishing a source export requires source-import shared storage, and
    clause 5 admits only a sole scalar i64 function with no storage authority,
    so the two are mutually exclusive at this boundary: a module this runtime
    can load publishes no export, and a module that publishes one cannot load.
    Lookup and call are therefore written against the real verified tables and
    canonical entry cell, and fail closed for that structural reason rather than
    being stubbed. There is no name guess, no implicit entry, and no result
    reported that was not executed.
12. An entry ABI fingerprint is a callee identity, separate from
    `XrTargetCallRecord.fingerprint`. The target call fingerprint intentionally
    includes the semantic operation, caller function, caller/result slots, and
    call-argument rows, so reusing it would make one callee acquire a different
    identity at every call site. `XR_ENTRY_ABI_SCHEMA_VERSION == 1` instead hashes
    the exact signed-i64 parameter/result shape, native ABI, target data-layout
    hash, and target-profile fingerprint under an entry-specific domain. This
    record is runtime-only and is not serialized into TargetPlan schema 34 or
    XTP schema 34. Persisting a dynamic-call expectation later requires an atomic
    schema cutover; no compatibility interpretation is permitted.
13. Binding requires an immutable verified TargetPlan with an intact plan
    fingerprint, an exact `SCALAR_I64_CLOSED` function, and zero adapter rows.
    The only admitted adapter is an identity adapter whose fingerprint is
    derived from the entry ABI. The expectation binds ABI, adapter, plan,
    generation, binding, and executor identities; any mismatch fails closed
    before execution. This does not claim dynamic, hosted, FFI, aggregate,
    ownership-bearing, suspending, or non-identity-adapter calls.
14. An entry cell stores either a verified VM function index or a typed native
    i64 entry. A native code pointer may enter only through process-local runtime
    registration. It has no artifact representation and is never hashed; a
    nonzero stable native-entry identity participates in the binding fingerprint
    instead. Artifact-driven export resolution can register only the VM executor
    and therefore cannot inject a raw code pointer.
15. A published binding owns one `STATIC_ROOT` generation pin. Acquisition
    compares the complete expectation while the cell gate is held, obtains one
    `INFLIGHT_CALL` pin, rechecks the exact generation/plan identity, and only
    then snapshots executable authority into the call token. Swap publishes the
    new binding while preserving any old in-flight token; clear removes the
    binding and releases its static-root pin. Success, VM/native error, and
    cancellation all release the token, and an atomic token state permits
    exactly one release: a duplicate or never-acquired release fails without a
    second decrement. DRAINING rejects new acquisition, while an already acquired
    token can exit and unblock retirement. Stale expectations and wrong ABI,
    adapter, plan, generation, binding, or executor identities fail closed
    without acquiring a pin.

anchor-sha256: include/xray_runtime_generation.h b8d8ab25bf7945cb6837af74a2460ff52d516714b47c3331f6ce82fbc33c05d0
anchor-sha256: src/runtime/xr_module_generation_internal.h a2f5788ce7b589e4d5d2a966e67e6f9b326016c045a4d9ec7d8ce9cf3fdf5580
anchor-sha256: src/runtime/xr_module_generation.c 3085a606baa8a7c5779a8abf83dc2d16ee758a872b1b5ca45c0b04e4cd808788
anchor-sha256: src/runtime/xr_module_generation_verify.c a5d61df40d4442e6ccf8c9824bf73d7a8b3dc751b90b46ec4baf7039135feb25
anchor-sha256: src/vm/xr_typed_dispatch.h 10c108b77e3beff1dfd6c04137ce684a4c8c1d08b3af3a4402dae1443fcff768
anchor-sha256: src/vm/xr_typed_dispatch.c d322275c81f8e833f2f9a850ca8163384e42bb9d703d8a40c16ccb7d711aa9ac
anchor-sha256: src/vm/xr_vm_decoded_cache.h b8dd666865e181f77203aff6b65217f3d1b5d3b413419c831d896a2e31902e23
anchor-sha256: src/vm/xr_vm_decoded_cache.c 2d4f14d54740e6aac0cdfb23fc7b90b075725c479751ba52a1948a658f91fa77
anchor-sha256: src/vm/xr_typed_frame.h 2a66ff7a7601917033f717c794a4c3234534cfe146bd2c9b8067eb3f15b4b48c
anchor-sha256: src/vm/xr_typed_frame.c 5e5d7615d8dfe580ac8104f6f391219b876f2789335cfa41ea9e122abf45adb1
anchor-sha256: contracts/target-machine/diagnostic-codes.toml f4cea43f422ccd0a5e336922eca0965d234f40bb935aef6360bc5418ac51da9a
anchor-sha256: tests/unit/runtime/test_runtime_generation.c 971cf9989386d2c660c828f8e36df295d1ee61fa1458d12b03700e1ecb88f246
anchor-sha256: tests/unit/runtime/test_runtime_generation_archive.c 6824d75bad49bbd7dce591994ab2368ec58537cd1f82ebfa654445bda828b41b
anchor-sha256: scripts/target_machine_retired_runtime_symbols.py 3db52d4670d4d76a640d91709f5a6fdd091511ac421ca6326c34ed3b8739d4f7
anchor-sha256: tests/install/run_installed_runtime_symbol_tests.py d68f03a9e2da14514ede7254e10a55b4655c2b250e4a815201bed9480928f4da
anchor-sha256: tests/install/run_install_public_surface_tests.py 1bbef0d66f5d0d78dbe41848497b678c02c88fc9663f296d9863571fe20eb38e
anchor-sha256: include/xray_runtime_api.h c57754f0204441d3575c6c5b3891333bd01eb72b858cb1b17389f5e701866a98
anchor-sha256: src/runtime/xr_runtime_api.c 9de680b6941442442c0a67ff66e14858c38375590bea625556da4a912f9c787e
anchor-sha256: tests/unit/runtime/test_runtime_api_archive.c 225e5777c21a94fcbff21619eef26956c7fad284370c7cbb7091eacebf9817c8
anchor-sha256: src/runtime/xr_entry_cell.h 9e5012d17116a09ba81fccce7c74c380f4f74726026001f88010406589a19b7d
anchor-sha256: src/runtime/xr_entry_cell.c fa1983d0fbd8c20b7daf1ce17fcfa8f25e2b0ada0b35fc613acd5789b86b5514
anchor-sha256: tests/unit/runtime/test_entry_cell_runtime_archive.c 34bc22820144f368a5e5914ac387f2a19db85ef4555d458b07215548eef1dca0
anchor-sha256: tests/unit/vm/test_vm_decoded_cache.c 5266ff18ca9b135f0b16280c7b4ab4644c96b3b4d9da7e5f10e42b9dbcd01cbf
