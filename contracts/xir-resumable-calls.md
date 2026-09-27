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

Call ABI 14 retains the admission TypeArena for the activation lifetime and
passes it through each CallView. Typed argument, return, inbox and action
admission checks the expected arena/type as well as existing execution authority.
VM frame copies and emitted native frame copies use that same arena. Scalar
leaf entries admit only primitive values and use no constructed owner. ARRAY
descriptors do not by themselves authorize a runtime Array carrier or operation;
that implementation and its transitive boundary tests remain OPEN.

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

VM and generated C translate scalar runtime failures through the same fault
action constructor. Divide-by-zero and numeric-range failures retain their
distinct call statuses; allocation failure maps to OOM, and scalar step/frame
limits map to LIMIT. Invalid arguments, artifacts or ABI facts inside an
already admitted entry are BAD_STATE. OK and unknown status values cannot
manufacture success through the fault channel and also become BAD_STATE.
Argument and ABI admission errors before entry remain their existing distinct
statuses. Every fault clears the language result, unwinds children before
parents and balances physical frame accounting. This normalization changes no
instruction wire layout or opcode identity.

## Allocation-free bounds fault detail

Call ABI 14 atomically adds `fault` to Action and CallResult. The only admitted
detail is a 24-byte, alignment-eight record: u32 code, u32 reserved, i64 index,
i64 length. Bounds uses code 430, reserved zero, nonnegative length and an index
less than zero or at least length. Index and length retain their full signed
values. Every non-Bounds action/result carries an all-zero detail. Unknown codes,
nonzero reserved bits, a negative length, an in-range Bounds index, or detail
on a non-Bounds action reject as BAD_STATE before any action side effect.

Bounds is a distinct fatal CALL_BOUNDS status, never language THROW, numeric
range, OOM or a successful result. Its FAULT action has no callee or arguments
and carries the canonical i64 CALL_BOUNDS reason. Its terminal result has unit
value and zero wake. The driver validates and copies detail before unwinding;
cleanup cannot overwrite it or recover the parent frame. Detail owns no memory,
code, arena or Instance lease and can be formatted after all owners are freed.
Constructing/reporting Bounds never allocates. Cancellation arbitration retains
its existing precedence; a discarded action cannot publish partial detail.

Generated native entries assert the new Action/CallResult/detail sizes,
alignments and field offsets. Old Call ABI entries reject before execution.
No reader, adapter, old result signature or compatibility entry is retained.
Implementation and new assertions are OPEN until the corresponding tests pass.

Metadata and frames share one physical byte budget per activation. Frame depth and resume work
have separate limits. Accounting includes the activation and copied descriptors,
not just user payload. Allocation failure restores live bytes and allocation/free
counts to a balanced state with live bytes at the caller's baseline. Entry state
is aligned to sixteen bytes on this target. Cleanup cannot create new calls or suspend.
An Instance may retain one completed activation while constructing one unpublished
replacement under the same per-activation limit. The two physical accounts and
failure-atomic replacement bound are governed by `xir-program-instance.md`;
activation accounting never silently combines or discards either live allocation.

Resume count and value-admission work are separate activation-lifetime accounts.
`poll_limit` initializes the resume count once; a host poll may invoke multiple
resume entries, and later polls or matching suspend/resume never replenish it.
The CallConfig admission work is likewise consumed cumulatively by initial and
child arguments, indirect targets, returns and the current Instance's value
helpers. An Instance initializes that separate work allowance from its configured
poll_limit, but consuming admission work does not consume resume-count credit.
Per-operation copies of the original allowance are forbidden. Consumed work is
not refunded after a failure, even when value/allocation publication rolls back.
Scratch bytes remain a simultaneous temporary-storage bound, not lifetime credit.

Only the current resume callback's actual CallView may borrow the activation's
admission account through `xr_xir_call_admission`. The driver verifies the active
view identity and its current frame, arguments, environment, Instance and arena.
Copied or forged views, cleanup, inactive/suspended/terminal activations and
stale parent views during child execution do not expose that account. The loan
ends when the callback returns; it cannot be stored in a value or retained across
suspension. This is an internal native interface, not a source capability to
rewrite limits or authority.

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
