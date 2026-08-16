# Runtime module generation lifecycle contract

This contract freezes the runtime-owned authority for loaded TargetPlan
generations, the closed and exact SOURCE_EXPORT signed-`i64` typed executor,
and the canonical runtime-only entry registry/cell/cache authority. It does not
claim that INDIRECT_CALLABLE calls, escaping closures, a root scanner,
hosted/FFI adapters, owned/object values, or suspension are installed. The
sole-function scalar route remains a narrow public convenience, while verified
dynamic functions execute only through the same typed request and pinned entry
authority.

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
   typed scalar executor is installed. PREPARE independently requires at least
   one complete `SCALAR_I64_CLOSED` or `SCALAR_I64_DYNAMIC` instruction group,
   the exact typed-frame schema and family mask, and no allocation,
   extent-operand, root-map, root-slot, cleanup, adapter, or coroutine execution
   authority. Every nonzero family must be one of those two exact families.
   Any other verified plan remains VERIFIED and fails PREPARE with
   `XR_EXEC_5004`; it is not partially activated.
   Scalar dispatch independently repeats the selected function's zero
   root-map, cleanup, and coroutine condition before frame creation, and
   requires zero sparse lifecycle bytes before its status-returning frame
   destruction succeeds. Managed coroutine frames are owned by their explicit
   lifecycle consumer and cannot enter this generation route.
   TargetPlan schema 40 preserves exact SOURCE-import storage and dense
   SOURCE_EXPORT argument rows together with exact ADT-enum, aggregate
   field-name, Array-intrinsic, String-runes result, and sealed Iterator-rune
   `hasNext`/`next`/`rune.toUInt32`/`rune.isWhitespace` call authority, plus exact direct-local
   borrowed `Array` parameter storage and exact String range-slice call authority.
   Only the non-suspending exact signed-`i64`, trivial-ownership,
   identity-adapter SOURCE_EXPORT subset becomes dynamic execution authority;
   all other dynamic rows remain non-executable.
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
   naming authority. A runtime owns exactly one generation authority and one
   explicit, versioned activation configuration. Zero budgets are rejected at
   construction; implicit/default provider callbacks are never installed, and a
   missing plan-required binding is rejected at activation. A module load requires
   both exact artifact images: the semantic image is the authority the target
   image must bind, neither is inferred from the other, and success means the
   generation reached ACTIVE under the same PREPARE gate above and every exact
   source export plus every plan-required provider/finalizer operation was
   published by one bounded activation transaction. A load that cannot reach
   ACTIVE or complete that transaction unwinds through rollback, retirement,
   and unload, releases both artifacts, and returns no handle, so no module ever
   denotes a generation that cannot run or an incomplete publication. `XrExport`
   contains a loan to one entry handle/cell and a loan to the owning
   activation; there is no parallel module/function handle or second execution
   path.
10. Export names are a semantic-artifact fact. The TargetPlan carries dense
    numeric tables and no spelling, so lookup reads the verified source export
    table the plan retains and matches an exact name in it. It never resolves an
    unpublished internal function name, and it independently requires a healthy
    ACTIVE generation, a plan-bounded function index, and the exact installed
    scalar i64 execution family. A resolved handle is a module-owned loan that
    the module invalidates at unload; the caller never frees it.
11. After re-verifying the immutable TargetPlan and instruction program, module
    activation reconstructs its exact native provider requirements from the
    verified capability rows. The hosted foundation allocator's
    `ALLOCATES/RETURNS_OWNED` operation and the hosted panic
    `PANICS/BORROWS/NO_RETURN` operation are provider registrations; the same
    allocator contract's `DEALLOCATES/CONSUMES_OWNED` operation is the module
    activation deallocator-finalizer. This finalizer owns activation-transaction
    storage and is consumed by rollback or unload; it is not an object-layout
    destructor and grants no destructor row authority. Missing or differently
    shaped host bindings fail before any callback. The exact allocator callback
    then creates the private transaction record, and activation binds every
    admitted SOURCE_EXPORT and stages every provider/finalizer registration under
    one bounded registry transaction. Every binding is frozen, every row and
    retained handle is prepared, and active-key conflicts plus entry/provider/
    finalizer budgets are rejected before anything becomes visible. Failure
    exposes no row or provider/finalizer registration, invokes the exact
    deallocator once for any staged record, clears every private entry cell and
    pin, rolls back the generation, and returns no module; success publishes the
    module only after the whole batch commits. Lookup is read-only and repeated
    lookup of the same export returns the same module-owned handle. Export
    execution pins the exact published allocator and panic registrations before
    entering its cell, and unload removes provider/finalizer registrations only
    after the generation is RETIRED with zero pins. Distinct export aliases may
    publish the same exact function handle under separate stable keys. There is
    no name guess, implicit provider, partial registration, or result reported
    that was not executed.
12. An entry ABI fingerprint is a callee identity, separate from
    `XrTargetCallRecord.fingerprint`. The target call fingerprint intentionally
    includes the semantic operation, caller function, caller/result slots, and
    call-argument rows, so reusing it would make one callee acquire a different
    identity at every call site. `XR_ENTRY_ABI_SCHEMA_VERSION == 1` instead hashes
    the exact signed-i64 parameter/result shape, native ABI, target data-layout
    hash, and target-profile fingerprint under an entry-specific domain. This
    TargetPlan schema 40 and XTP schema 40 persist that ABI fingerprint in a
    dedicated entry-expectation row together with the identity-adapter and
    target-profile fingerprints. The artifact never persists an entry cell,
    handle, generation pointer, or code pointer. No earlier schema is accepted.
13. Binding requires an immutable verified TargetPlan with an intact plan
    fingerprint, an exact `SCALAR_I64_CLOSED` function, and zero adapter rows.
    The only admitted adapter is an identity adapter whose fingerprint is
    derived from the entry ABI. The expectation binds ABI, adapter, plan,
    generation, binding, and executor identities; any mismatch fails closed
    before execution. The installed dynamic subset is SOURCE_EXPORT only and
    still does not claim hosted, FFI, aggregate, ownership-bearing, suspending,
    or non-identity-adapter calls.
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
    cancellation all retire the token, and an atomic token state permits
    exactly one pin decrement: a duplicate or never-acquired retirement fails
    without a second decrement. Dynamic context schema 3 has no legacy release
    callback or untyped lease. Every successful dynamic acquisition first
    registers its opaque lease in an authority-owned ledger bounded by the
    authority's total-pin budget. Retirement consumes and clears the resolution
    on every return. An immediate pin-release refusal leaves the token LIVE,
    transfers the holder to the ledger's PENDING state, and poisons the
    generation fail closed; drain retries pending holders deterministically,
    while retirement, unload, and authority destruction refuse any remaining
    holder. Frame disposal and lease retirement are independent exit
    obligations, so one failure cannot skip the other or strand a stack-local
    lease. DRAINING rejects new acquisition, while an already acquired token can
    exit and unblock retirement. Stale expectations and wrong ABI, adapter,
    plan, generation, binding, or executor identities fail closed without
    acquiring a pin.

16. The registry owns stable per-key rows until authority teardown. Each row
    has an atomic epoch and an optional retained handle. Single-entry publish
    replacement, activation-batch publish, and unpublish are serialized through
    a mutation gate and validate each handle's frozen plan/function/generation
    snapshot before touching a row. Activation publication rejects every active
    duplicate and changes entry rows, module/provider/finalizer counts, the
    activation list, and the registry epoch only after all row capacity and
    handle retains succeed. Alias rows keep a shared handle bound until its final
    published key is removed. Removal first clears the cell's static binding,
    while every already-acquired in-flight or provider token retains its own
    generation pin, and only then releases row ownership; a sole registry or
    cache owner is never orphaned. Publication freezes and retains every handle
    and invokes the allocator before acquiring the registry-mutation then
    generation-authority gates. Unpublish likewise retains its guard first and
    drops the authority gate before clearing the entry cell. Activation removal
    detaches counters and ownership under the same two gates, then invokes the
    deallocator only after both gates are released. Thus neither callback nor
    handle API creates an inverse edge against the `handle -> cell -> authority`
    binding order through publication, rollback, or unload.
17. Every dynamic caller generation owns one bounded cache slot per serialized
    expectation. Warm cache lookup uses only the slot mutex and the row epoch
    acquire load; it neither locks nor scans the generation authority, and
    unrelated keys cannot invalidate it. The subsequent entry acquisition owns
    the exact generation pin independently. A miss scans the registry once, retains a handle,
    validates ABI/adapter/plan/generation authority, acquires an in-flight pin,
    and independently re-verifies the callee instruction program. Failures are
    never cached. Cache teardown happens outside the authority lock, and unload
    rechecks RETIRED/zero-pin state before committing, preserving lock order and
    deterministic ownership on every failure.

anchor-sha256: include/xray_runtime_generation.h b8d8ab25bf7945cb6837af74a2460ff52d516714b47c3331f6ce82fbc33c05d0
anchor-sha256: src/runtime/xr_module_generation_internal.h 892b04cdd946296e3812a68147bc68e541848f22086c460494fc699f1871b5d9
anchor-sha256: src/runtime/xr_module_generation.c a07ba16736dd26135e4256d01092ad7f1be71e29787ef68504cd3f61eb979305
anchor-sha256: src/runtime/xr_module_generation_verify.c d954584f4235e75773790b7a0415c6613ba81e09ed7507338d0021893f7b722a
anchor-sha256: src/vm/xr_typed_dispatch.h 40941e5ddffe1544da8a681e1e916dd1c85c3a856a80bc507d34e2f830578b10
anchor-sha256: src/vm/xr_typed_dispatch.c f813ca760ac0be178e9031118b5d144b52f4e6613f5d8bf5f0374eb1bd46b4d0
anchor-sha256: src/vm/xr_vm_decoded_cache.h b8dd666865e181f77203aff6b65217f3d1b5d3b413419c831d896a2e31902e23
anchor-sha256: src/vm/xr_vm_decoded_cache.c 216b764f20711e5612c653d11b25651aedd9afae20ec066e588cc69e60f05c21
anchor-sha256: src/vm/xr_typed_frame.h fa468887f8d87a5ff1fdf6c82b863cb7a62a204b4a30542f9fd0c62693486196
anchor-sha256: src/vm/xr_typed_frame.c 397f46fa8647614c06745dc365676989abdc9e079f25a89994b96d9d34fbd405
anchor-sha256: scripts/check_coroutine_lifecycle_projection.py 74fdc88cea8045a258dae39f2194839ec54f3a2b8759fa56f1b537226fdbc1a2
anchor-sha256: contracts/target-machine/diagnostic-codes.toml f4cea43f422ccd0a5e336922eca0965d234f40bb935aef6360bc5418ac51da9a
anchor-sha256: tests/unit/runtime/test_runtime_generation.c 98a80d0e5d24ffafaca415fd5c07abde8f560a239e76b6b2d321b629e55fd355
anchor-sha256: tests/unit/runtime/test_runtime_generation_archive.c 6824d75bad49bbd7dce591994ab2368ec58537cd1f82ebfa654445bda828b41b
anchor-sha256: scripts/target_machine_retired_runtime_symbols.py 3db52d4670d4d76a640d91709f5a6fdd091511ac421ca6326c34ed3b8739d4f7
anchor-sha256: tests/install/run_installed_runtime_symbol_tests.py 2f3570bbfb67a0a73e7d0b2aea41ef422fbf096eac0cc42b1821993d5da4a19b
anchor-sha256: tests/install/run_install_public_surface_tests.py 1bbef0d66f5d0d78dbe41848497b678c02c88fc9663f296d9863571fe20eb38e
anchor-sha256: include/xray_runtime_api.h b0cff9caa4f70925bba21173b9991a8bca9837f563cd64cfb8dd91206c66b319
anchor-sha256: src/runtime/xr_runtime_api.c 268bae675264a6b5892dea75ccae248907b6ce97eccf473f8190fac13eca9557
anchor-sha256: tests/unit/runtime/test_runtime_api_archive.c 36e110b8ef201e1684b5aa3830fa12664381e15e39d4166d6dd06ace0c837255
anchor-sha256: src/runtime/xr_entry_cell.h 9e5012d17116a09ba81fccce7c74c380f4f74726026001f88010406589a19b7d
anchor-sha256: src/runtime/xr_entry_cell.c 65446775907bdbef5c293d564b1020861836c9b94c52adf727a3e32dbb49241f
anchor-sha256: tests/unit/runtime/test_entry_cell_runtime_archive.c 34bc22820144f368a5e5914ac387f2a19db85ef4555d458b07215548eef1dca0
anchor-sha256: tests/unit/vm/test_vm_decoded_cache.c 1f7e032e521c9cdf3cbe8e3b435a6e4c2e9113a8f838e5ac935209589213f183
anchor-sha256: src/runtime/xr_dynamic_entry_runtime.h f922cec7513c0e232d840936820f270d4b8a0c22c23296fe53a45e924ed76ee4
anchor-sha256: src/runtime/xr_dynamic_entry_runtime.c 96cbabf92dc3dc8f4fb279fe85f87ad9b6a6b90f021a689fcf39505257cd8408
anchor-sha256: src/vm/xr_vm_dynamic_entry.h e365e02d0596394df881026895d127ac54ade9d7bccf5ff272ad6f704a88becf
anchor-sha256: tests/unit/runtime/test_dynamic_entry_runtime.c 119cca4a922e3039c9b57e69102d1fd06b9aaba5cdae55fb10e7fdda0ba9bbb9
