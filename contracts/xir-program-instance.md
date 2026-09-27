# XIR immutable programs and isolated instances

This contract governs the internal module/literal/state extension. Source syntax,
generic package decoding, dynamic native-cache admission, and concurrent instance
driving require their own qualification. No legacy artifact reader is admitted.

A program seals a verified Lowered closure before execution. It owns copied
module/function identities, exact signatures, dependency edges, slot declarations,
literal bytes, a closed constructed-type arena, and a deterministic initialization
order. Program ABI 11, Call ABI 14 and Value ABI 10 are admitted atomically. The
ProgramSpec supplies the unique type pool; sealing verifies and deep-copies its
descriptors into the arena before publishing the Program. Code environments are
either explicitly process-static or transferred to the program with one release
callback. Instances and function-admission records retain their program; final
program release destroys its metadata and owned code environment after the last
Program lease. The arena has no reverse Program lease and can outlive that release
through an escaped constructed value. A native image
is a trusted compiler-produced C descriptor, not an untrusted byte decoder.

Constructed boundary identity is `(arena, local type ID)`. Separate seals reject
each other's values even if descriptor structure and IDs match. Instances of
the same Program share its arena; function execution still requires the correct
live Instance gate. ARRAY descriptors are admitted as metadata only in this
foundation. Array construction, cross-Instance pure Array values, COW operations
and escaped Array physical release remain unimplemented and unqualified here.

Modules form a DAG rooted at the entry module. Cycles, duplicate identities,
duplicate dependencies, unreachable modules and invalid indices reject. A ready
module with the lexicographically smallest UTF-8 name is initialized first;
dependencies always precede importers. Each module has one private zero-argument
unit initializer. The root entry is a distinct zero-argument i64 function. CALL
cannot invoke an initializer. Cross-module calls require an exported declaration
in an imported dependency. Slot access is private to its declaring module.

Slots are typed and per instance. CONST slots publish once, from their owning
initializer only. Mutable slots are restricted to the root module and the root
execution flow. No library module var is introduced. Atomic<i64> is a Sendable
identity value: a const handle may perform audited synchronized interior mutation.
Copying it retains the same cell, never applies string CoW. Initial allocation is
per instance. Its first operations are new, SeqCst load, and SeqCst fetchAdd;
fetchAdd returns the old value and wraps modulo 2^64 without signed C overflow.
Other Atomic operations/orderings are not yet admitted by this internal subset.

Initialization has one publisher, even while suspended. The initial instance API
is exclusively driven by one host thread; a second/reentrant start returns BUSY.
No waiters or concurrent cancellation claims follow from this restriction.
Reads require a published slot and either a ready module or the current publisher
of that module. Completing an initializer with missing slots fails closed.
Success advances once to the next initializer; the root initializer is never
replayed as the entry body. Failure, cancellation, OOM and runtime faults during
initialization are sticky and release published slots in reverse publication
order. A later start returns the same failure without replaying effects; only a
new instance retries. Failure caches the complete CallResult, including Bounds
detail, before releasing the activation and slots. The sole failure-copy API
accepts an empty CallResult output (READY, unit, zero wake and detail), copies
the value with independent ownership and copies detail by value. Failure leaves
the output unchanged. The old value-only signature is removed atomically with
Program ABI 9; current Program ABI 11 preserves this complete fault contract.
Repeated failed polls/copies preserve the original index/length
without rereading released storage or replaying initialization effects. An
ordinary entry Bounds fault does not change an already-ready Instance into an
initialization-failed Instance. These new assertions remain OPEN until tested.

After readiness, entry calls share the instance's slots and providers. Different
instances share code but no state or initialization progress. Replacing a root
var immediately releases its previous owner. Instance shutdown stops admission,
cancels/drains its activation, releases slots in reverse publication order, drops
its domain handle and finally its program lease. Pure string results retain only
their allocation domain and survive instance/program destruction. Atomic results
retain their cell/domain but do not retain unrelated module slots.

Each accepted root execution has a monotonically increasing epoch. Resume must
match both epoch and activation wake token, so a stale wake cannot match a later
execution. Poll results are borrowed; owned results use explicit transfer. No
external callback may destroy an instance while it is actively driving code.

Literal bytes are strict UTF-8, length-delimited and copied on compiler stage
transitions. CONST_STRING creates an owned runtime string in the instance domain.
Unknown declarations, types, slot permissions and malformed operands reject before
execution. Metadata counts, bytes, graph work, code frames and instance values
have explicit budgets. Every allocation failure publishes no partial owner.

`call_limit` bounds copied entry descriptors and physical frame segments per
activation. Replacing a completed execution may retain one terminal activation
and one unpublished candidate, each independently accounted; their combined call
storage never exceeds twice `call_limit`. Candidate failure restores its live
bytes and depth and balances allocations/frees without changing the terminal
activation, result, epoch or Instance state. Successful replacement releases the
terminal activation before publishing execution progress. Both activations share
the Instance's single value Domain and `value_limit`; replacement grants no second
value budget. There is no third retained activation or uncharged frame cache.

Each candidate start receives one unpublished value-admission work account,
initially bounded by the configured poll_limit independently of resume count.
Starting a function value charges its gate resolution and argument preparation
to that same candidate account. Call construction receives its remaining work;
successful publication carries that balance into the activation. Rejected host
input, budget or allocation failure cannot debit an existing activation or change
its result, epoch or initialization state. A new independent start may obtain a
new account; repeated helpers, child calls, polls and suspension within an
activation may not. All existing Instance helper admission consumers borrow the
current CallView's account instead of recreating a full per-helper allowance.

`value_limit` charges physical allocations made in that Domain, not all bytes
reachable from an Instance. Retaining imported shared storage does not charge
it twice. The allocating Domain remains alive with its backing after the origin
Instance is destroyed. New Array allocations caused by mutation use the current
admission Domain; a sufficient unique backing may be changed in place without
retagging its existing charge. Cross-Domain growth prepares replacement storage
in the receiving Domain and publishes it only after every fallible step succeeds.
Domain identity grants neither write permission nor function execution authority.

CONST_STRING indexes a literal; SLOT_LOAD returns an owned copy or scalar value.
SLOT_INIT and SLOT_STORE have a unit result and one exactly typed operand. The
former is admitted only in the owning initializer, the latter only for a mutable
root slot. ATOMIC_I64_NEW consumes an i64 initial value; ATOMIC_I64_LOAD consumes
an Atomic<i64>; ATOMIC_I64_FETCH_ADD consumes that handle and an i64 delta. OUTPUT
accepts only bool, i64 or string and selects stdout/stderr with its immediate.
These operations use the same checked declarations in VM and emitted native C.

verification-test: test_xir_instances
verification-test: test_xir_emit_program
verification-test: test_xir_program_vm
verification-test: test_xir_program_native
verification-test: test_xir_allocations

Program ABI 11 includes the native nominal type descriptors and the per-function
nominal_owner word. XrXirTypes owns a declaration/identity table, and each
XrXirTypeNode includes ordered nominal arguments and substituted field types.
Program ABI 10 and earlier reject before reading declarations or type metadata,
taking a code lease, or allocating a runtime arena. Generated C binds the numeric
Program ABI in its static assertion. Tests supply inaccessible descriptor pointers
with old Program and Value ABI versions to verify this admission order.

Value ABI 10 admits the owned boxed nominal carrier through trusted runtime
helpers; its tagged value remains 16 bytes. Call ABI 14 is unchanged. Helpers and
metadata alone do not establish source admission. STRUCT_NEW/GET/SET consume verified
construction/member authority under `xir-nominal-struct-types.md`. Closed
nominal signatures and root slots use the ordinary owned transport contract,
with type visibility checked again during sealing. The declaration-owner word resolves to a same-module
nominal declaration; entry and initializer functions must have no nominal owner.
The shared member authority rules are governed by `xir-nominal-struct-types.md`.
