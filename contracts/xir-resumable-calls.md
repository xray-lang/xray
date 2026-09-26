# XIR resumable call boundary

This internal x86_64 call ABI establishes control-flow and cleanup ownership.
It does not certify source calls, generic caches, module instances,
concurrent cancellation, or product migration.

A sealed call table contains typed entry descriptors. An activation owns a copied
table, copied parameter types, and its nonmoving frames. Entry environments and
the opaque instance context are explicitly borrowed for the activation lifetime.
No process-global current instance is used. Inputs are copied before a call is
accepted; scalar results own their inline payload. Unit, canonical bool, i64, and strings are admitted. Managed ownership, output
and one-shot result transfer are governed by `xir-managed-values.md`. ABI mismatches fail before publication.

Each entry is a bounded resume callback and optional non-suspending cleanup.
It returns an explicit continue, call, return, throw, suspend, typed output, or runtime-fault
action. Runtime arithmetic faults are distinct from language throw. Calls yield to a
trampoline; callers never retain child execution on the native C stack. The
caller frame survives until the child completes. The caller then receives one
typed return or throw inbox. There is no implicit blocking fallback.

Suspension publishes a monotonically increasing nonzero wake token. A matching
resume consumes that token exactly once. Stale, duplicate, or terminal resumes
reject without entering code. Polling a suspended activation does not reenter the
entry. Reentrant poll or destruction during resume/cleanup rejects as busy.
Cancellation may be requested reentrantly, but cleanup occurs only after control
returns to the trampoline; it unwinds children before parents. Completion,
throw, cancellation, resource failure, and explicit destruction each release
every accepted frame exactly once, including a frame never entered. Cancellation
from cleanup rejects as busy: cleanup cannot reopen completion arbitration.

The initial context is exclusively driven by one host thread. Concurrent access
is not admitted; these rules do not replace the later scheduler's atomic
completion/cancellation arbitration. Suspension is not generator output or EOF.
Language throw carries an i64 error token in this scalar subset. Runtime faults
remain distinct and unwind the activation without a partial language result.

Metadata and frames share a physical byte budget. Frame depth and resume work
have separate limits. Accounting includes the activation and copied descriptors,
not just user payload. Allocation failure restores live bytes and allocation/free
counts to a balanced state with live bytes at the caller's baseline. Entry state
is aligned to sixteen bytes on this target. Cleanup cannot create new calls or suspend.

Each activation owns a linked stack of nonmoving frame segments. A segment normally
reserves 4096 physical bytes including its header; oversized frames reserve their
exact aligned footprint. A smaller remaining budget may reduce a normal segment,
but never below the complete frame plus metadata and guard space. Every reserved
byte, unused tail, header and alignment gap is charged to the same physical budget.
No active segment is reallocated or shared between activations. Pushing a frame
copies arguments before publishing it; failed argument copies roll back the
reservation and release an otherwise empty segment. Frames are zeroed on admission.
Popping runs cleanup and releases arguments/inbox before rewinding storage. Empty
segments are physically freed immediately, including the last segment on terminal
completion; there is no retained empty-segment cache. Live ancestors keep their
addresses across expansion, child return and suspension. No source borrow lifetime
or escaping view permission follows merely from address stability.

Sanitizer builds poison unused segment storage, frame tail guards and popped
frames, and unpoison only a newly admitted frame. This preserves detection of
cross-frame overrun and use-after-pop while a segment still contains live ancestors.
Normal and instrumented builds use identical physical layout and budget rules.
The host-thread exclusivity and nonrecursive trampoline rules above still apply;
segmentation neither introduces a scheduler nor admits concurrent driving.

verification-test: test_xir_calls
verification-test: test_xir_emit_calls
verification-test: test_xir_call_native
verification-test: test_xir_allocations

The C emitter publishes resumable entries from Lowered instructions. Verified
closed scalar leaf emission is a separate internal optimization entry that
rejects CALL/SUSPEND/THROW; it is never an execution fallback. The VM uses one
instruction implementation for its leaf runner and resumable bindings. Bindings
and immutable artifacts outlive activations. The trusted linker preserves module
function indices; serialized/cache identity admission is not yet provided here.

Source Coro.yield() now uses the same SUSPEND boundary under the source-owner
contract. Source-built module initialization can suspend before or after publishing
slots; progress and code/value owners survive until resume, cancellation or failure.
Cancellation while initializing is sticky and never publishes READY. Cancellation
of an ordinary suspended call stops the instance and rejects stale resumes. Test
hosts drive two instances in alternating order and validate independent fixed
results, exact wake consumption and physical release. This is not qualification of
the multi-threaded scheduler or generator driving protocol.

verification-test: test_xir_source
verification-test: test_xir_source_native
verification-test: test_xir_source_mixed
verification-test: test_xir_packet_vm
verification-test: test_xir_source_allocations
