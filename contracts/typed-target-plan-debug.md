# Typed TargetPlan runtime debug contract

The runtime-only typed executor exposes one optional observation boundary in
its single `XrTypedDispatchI64Request`. A null debug session performs exactly
the same validation, frame operations, instruction dispatch, result transfer,
and generation lifetime work as an unobserved execution. There is no alternate
debug executor, legacy bytecode offset, source/name lookup, SemanticPlan walk,
or AOT/CGen fallback. The request's mandatory generated switch or generated
function-table provider selection does not alter the canonical event stream or
profile counters; nested direct calls inherit the same provider.

A debug session is bound to the root TargetPlan fingerprint. Generation
identity comes independently from the execution request; when the session also
records it, the two must match exactly before any frame or event. The session is
observation only and can never supply execution authority. A dynamic child may
carry a different exact plan and generation identity acquired from its pinned
entry token; its events are scoped by those child identities while retaining
the same session, provider, ordinal stream, and profile. The session and every
trace event contain only
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
only exact function, instruction, call, entry-expectation, and slot rows.
TargetPlan schema 37 has
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

anchor-sha256: src/vm/debug/xr_vm_trace.h 9f02f234220c04d260ef0754b752fa4daff7f56041732f8be468ac3e45007fd6
anchor-sha256: src/vm/debug/xr_vm_trace.c 7c14c3d4a06e329d2a79a24f7fe4a978c872d86a0d442930730f234626c57770
anchor-sha256: src/vm/debug/xr_vm_profile.h 494f41cb32b3b3e48162f2f5b23c78c5e85ec41afaa702b690c8331f94af1892
anchor-sha256: src/vm/debug/xr_vm_profile.c d4a1cd75c1d520f3a14721e64757559952ffb6c734b62c7a6769d81444ccbda1
anchor-sha256: src/vm/debug/xr_vm_materialize.h 6e784c930527ea25bee1301db2d16065a11d091e51de5a58316ad36f65a0e9db
anchor-sha256: src/vm/debug/xr_vm_materialize.c dc70a2881d49cab77a9382a5ae9c88e914be806803105aad55a1667001735c6e
anchor-sha256: src/vm/xr_typed_dispatch.h 25f04f8562c5159cb775553242e753910c91682d3873913ad3eab867cee4a412
anchor-sha256: src/vm/xr_typed_dispatch.c 32c7045f5b5bdc00220dc816d099e160a5077f1c3ef16d89ddefe2f04bdcfc1d
anchor-sha256: tests/unit/vm/test_typed_dispatch.c 74e13773e2b235e1dfa4083868e305587bb95604c3445b62bd847c5b3faddbd5
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c 6d0ea6cc1968fc5739b65b39b758e18b784b3da2132a63635b36bccbf9e04309
anchor-sha256: tests/install/run_installed_runtime_symbol_tests.py cb1b4fd056aebee2f73c03d537294df3d8a2dcc8617a55a1a7e25b0e42570228
anchor-sha256: CMakeLists.txt 52120c519042aa84194f877420c808db07fdde1a563f8bdb94e65af9fde9e00b
anchor-sha256: tests/unit/runtime/test_dynamic_entry_runtime.c 5687d46dfaf34f04a68b2cdfc33314569e3b71109cbd621975db66e80a301905
