# Typed local places and structured control flow

A mutable local binding is a typed activation-local place, distinct from an SSA
value. It must have an initializer. LOCAL_NEW defines the place; LOCAL_READ
copies its current value; LOCAL_WRITE replaces it from an already evaluated value.
A place cannot be returned, passed, printed, copied as a value or used as another
place's initializer. Only the first operand of local read/write can name a place.
The initializer must dominate every access. Types match exactly, including generic
parameters before specialization. Read parameters and const bindings stay values.

Checked operations remain abstract. Lowering selects SCALAR_LOCAL or OWNED_LOCAL
new/read/write, preserving the place/value distinction and frozen frame offsets.
Owned replacement retains the source before dropping the previous destination;
self-assignment and aliases stay valid. Reads are independent owned snapshots.
All owned offsets, including places and read temporaries, are cleared on every
activation exit. Current copyable values have no user destructor; precise last-use
release and noncopyable scope cleanup are separate unqualified responsibilities.

Source if/else, while and C-style for require bool conditions. Conditions are reevaluated on
loop entry; only the selected branch executes. Local updates survive joins and
loop backedges without becoming module or process state. Lexical block bindings
do not escape. Unlabelled break/continue target the innermost loop. Every live
path in a value-returning function must return; unreachable source statements
reject. Declared generic constraints still check all branches at definition time.
Only concrete i64 addition/equality/less-than and existing string addition are
admitted here; an ordinary type parameter gains no arithmetic/comparison witness.

CFG construction is bounded by block, instruction, metadata, depth and work
budgets. Block zero has no incoming edge. All stored blocks must be reachable;
every block ends in exactly one terminator. Scope state must not leak between
branches, functions or compiler-session requests. Loops obey the existing runtime
step budget and cancellation boundary. Failures publish no partial artifact.
Checked wire schema remains 2; semantic contract becomes 3 atomically. Older
semantic revisions reject with no compatibility reader.

verification-test: test_xir_locals
verification-test: test_xir_local_native
verification-test: test_xir_source
verification-test: test_xir_source_native
verification-test: test_xir_source_mixed
verification-test: test_xir_packet_vm
verification-test: test_xir_source_allocations

Parenthesized grouping preserves its operand type and evaluation. Boolean ! requires bool; && and || check both operands as bool at definition time,
including a statically skipped right operand. Execution evaluates the left operand
once and evaluates the right operand only when needed, preserving left-to-right
side effects. These forms produce ordinary CFG and typed local places, not new
runtime or XIR operations. Nullable narrowing remains outside this admitted family.

C-style for (initializer; condition; step) has a loop-local initializer scope. The
initializer runs once, the optional condition before each iteration, and the step
after normal fallthrough or continue, before the next condition. Break and return
skip the step. An absent condition is true; an absent step does nothing. Nested
while/for bind exits to their innermost loop. Step expressions cannot see body-local
bindings. Even a step with no incoming path is typechecked; its temporary emitted
instructions/blocks/operand and type-argument ranges are discarded without budget
refund, leaving no unreachable executable blocks. Literals retain their existing
whole-closure table policy. Source checking does not execute the step.

Standalone name++/name-- and for steps require mutable i64 bindings; they use the
same checked addition and local/module storage path as assignment, and produce no
expression value. Overflow faults before replacing the binding. Const/read names,
non-i64 bindings and use as expressions reject. Labels and for-in protocols remain
unqualified. No packet or ABI revision changes: existing Checked CFG/place semantics
fully describe these additional source constructs.
