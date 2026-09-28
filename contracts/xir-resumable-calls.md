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

Call ABI 16 retains the admission TypeArena for the activation lifetime and
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
Language throw carries a verified enum value with its exact arena-owned nominal identity. Integer error tokens are rejected. Runtime faults
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

Call ABI 14 atomically adds `fault` to Action and CallResult. The bounds
detail is a 24-byte, alignment-eight record: u32 code, u32 reserved, i64 index,
i64 length. Bounds uses code 430, reserved zero, nonnegative length and an index
less than zero or at least length. Index and length retain their full signed
values. Except for the match-failure extension below, non-Bounds actions/results carry all-zero detail. Unknown codes,
nonzero reserved bits, a negative length, an in-range Bounds index, or detail
on an unrelated action reject as BAD_STATE before any action side effect.

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

## Match-failure fault transport

Call and Program ABI 15 admit MATCH_FAILURE with detail code 442 and zero
reserved/index/length fields. Every other status/detail combination rejects
before publication or output. The result is unit with no wake, survives cleanup
and owner destruction, and allocates no diagnostic object. Existing child-first
cleanup and initialization failure stickiness apply. This is the low-level panic
transport; it does not yet implement the language PanicInfo object or catch panic.
It is never represented as an i64 language THROW or a process termination.

Checked semantic revision 28 adds MATCH_FAIL, opcode 89, in all three stages.
It is a unit terminator with no operands, successors or immediate. VM and native
resumable entries produce the same validated E0442 fault action. Scalar leaf
optimization rejects this effect. Packets and native entries of earlier semantic
or ABI revisions reject; there is no compatibility reader or duplicate executor.

## Owned enum error transport

Checked semantic revision 29 and Call/Program ABI 16 replace integer THROW
without a compatibility entry. THROW accepts an enum-typed operand, including
generic applications checked at definition and specialized before execution;
normal result types are unchanged. VM and emitted C
publish the same borrowed typed action. The driver validates enum classification,
exact arena/type and transitive payload authority, takes ownership before child
cleanup, and transfers the owned inbox to the surviving parent. Admission or
copy failure publishes no partial error. Pending errors, one-shot result take,
initialization failure and cancellation retain the same physical release rules.
Runtime faults remain a separate channel. Source try/catch,
full effects and panic objects are separate open capabilities.

## Catch error ownership and protected-call execution

Value ABI 12 adds the enum-only Error handle; Call/Program ABI 17 reject older
entries at independent runtime boundaries. The value conversion APIs implement
retained erasure and exact nominal narrowing. Error type expressions and ERROR_ERASE were admitted in semantic 30;
the current schema 10 / semantic 31 additionally admits explicit invoke edges. ERROR_ERASE has one value operand and an Error result;
the operand must prove Error in its own declaration context. The proof is checked
again after specialization, including an Error actual argument. Error is storable
and copyable but does not prove Sendable or grant enum field authority.

Source expected-type conversion emits ERROR_ERASE only after that proof. A
declared nominal type named Error retains ordinary nominal name resolution;
otherwise the built-in Error type names the enum existential. Error-constrained
generic conversion remains ordinary Checked code, not AST instantiation. The
VM and native emitter canonicalize an Error THROW to a borrowed concrete enum
action, while the driver preserves its exact-arena admission and ownership rules.
VM and native invoke execution use explicit successors and owned result slots.
Source catch and checked narrowing control flow remain pending.

Nominal and authority-bearing array admission share one iterative value walker.
Traversal depth is bounded by work and scratch, not the number of type nodes.
Each visit spends work before inspection; stack growth accounts for simultaneous
old and new buffers. All exits free scratch and restore its allowance, including
allocation failure during growth. Basic arrays without transitive executable
authority retain their type-validated fast path. Error fields and components use
the same nominal/callable/array type-expression checks and iterative admission.
Finite storage layout does not bound existential value depth; tests must cover
values deeper than the descriptor count and restore scratch on every failure.

The catch-all binding has the static type Error. Error is an enum-only owned
existential, not an unrestricted value or a finite ordinary union. It preserves
the concrete nominal identity, ordered generic arguments and active variant.
It supplies no construction, member-access, visibility or Sendable authority.
Concrete enum and Error values may be rethrown; no public throws annotation or
error-set component is added to ordinary function types.

The representation reuses the existing enum object pointer. The owning handle
has static Error type; the object retains its concrete enum type and arena.
Erasure must not mutate that object or allocate a wrapper. Nonempty payloads
retain the object; empty variants retain the arena lease, exactly as concrete
enum values do. Copy, drop, frame cleanup and escaped-result lifetime use the
actual object representation. This is an ownership requirement, not a claim
that recursive admission requires no scratch allocation.

Every Error admission validates a closed enum object in the exact admitted
arena and recursively validates its active payload, including callable
capabilities. A primitive-range Error tag must not bypass the arena check.
Invalid tags, structs disguised as Error, foreign arenas, invalid variants and
exhausted budgets fail before publishing an output. Erasure and narrowing leave
their source unchanged and publish an owned output only after successful checks
and retain. Normal outputs begin as canonical unit handles.

Narrowing compares exact concrete type identity within that arena. The compiler
must prove permission to name the target type; successful identity comparison
does not confer permission to construct a variant or access a private field.
Existing enum tag/field operations require a concrete enum handle, so an Error
handle cannot use them without checked narrowing. The host throw action retains
one canonical representation: concrete enum type plus payload. Rethrowing an
Error canonicalizes the borrowed action while its frame still owns the value;
the driver validates and fixes the independent owner before releasing the frame.

Locally handled calls require explicit normal and error CFG successors. The
catch implementation cutover separates `type_arguments[2]` (first/count in the
caller's generic argument table) from `targets[2]` (CFG block IDs). Generic CALL,
FUNCTION_REF and direct INVOKE use that range; all other operations require it
to be zero. Specialization clears only type_arguments and preserves CFG edges.
Provenance compares the same edges and independently validates substituted type
arguments. Nongeneric ranges are canonical zero/zero. This is a single schema
migration, not an additional interpretation of the old targets fields.

INVOKE and INVOKE_INDIRECT are terminators with normal target 0 and error target
1. Their args remain the ordinary value-argument table range; immediate names
the direct callee or the indirect callable value, respectively. Their type is
the normal result type (possibly Unit), but they define no usable SSA value.
Direct INVOKE retains direct-call resolution and permission checks; it must not
allocate a closure merely to obtain protected-call semantics.

Each successor must be nonentry, distinct and have exactly the originating
invoke as its sole predecessor. For non-Unit normal results, the first normal
successor instruction is INVOKE_RESULT of the exact substituted result type.
Unit results have no result instruction. The first error successor instruction
is INVOKE_ERROR of static Error type. Each result instruction's immediate is
the originating invoke instruction index, not an SSA operand or a function ID;
args, targets and type_arguments are zero. Only the corresponding successor may
contain that selector, once, at its first instruction. No PHI precedes it.
Ordinary dominance governs all subsequent uses. Forged selectors, wrong-edge
uses, additional incoming edges and entry-block selectors reject, including in
unused templates and after packet digest recomputation. General catch joins use
ordinary PHIs after these dedicated successor blocks.

On resume, the backend selects the normal/error successor from the driver's
owned inbox and copies its result into the selector's owned frame slot before
any later call can replace the inbox. Error selection retains the concrete enum
owner as an Error view without an extra wrapper. The inbox is a borrowed view
to backend code; it must not be stolen or dropped independently. Failure to
retain publishes no result and follows normal fault cleanup. Cancellation and
panic/fault outcomes do not enter a normal error handler. No handler stack,
hidden mid-block edge or legacy executor is admitted.

Before source catch is admitted, verification must cover both successors even
for unused templates and statically unselected source branches. Catch clauses
match in source order, unmatched errors propagate, and a catch body uses its
enclosing error continuation rather than catching its own rethrow. A binding
must survive another suspend/call independently of the transient inbox. Error
and panic handlers remain distinct. Effects are derived from the same Checked
graph and rechecked after specialization; unknown effects never prove no-throw.

Required gates include payload and empty-variant ownership, allocation counts
around erasure, foreign-arena and disguised-value rejection, retain/budget
failure atomicity, escaped owners after Program/Instance release, and physical
release after catch, rethrow, suspend and cancellation. VM, generated native C
and mixed calls each need independent expected results. Forged Checked edges
and result use on the wrong successor must reject after digest recomputation.
Adding executable Error types or invoke metadata requires an atomic semantic,
schema and ABI review of the actual affected readers and runtime boundaries;
no compatibility reader or alternate error transport is permitted.
