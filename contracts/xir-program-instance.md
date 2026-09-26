# XIR immutable programs and isolated instances

This contract governs the internal module/literal/state extension. Source syntax,
generic package decoding, dynamic native-cache admission, and concurrent instance
driving require their own qualification. No legacy artifact reader is admitted.

A program seals a verified Lowered closure before execution. It owns copied
module/function identities, exact signatures, dependency edges, slot declarations,
literal bytes, and a deterministic initialization order. Code environments are
either explicitly process-static or transferred to the program with one release
callback. Instances retain their program; final program release destroys its
metadata and owned code environment only after the last instance. A native image
is a trusted compiler-produced C descriptor, not an untrusted byte decoder.

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
new instance retries. A failure-copy operation returns an independent owned value.

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
