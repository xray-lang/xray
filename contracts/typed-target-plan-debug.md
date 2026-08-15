# Typed TargetPlan runtime debug contract

The runtime-only typed executor exposes one optional observation boundary in
its single `XrTypedDispatchI64Request`. A null debug session performs exactly
the same validation, frame operations, instruction dispatch, result transfer,
and generation lifetime work as an unobserved execution. There is no alternate
debug executor, legacy bytecode offset, source/name lookup, SemanticPlan walk,
or AOT/CGen fallback. The request's mandatory generated switch or generated
function-table provider selection does not alter the canonical event stream or
profile counters; nested direct calls inherit the same provider.

A debug session is bound to one exact TargetPlan fingerprint. When generation
identity is present, the session copies the complete public generation identity
and every root and child frame admits it through the existing exact generation
binding check before execution. The session and every trace event contain only
copied numeric identities and fingerprints; no plan, frame, slot, or generation
pointer is retained by trace, profile, or materialization state.

The canonical event order is frame-enter, block-enter at each reached block,
instruction before its semantic action, call-enter before child-frame work,
nested child events, call-return with the child outcome, error on each affected
frame, and frame-exit with its outcome. Event ordinals and frame IDs start from
zero for each request and advance deterministically. Function, instruction,
block-entry, call, and slot facts are scoped by the exact TargetPlan
fingerprint; generation number and fingerprint are present only when exact
generation context was supplied. A sink refusal or fixed-buffer capacity edge
stops execution with `XR_TYPED_DISPATCH_TRACE_REJECTED` and publishes no result.
When the request carries an exact decoded cache, block-entry identity comes
from its verified block partition and is written into the same frame context;
event kinds, identities, ordering, step accounting, and rejection behavior are
unchanged.

Profile counters consume only successfully accepted canonical events. They
count event kinds and generated instruction opcodes, record maximum frame
depth, saturate instead of wrapping, allocate nothing, and retain no runtime
authority. Profile presence or absence cannot make a plan legal, change a slot
byte, extend a generation pin, or change the program result.

Materialization rechecks the immutable plan and event fingerprint, then copies
only exact function, instruction, call, and slot rows. TargetPlan schema 33 has
no source-span table, stable owner identity, or slot-to-layout identity relation;
those facts are reported as `XR_VM_DEBUG_FACT_SCHEMA_UNAVAILABLE` rather than
guessed from semantic IDs, names, layouts, or compiler structures. Breakpoint,
step, arbitrary locals/stack, VM/AOT first-divergence, allocation/RC/suspend,
mailbox, provider-cost, and source-span debugging remain unavailable and are
not claimed by this runtime slice.

Evidence:

- `test_typed_dispatch` proves independent switch-provider and function-table
  runs produce byte-identical canonical trace events and profile snapshots,
  proves exact direct-call nesting and return/error
  order, exact generation binding, fixed-capacity sink refusal, profile-disabled
  parity, instruction/call/slot materialization, and explicit unavailable
  source/owner/layout facts.
- `test_typed_frame_runtime_archive` proves trace, profile, materialization,
  debug-session, and request-based dispatcher symbols link from `xray_vm`.

anchor-sha256: src/vm/debug/xr_vm_trace.h 7763cd87f285d11f0db3ac32caa235bb40fdb1db2dd4b090b02adac265a2e83f
anchor-sha256: src/vm/debug/xr_vm_trace.c 117f3c230b90e2bde2817996af8f8108a3e85b6a7f1ed3575d356fc0805ee544
anchor-sha256: src/vm/debug/xr_vm_profile.h 494f41cb32b3b3e48162f2f5b23c78c5e85ec41afaa702b690c8331f94af1892
anchor-sha256: src/vm/debug/xr_vm_profile.c d4a1cd75c1d520f3a14721e64757559952ffb6c734b62c7a6769d81444ccbda1
anchor-sha256: src/vm/debug/xr_vm_materialize.h 6e784c930527ea25bee1301db2d16065a11d091e51de5a58316ad36f65a0e9db
anchor-sha256: src/vm/debug/xr_vm_materialize.c dc70a2881d49cab77a9382a5ae9c88e914be806803105aad55a1667001735c6e
anchor-sha256: src/vm/xr_typed_dispatch.h 10c108b77e3beff1dfd6c04137ce684a4c8c1d08b3af3a4402dae1443fcff768
anchor-sha256: src/vm/xr_typed_dispatch.c d322275c81f8e833f2f9a850ca8163384e42bb9d703d8a40c16ccb7d711aa9ac
anchor-sha256: tests/unit/vm/test_typed_dispatch.c 6759f44e43d36d704cb5d77330dd048f3c699280042eb856877cf5e5239c4bcd
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c 948a32a64f0f74a42f088da5308c076788f1b87cf39bcede5589ad47aceda380
anchor-sha256: tests/install/run_installed_runtime_symbol_tests.py d68f03a9e2da14514ede7254e10a55b4655c2b250e4a815201bed9480928f4da
anchor-sha256: CMakeLists.txt 6afe2cdfb3157defec19cb249bc3dd4a3634e3091b83845ff21e3df45096edb2
