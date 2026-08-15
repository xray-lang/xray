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
   TargetPlan schema 31 preserves exact SOURCE-import storage and dense
   SOURCE_EXPORT argument rows together with exact ADT-enum, aggregate
   field-name, Array-intrinsic, String-runes result, and sealed Iterator-rune
   `hasNext`/`next`/`rune.toUInt32`/`rune.isWhitespace` call authority, plus exact direct-local
   borrowed `Array` parameter storage. Those dynamic rows remain
   storage/call authority and
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
    record is runtime-only and is not serialized into TargetPlan schema 31 or
    XTP schema 29. Persisting a dynamic-call expectation later requires an atomic
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
anchor-sha256: src/runtime/xr_module_generation_internal.h 427d2d23bfa8991dd8d20463169fb9bb23d7486a38d5274cb5d84b089a14a96a
anchor-sha256: src/runtime/xr_module_generation.c f3fe95413105fbb79fb40b5a0a6f718179b997ad4b823a90baae94a045ba103a
anchor-sha256: src/runtime/xr_module_generation_verify.c 0f146f9f8526f83d84157febadde7cb92327f24186fb6d8d45138968ecaaf4bd
anchor-sha256: src/vm/xr_typed_dispatch.h 1def13ea54294774bb120596f2c640498fc3f92e61d95b17f54ae51a8df0ae3d
anchor-sha256: src/vm/xr_typed_dispatch.c ef9ad355401e587b4d27f3189f01c0b0e7779145e0aa1089a70a3dfca9d9147a
anchor-sha256: src/vm/xr_typed_frame.h e6331d14764199f4256d2a11acc64f520454bedb50615c4565bf502cdf789033
anchor-sha256: src/vm/xr_typed_frame.c 898f5a49db5ce3676e8f21a1835812034d05fe646e2b21931ada4b571fb391fc
anchor-sha256: contracts/target-machine/diagnostic-codes.toml f4cea43f422ccd0a5e336922eca0965d234f40bb935aef6360bc5418ac51da9a
anchor-sha256: tests/unit/runtime/test_runtime_generation.c 42bfb35e761bf2a0d187e35c1cc28a2173caa43e919bd9cb471a4415896edef1
anchor-sha256: tests/unit/runtime/test_runtime_generation_archive.c 6824d75bad49bbd7dce591994ab2368ec58537cd1f82ebfa654445bda828b41b
anchor-sha256: scripts/target_machine_retired_runtime_symbols.py 3db52d4670d4d76a640d91709f5a6fdd091511ac421ca6326c34ed3b8739d4f7
anchor-sha256: tests/install/run_installed_runtime_symbol_tests.py adbe96cb24fd2da66f8e8c3148d78eab7f59d3b2544ba07a87f41540b6f760cd
anchor-sha256: tests/install/run_install_public_surface_tests.py 1bbef0d66f5d0d78dbe41848497b678c02c88fc9663f296d9863571fe20eb38e
anchor-sha256: include/xray_runtime_api.h c57754f0204441d3575c6c5b3891333bd01eb72b858cb1b17389f5e701866a98
anchor-sha256: src/runtime/xr_runtime_api.c 9a904890a95976df5082e5e66fe2e5b4cd0758917cfbeb936920c652004267c3
anchor-sha256: tests/unit/runtime/test_runtime_api_archive.c 225e5777c21a94fcbff21619eef26956c7fad284370c7cbb7091eacebf9817c8
anchor-sha256: src/runtime/xr_entry_cell.h 9e5012d17116a09ba81fccce7c74c380f4f74726026001f88010406589a19b7d
anchor-sha256: src/runtime/xr_entry_cell.c 8798082b7c04b33d171be75161f5662bcae485b4138397aac73cbe83ffe24251
anchor-sha256: tests/unit/runtime/test_entry_cell_runtime_archive.c 34bc22820144f368a5e5914ac387f2a19db85ef4555d458b07215548eef1dca0
