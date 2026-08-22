# Typed TargetPlan runtime debug contract

The runtime-only typed executor exposes one optional debug boundary in its
single `XrTypedDispatchI64Request`. A null debug session performs exactly
the same validation, frame operations, instruction dispatch, result transfer,
and generation lifetime work as an unobserved execution. There is no alternate
debug executor, legacy bytecode offset, source/name lookup, SemanticPlan walk,
or AOT/CGen fallback. The request's mandatory generated switch or generated
function-table provider selection does not alter the canonical event stream or
profile counters; nested direct calls inherit the same provider.

A debug session is an opaque allocated object bound to the root TargetPlan
fingerprint. Creation may attach trace/profile observation only, or retain one
TargetPlan-driven control whose immutable debug plan was built from that same
verified fingerprint. The caller must dispose the session through its unique
pointer-consuming free API; there is no stack-layout contract, copy operation,
or compatibility initializer. A dispatch request borrows the session for its
complete synchronous call; its caller must keep the session owner alive and
must not concurrently free that same session. The caller may release its
control owner after session creation because the session holds an independent
reference. Each claimed execution holds another reference until its common
terminal path, so owner release cannot race a begin or active execution into a
dangling control.
Arm and begin are linearized by the control's idle/arming/active state; neither
can silently overwrite the other's command. Generation
identity comes independently from the execution request; when the session also
records it, the two must match exactly before any frame or event. Debug state
can never supply execution authority. A dynamic child may
carry a different exact plan and generation identity acquired from its pinned
entry token; its events are scoped by those child identities while retaining
the same session, provider, ordinal stream, and profile. The session and every
trace event contain only copied numeric identities and fingerprints; no plan,
frame, slot, or generation pointer is retained by trace, profile, or
materialization state. The control holds frame pointers only while its single
claimed execution is active and clears them on every exit. Its stop callback
receives an ephemeral snapshot whose frame stack, initialized locals, and value
bytes are owned copies; those pointers expire when the callback returns.

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

The debug plan accepts only nonzero frozen source-span or Semantic-operation
stable IDs that exist in the verified TargetPlan debug-fact table. Requests are
sorted and duplicate or missing identities fail closed before a control exists.
There is no file-name, line-number, function-name, or legacy bytecode-offset
lookup. One control may be active in only one request. It may start in continue
or step-into mode; a callback may resume with continue, step-into, step-over,
step-out, or terminate. Step decisions use the canonical event ordinal and
numeric frame depth, so direct and dynamic child calls inherit the same rule.
Step-out at the root frame is rejected because no lower frame can exist.
Callback refusal and explicit termination have distinct dispatcher statuses,
publish no result, and release all control state on the common exit path.

Each successful instruction commit marks only that row's result slot initialized;
prebound child parameters are marked only after exact argument staging. A stop
copies initialized slots from every active typed frame using the frozen slot
size, alignment, register representation, memory representation, identity,
ownership, and root facts. It never reads a poison or uninitialized slot,
exposes a live arena pointer, scans a legacy stack, or invents a local from
source names.
The control admits at most 65,536 breakpoint requests or resolved rows, 256 active frames,
65,536 tracked slots, and 16 MiB of copied local bytes per stop. Each limit is
checked with overflow-safe arithmetic before allocation or slot access; an
over-budget breakpoint plan or stop fails closed without invoking the callback.

Materialization rechecks the immutable plan and event fingerprint, then copies
only exact function, instruction, call, entry-expectation, slot, and immutable
debug-fact rows. TargetPlan schema 42 carries one debug fact for every target
instruction. It names the exact SemanticPlan operation and its stable ID; when
the operation has a verified source span it carries the source-span stable ID
and coordinates, otherwise it reports context unavailable. It also carries the
exact coroutine-state and owner stable IDs when applicable and the verified
layout fingerprint when the operation result has an exact target layout. The
runtime only attaches these frozen rows; it never derives them from a legacy
bytecode offset, name lookup, SemanticPlan walk, or a guessed relation. A
fabricated, stale, or mutated fact is rejected before materialization.
Breakpoint, step, initialized locals, and the typed call stack consume only
those exact facts. Arbitrary memory inspection or mutation, VM/AOT
first-divergence, allocation/RC/suspend, mailbox, and provider-cost debugging
remain unavailable and are not claimed by this runtime slice.

The private target-machine comparison harness projects each accepted typed
event into `xray-canonical-logical-safepoint-trace/1` without adding a CLI or
runtime mode. Its manifest records trace availability separately for the
legacy, typed, and current native lanes. A lane without this exact event
boundary carries an explicit unavailable reason; neither process output nor an
empty trace is accepted as trace evidence. The report still compares the
shared value, error, termination, destruction, and lifecycle observations and
reports their exact first divergent field or lifecycle event. Cross-lane
first-divergence remains blocked until every required lane emits the canonical
safepoints. The harness exercises exact event mismatch and truncation, plus
empty output, abnormal exit, and timeout refusal, before running its corpus.

Evidence:

- `target_machine_scalar_comparison` captures the typed executor's real
  canonical stream, validates exact safepoint fields and order, and preserves
  explicit legacy/native unavailability while comparing every shared scalar
  observation against the independent oracle.
- `test_typed_dispatch` proves independent switch-provider and function-table
  runs produce byte-identical canonical trace events and profile snapshots,
  proves exact direct-call nesting and return/error
  order, exact generation binding, fixed-capacity sink refusal, profile-disabled
  parity, instruction/call/slot materialization, source-backed stable facts,
  and source-fact mutation rejection.
- `test_typed_dispatch` also proves source-span breakpoints, deterministic
  switch/function-table stepping, initialized-only local copies, explicit
  termination and callback rejection, and step-into/over/out across a real
  two-frame direct call without retaining a stack pointer. It also proves the
  breakpoint and snapshot budget edges, root step-out rejection, and session
  ownership after the caller releases its control reference.
- `test_dynamic_entry_runtime` proves the same control steps into a pinned
  child with a distinct plan/generation, observes the exact two-frame stack,
  and steps back to the caller. It also drives terminate, callback refusal,
  and invalid-resume rejection from that child and proves every outcome retires
  exactly one lease while restoring caller/callee pins and leaving no live or
  pending lease.
- `test_typed_frame_runtime_archive` proves trace, profile, materialization,
  debug-session, debug-plan/control, and request-based dispatcher symbols link
  from `xray_vm`.

anchor-sha256: src/vm/debug/xr_vm_trace.h 4e5ed29033c195caa74028ca75f52013abdd340af4d368a04337e692e3067e0a
anchor-sha256: src/vm/debug/xr_vm_trace.c 6206123d9b212506679303049b1ec983cd6696614b1a2a1c36173aad9d7a88de
anchor-sha256: src/vm/debug/xr_vm_trace_internal.h 715c757e80e375f9a4923d9c367cdc22d500fa838c1fff61a1e2a842a9840cf2
anchor-sha256: src/vm/debug/xr_vm_debug_control.h ff0b4660f4f6c9db6783a7e9bfedf4258c14ef33b4a25aa577ee5ba39d59d419
anchor-sha256: src/vm/debug/xr_vm_debug_control_internal.h 7034512a59f1683cd5d0a85997b293f84f6012c1d8aea0363d1d02570f0dfd84
anchor-sha256: src/vm/debug/xr_vm_debug_control.c 0576a576a03a74dffcd02a3b44ae77d1d3f4fd4cd1e5eee7d1b5aa26c27b2721
anchor-sha256: src/vm/debug/xr_vm_profile.h 494f41cb32b3b3e48162f2f5b23c78c5e85ec41afaa702b690c8331f94af1892
anchor-sha256: src/vm/debug/xr_vm_profile.c d4a1cd75c1d520f3a14721e64757559952ffb6c734b62c7a6769d81444ccbda1
anchor-sha256: src/vm/debug/xr_vm_materialize.h d87726c0853aafe634f6888fb242167be4bac256ab117f24a58df5aa711ccb8f
anchor-sha256: src/vm/debug/xr_vm_materialize.c b86beab806b748e1eced0aa4c04c2a87da11f5e774289227fa4d144320195d2a
anchor-sha256: src/vm/xr_typed_dispatch.h 3c3adf76ba8478621d9e6b860e98b111599dc9f4149cef433fbc2db8eee1692a
anchor-sha256: src/vm/xr_typed_dispatch.c d60391ef1366d9933fa19dd7f4fb30f5a2e86eeef6ca45061dba7985d9607cb4
anchor-sha256: tests/unit/vm/test_typed_dispatch.c 0b03b534fe198f74ac4736ca848ee0f9d80a35b44ad3548c52b2ce52f62bfda1
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c 764004285c19abd6ef2491b3d6bdd466425e643f280e719f5833f0ddfaaaa5e1
anchor-sha256: tests/install/run_installed_runtime_symbol_tests.py 70d40dfa429c78f663381887bf4676c2b68c97334c55344554a6da587e886be8
anchor-sha256: CMakeLists.txt 66811cb19bcb3edbaccc997fba4f56e819944207fb474973e718542b9d61ce90
anchor-sha256: tests/unit/runtime/test_dynamic_entry_runtime.c eede18e3210d979c26b0adaca5c5454acdbd609514383d0e285525f69fef9883
