# XIR stage and scalar control-flow contract

This contract owns the initial XIR scalar subset. It does not certify source
admission, generic specialization, or product cutover. Managed strings extend this
core through `xir-managed-values.md`. Scalar
layout and execution are separately governed by `xir-scalar-execution.md`.
Those surfaces remain unavailable until their corresponding contracts and tests
are implemented. No existing executor or safety assertion is retired here.

The semantic stages are Built, Checked, and Lowered. Checking accepts only Built;
lowering accepts only an owned Checked module and verifies its invariants again.
Each successful transition publishes a distinct immutable owned snapshot; input
storage may immediately be destroyed. Failure publishes no partial artifact.
Neither a caller-supplied stage tag nor prior checking exempts input validation.

The scalar subset has unit, bool, and signed i64. Function parameters are read
bool/i64 values; unit is a result/terminator type and has no value ID in this
internal subset. Copying a scalar preserves its value without resource ownership. Integer
addition wraps modulo 2^64; equality and signed less-than return bool. Branch conditions
must be bool. Direct scalar CALL, SUSPEND, and THROW are separately governed by the resumable
call contract. Calls carry a function-table index, a checked operand-table range, and an
exact declared result type. Generic parameters and explicit CALL type arguments are governed by
`xir-generic-templates.md`. Resource and borrowing operations are outside this subset and are rejected rather than guessed or lowered through
an older representation. This subset introduces no new source spelling.

The op definition table owns opcode identity, stage membership, operand count,
result type rule, and successor count. Unknown stages, types, and ops fail closed.
Abstract copy is restricted to Built/Checked; scalar copy is restricted to Lowered.
Lowering preserves CFG and value identity and replaces validated copy with scalar-copy or owned-retain. Managed cleanup is
frozen in the Lowered ownership layout and reverified before consumption.

Blocks partition the instruction table exactly, are nonempty and reachable from
block zero, and end in exactly one terminator. No edge targets the entry block.
Parameter values precede instruction-result IDs; unit instructions define no
usable value. Every use has a matching type and a strictly earlier definition in
the same block, or a definition in a dominating block. Loop backedges are allowed
outside the entry block. PHI is the sole edge-selected value merge, as specified below.
Return values exactly match the declared result. Unused fields are canonical zero.

Budgets bound functions, parameters, blocks, instructions, metadata bytes, scratch
memory, and verification work across the whole module. Counts are checked before
traversal/allocation. Host view pointers must designate the advertised live arrays;
the view API is not an untrusted byte decoder. Owned Checked packet admission is
governed separately by `xir-checked-packet.md`.
Allocation failure, malformed data, and exhausted budgets are distinct failures.

CALL, PRINT and PHI use args[0]/args[1] as a first/count range into the owning function's
operand table. CALL immediate names its callee; PRINT immediate is zero. Empty
ranges are canonical zero/zero. Nonempty ranges partition the table exactly in
instruction order; gaps, overlaps, trailing entries and overflow are rejected.
CALL/PRINT values are type-checked and dominance-checked at their instruction.
PHI inputs are checked at predecessor terminators as below.
CALL range length equals the callee signature. Arity is bounded by the current
native boundary's 65536-value limit and resource budgets, not a two-value format.
Both stage transitions copy the operand table. Lowered layout freezes the maximum
outgoing group size; its byte cost is charged to frame limits and reverified.
VM/native argument staging lives in the stable caller activation across child
calls or suspension; staged values borrow owned SSA slots until the driver copies
them. The old inline CALL/PRINT operand encoding is not retained.

verification-test: test_xir_stages
verification-test: test_xir_allocations
verification-test: test_xir_locals
verification-test: test_xir_local_native
verification-test: test_xir_execution
verification-test: test_xir_native

PHI defines a non-unit copyable value at the beginning of a non-entry block.
Its args are a first/count range of u32 entries in the shared operand table;
entries alternate predecessor block ID and incoming value ID. The nonempty even
range (at most 65536 entries) lists each distinct predecessor exactly once, sorted by block ID. Duplicate
branch successors count as one predecessor. Missing, extra, duplicated, unsorted
and non-predecessor entries reject. All PHIs form a block prefix. Incoming values
have the exact result type and dominate their predecessor terminator, not the
join block. Local places are not values. Generic PHIs are checked at definition
and specialized/rechecked with the same type identity. No implicit conversion,
default construction, visibility exception or extra generic witness is granted.

An edge captures every selected input before replacing any PHI destination.
Lowered layout reserves a private adjacent frame slot per PHI; owned snapshots
are retained before any destination is cleared. Publishing moves those snapshots
into destinations and empties the private slots. Thus loop swaps, self edges and
shared owned inputs preserve simultaneous assignment. Snapshot failure publishes
no destination, faults the activation and clears every live destination/snapshot.
The edge is one indivisible activation step; each PHI still consumes its normal
instruction step. There is no suspension/provider call inside edge capture. Frame
and metadata budgets include scratch ownership, and cancellation/step exhaustion
use normal frame cleanup. Checked serialization preserves the incoming table;
semantic contract 10 rejects earlier versions. No separate executable PHI format.
