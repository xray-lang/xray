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
only exact function, instruction, call, entry-expectation, slot, and immutable
debug-fact rows. TargetPlan schema 38 carries one debug fact for every target
instruction. It names the exact SemanticPlan operation and its stable ID; when
the operation has a verified source span it carries the source-span stable ID
and coordinates, otherwise it reports context unavailable. It also carries the
exact coroutine-state and owner stable IDs when applicable and the verified
layout fingerprint when the operation result has an exact target layout. The
runtime only attaches these frozen rows; it never derives them from a legacy
bytecode offset, name lookup, SemanticPlan walk, or a guessed relation. A
fabricated, stale, or mutated fact is rejected before materialization. Breakpoint,
step, arbitrary locals/stack, VM/AOT first-divergence, allocation/RC/suspend,
mailbox, provider-cost, and source-span debugging remain unavailable and are
not claimed by this runtime slice.

Evidence:

- `test_typed_dispatch` proves independent switch-provider and function-table
  runs produce byte-identical canonical trace events and profile snapshots,
  proves exact direct-call nesting and return/error
  order, exact generation binding, fixed-capacity sink refusal, profile-disabled
  parity, instruction/call/slot materialization, source-backed stable facts,
  and source-fact mutation rejection.
- `test_typed_frame_runtime_archive` proves trace, profile, materialization,
  debug-session, and request-based dispatcher symbols link from `xray_vm`.

anchor-sha256: src/vm/debug/xr_vm_trace.h a8a281dad51876c63ff9ba6d8bd52e63a8e52f8ff29fbf683aca3323f4f6f924
anchor-sha256: src/vm/debug/xr_vm_trace.c c61c7b755f761d24a4f5c721b09502c7afdba32ed4a7c11acedba802c995a7cc
anchor-sha256: src/vm/debug/xr_vm_profile.h 494f41cb32b3b3e48162f2f5b23c78c5e85ec41afaa702b690c8331f94af1892
anchor-sha256: src/vm/debug/xr_vm_profile.c d4a1cd75c1d520f3a14721e64757559952ffb6c734b62c7a6769d81444ccbda1
anchor-sha256: src/vm/debug/xr_vm_materialize.h d87726c0853aafe634f6888fb242167be4bac256ab117f24a58df5aa711ccb8f
anchor-sha256: src/vm/debug/xr_vm_materialize.c b86beab806b748e1eced0aa4c04c2a87da11f5e774289227fa4d144320195d2a
anchor-sha256: src/vm/xr_typed_dispatch.h 25f04f8562c5159cb775553242e753910c91682d3873913ad3eab867cee4a412
anchor-sha256: src/vm/xr_typed_dispatch.c 8ce9c5270624b2a5c713ea0c57ecd23f09d8940516241f921506c2f7acb3433f
anchor-sha256: tests/unit/vm/test_typed_dispatch.c b3bd5b339e5bbd2756af3db1a791ed98dfeaeff45c404fc234923d1221ae961e
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c 5827191c191aeda84d937af1909a9d6849608fd93569822fd96130680f9f27e9
anchor-sha256: tests/install/run_installed_runtime_symbol_tests.py cb1b4fd056aebee2f73c03d537294df3d8a2dcc8617a55a1a7e25b0e42570228
anchor-sha256: CMakeLists.txt 52120c519042aa84194f877420c808db07fdde1a563f8bdb94e65af9fde9e00b
anchor-sha256: tests/unit/runtime/test_dynamic_entry_runtime.c 529b9e5618207d743789087697ef51d1b9642532e732e1d73f296d812d9e06b1
