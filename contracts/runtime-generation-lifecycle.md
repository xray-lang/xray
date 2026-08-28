# Runtime module generation lifecycle contract

This contract freezes the runtime-owned authority for loaded TargetPlan
generations, the closed and exact SOURCE_EXPORT signed-`i64` typed executor,
the bounded direct-`i64` program-graph executor, and the canonical runtime-only
entry registry/cell/cache authority. It does not
claim that INDIRECT_CALLABLE calls, escaping closures, a root scanner,
hosted/FFI adapters, owned/object values, or suspension are installed. The
sole-function scalar route and exact two-module program graph are narrow public
capabilities; they do not authorize general product graphs or dynamic reload.

1. A generation authority requires explicit, nonzero hard limits for loaded
   generations, total pins, per-generation pins, and every pin kind. The same
   runtime mutex serializes generation allocation, pin acquisition/release,
   retirement, unload, and authority destruction, so concurrent attempts
   cannot oversubscribe a checked budget.
2. Loading accepts only an independently verified immutable TargetPlan. The
   runtime assigns a monotonic nonzero generation number and derives a stable
   schema-v3 SHA-256 generation identity from the plan schema, completed family
   mask, exact capability mask, semantic/profile/plan fingerprints, and the
   native runtime/provider/object-header fingerprints. A bounded program graph
   additionally binds its exact program fingerprint, canonical module-set
   fingerprint, and 16-byte generation-closure identity. An ordinary plan
   carries zero in all three program fields. No pointer, path, host name, legacy
   bytecode, or caller-authored machine fact participates in authority.
3. The normal state order is exactly `LOADING -> VERIFIED -> READY -> ACTIVE ->
   DRAINING -> RETIRED -> UNLOADED`. A healthy ACTIVE generation alone may
   acquire pins. ACTIVE and DRAINING may release matched pins. Retirement and
   unload require every generic, in-flight call, callback, destructor, and
   static-root pin to be zero. ACTIVE publication also inserts the generation
   into one authority-owned, dynamically linked live manifest. That manifest
   loans the generation's retained Program TargetPlan and freezes the complete
   schema-v3 identity, including the program fingerprint, canonical module-set
   fingerprint, and 16-byte GCI. It has no fixed module-count layout and is
   removed atomically when ACTIVE begins draining or rolls back.
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
   TargetPlan schema 42 preserves exact SOURCE-import storage and dense
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
   constructs and publishes one immutable decoded cache. For an ordinary plan,
   the independent READY verifier retains the exact sole-function and zero-call
   shape. The first program-graph capability is separately fenced to one graph,
   two partitions, one `PROGRAM_DIRECT` call, and one argument; only its graph
   entry and producer may carry scalar execution rows. READY is unreachable if
   cache allocation, exact graph/GCI binding, or any hard
   function/row/block/byte budget fails; a failed attempt leaves the generation
   VERIFIED and publishes nothing.
6. A healthy ACTIVE eligible generation alone may execute its ordinary sole
   function or the unique entry of the admitted bounded program graph.
   Execution first acquires an `INFLIGHT_CALL` pin, binds the retained plan's
   exact generation identity fingerprint, requires the generation's exact
   decoded-cache plan/schema/fingerprint binding, and then uses only its
   verified instruction facts and typed frame through an explicitly selected
   generated function-table provider; there is no implicit default or fallback
   provider. Every success and failure path
   releases the pin. Program execution additionally obtains its entry only from
   the verified graph record and rechecks the live manifest's complete
   plan/program/module-set/GCI identity before dispatch; it has no public
   function-index or name resolver. DRAINING rejects new execution while permitting an
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
   global budgets. It also independently walks the bounded live-manifest list:
   every row must be ACTIVE, belong to the same authority, retain the same
   Program TargetPlan, and match the generation's complete identity; ACTIVE
   must occur exactly once and every other lifecycle state must occur zero
   times. For every READY, ACTIVE, or DRAINING generation it separately
   reconstructs the ordinary sole-scalar or bounded program-graph eligibility
   without calling the production PREPARE helper and requires one exact
   published decoded cache. A graph cache hit binds the same retained TargetPlan
   object and intact fingerprint, the complete generation identity, the program
   graph row, a freshly recomputed canonical module-set fingerprint, and the
   exact GCI. A wrong generation, foreign same-fingerprint plan, or mutated graph
   or partition fails closed; no per-module executable plan or second cache is
   constructed.
   A pre-PREPARE VERIFIED generation may have no cache; once published, the
   cache remains generation-owned through drain and retirement, while the
   in-flight pin prevents unload during reuse. UNLOAD destroys the cache before
   releasing the retained plan. Its transition model rejects identity mutation,
   skipped/repeated states, mismatched pins, illegal rollback, and revision
   mutation. Program-graph ACTIVATE invokes this independent verifier before
   live publication, then rechecks the exact decoded cache while holding the
   authority gate; a wrong GCI or edited/foreign plan remains READY and is never
   published.
8. The standalone public headers and lifecycle/scalar/program execution symbols, plus the
   internal production entry-cell implementation, are shipped by the Core
   component and link from `xray_vm` without compiler builders, encoders, mutable
   analyzer graphs, or Xi implementation symbols. The archive includes the
   immutable core-intrinsic registry that SemanticPlan verification owns.
   Installed runtime code can derive ordinary or canonical program-module-set
   authority from exact XSM images and materialize a matching XTP. Its archive
   gate owns both the XSM/XTP-to-sole-scalar route and the exact two-XSM bounded
   graph route, including reverse input order and joined concurrent execution.
   This remains an installed artifact-runtime boundary: it makes no general
   source/CLI, dynamic-reload, or `PRODUCT_ACTIVE` claim.
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
   path. The opaque `XrProgram` facade similarly accepts the complete XSM image
   vector plus one XTP, canonicalizes only by verified program-module row, and
   reuses that same generation, decoded cache, and live manifest. Its execute
   call selects only `entry_target_function`. Multiple execute calls may run in
   parallel, and unload decides quiescence before it takes a single teardown
   step: an unload that lands while a call is in flight, or while the
   generation still owns a pin, is refused with the program untouched and still
   callable. That is a lifecycle boundary, not a concurrent reload protocol.
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
    TargetPlan schema 42 and XTP schema 42 persist that ABI fingerprint in a
    dedicated entry-expectation row together with the identity-adapter and
    target-profile fingerprints. The artifact never persists an entry cell,
    handle, generation pointer, or code pointer. No earlier schema is accepted.
13. Binding requires an immutable verified TargetPlan with an intact plan
    fingerprint, an exact `SCALAR_I64_CLOSED` function, and zero adapter rows.
    The only admitted adapter is an identity adapter whose fingerprint is
    derived from the entry ABI. The expectation binds ABI, adapter, plan,
    generation, binding, and executor identities; any mismatch fails closed
    before execution. The installed dynamic subset is SOURCE_EXPORT with an
    exact signed-i64 identity adapter only. It may resolve to the typed VM or a
    process-local native executor, but still does not claim general hosted
    calls, FFI, aggregate/Buffer, ownership-bearing, suspending, or
    non-identity-adapter calls.
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
    without a second decrement. Dynamic context schema 4 carries one frozen
    generated adapter binding and has no legacy release callback or untyped
    lease. Every successful dynamic acquisition first
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

anchor-sha256: include/xray_runtime_generation.h e2540f1ff42e095c1a7e5a27387a74fbb26d778ead89846acc502b4b542da631
anchor-sha256: src/runtime/xr_module_generation_internal.h 0f80619cde6e8994ad2ae4b4dc93f50e0ce0735decb487e22c8b3b33202138b2
anchor-sha256: src/runtime/xr_module_generation.c 244477e3660ab3040fcc33b27e1fe37c2650ce03d2e422c2a7c9020a3d05efdb
anchor-sha256: src/runtime/xr_module_generation_verify.c a73b6751f32a92c74ba401cf4e552d105da7f32e1ddd0cf5cf27cd0592cdfcdd
anchor-sha256: src/vm/xr_typed_dispatch.h fffd736ebbad8f05ebd40fa04a14cb197b35ff1b0d84d5737fb06b23b93029ab
anchor-sha256: src/vm/xr_typed_dispatch.c a07a0c22b71e292a8b560d93d58474973a042ea20d2446493e1dcdb0574b5683
anchor-sha256: src/vm/xr_vm_decoded_cache.h 55ac6ffaab71ac0e77a3db5e10ad326057d0052f4ae3b9722029c8ea06c49cf0
anchor-sha256: src/vm/xr_vm_decoded_cache.c f1f420b39d78f39e372b3378425809fb6c7049bad84aa02f84df5e542cfd83de
anchor-sha256: src/vm/xr_typed_frame.h 1a139fbf8e4dfe08169fa67186c889c79665639f28674f5ecf53babd4f83120c
anchor-sha256: src/vm/xr_typed_frame.c 749f45bf957f82be3142e9aa9565b7bf9020b0f29ff494709bb4c5a900edea53
anchor-sha256: scripts/check_coroutine_lifecycle_projection.py 74fdc88cea8045a258dae39f2194839ec54f3a2b8759fa56f1b537226fdbc1a2
anchor-sha256: contracts/target-machine/diagnostic-codes.toml ba51e88a8f545e6e4086024940783f9150dc81d96b6d76e7aca0501bdff719fb
anchor-sha256: tests/unit/runtime/test_runtime_generation.c 0332f8c2423f606919ffab298a69ee14b863c4f5e590b5d51aa3c39f9344f14c
anchor-sha256: tests/unit/runtime/test_runtime_generation_archive.c 6824d75bad49bbd7dce591994ab2368ec58537cd1f82ebfa654445bda828b41b
anchor-sha256: scripts/target_machine_retired_runtime_symbols.py 3db52d4670d4d76a640d91709f5a6fdd091511ac421ca6326c34ed3b8739d4f7
anchor-sha256: tests/install/run_installed_runtime_symbol_tests.py 4c2a03c5f1f75a992c1a9aa67f8501f6ad49c1db3a93dfb9b3266884ca7f39d6
anchor-sha256: tests/install/run_install_public_surface_tests.py 7573dcf14236aebbad3f3840844d14f7060618d661e64886e31fb5f5ce3820be
anchor-sha256: include/xray_runtime_api.h a84f9ce3063c719f1ef4888b633111e0ab5baf61598c956599f1224b7498e102
anchor-sha256: src/runtime/xr_runtime_api.c b5f3de029b73e6f62f7b287934c3b4169c1f043a834d05ad8fdaf4056ad1e917
anchor-sha256: tests/unit/runtime/test_runtime_api_archive.c b7ef1d75a66f12b0b408dcd73a672e3e7df8af4a140ce612612996b60a778b9a
anchor-sha256: src/runtime/xr_entry_cell.h 9e5012d17116a09ba81fccce7c74c380f4f74726026001f88010406589a19b7d
anchor-sha256: src/runtime/xr_entry_cell.c c2bc18e2eb0c40767bff70b0137387a81d55bbe0b767673befcdc5acce4386a0
anchor-sha256: tests/unit/runtime/test_entry_cell_runtime_archive.c 34bc22820144f368a5e5914ac387f2a19db85ef4555d458b07215548eef1dca0
anchor-sha256: tests/unit/vm/test_vm_decoded_cache.c 576c9e443c711070aff3ab58efbaac42266f89be6be64d49f84889dc9668723c
anchor-sha256: src/runtime/xr_dynamic_entry_runtime.h 84d4d2c4feacd955ec13ed949379c8a23ca1a966c37221a5e8ec04126c1c55dc
anchor-sha256: src/runtime/xr_dynamic_entry_runtime.c 48ec9d693c6bc32c8d08933006363d1a530518c29950886fa2537c3f0a65b456
anchor-sha256: src/vm/xr_vm_entry_adapter.h 260bca5ab4abcef7cc679f5674e92c0b2c8e95fa04444b73cb1e3f8584b61544
anchor-sha256: src/vm/xr_vm_entry_adapter.c a18c76b33fa1a35b0b2b756d6eab77de5b7876f58b65bf6c5605a7596e701547
anchor-sha256: xisa/target/vm_entry_adapters.def db46c172fa847c54cb24d477404f00d74db9996b99be9fa357a3ce0864a9ddb9
anchor-sha256: src/vm/xr_vm_dynamic_entry.h 50a175071a41a521e11fa672b7f17663b73dda321fd6ab9703196324e642dfda
anchor-sha256: tests/unit/runtime/test_dynamic_entry_runtime.c 1c09d9175acb0870f42515071a88cdf6beb1f28becdb179fd9b3435945bed680
