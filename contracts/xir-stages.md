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
addition has checked signed overflow; equality and signed less-than return bool. Branch conditions
must be bool. Direct scalar CALL, SUSPEND, and THROW are separately governed by the resumable
call contract. Calls carry a function-table index, at most two arguments, and an
exact declared result type. Generic, resource, and borrowing operations
are outside this subset and are rejected rather than guessed or lowered through
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
outside the entry block; this subset has no block arguments or loop-carried values.
Return values exactly match the declared result. Unused fields are canonical zero.

Budgets bound functions, parameters, blocks, instructions, metadata bytes, scratch
memory, and verification work across the whole module. Counts are checked before
traversal/allocation. Host view pointers must designate the advertised live arrays;
the view API is not an untrusted byte decoder. Serialized admission is not provided.
Allocation failure, malformed data, and exhausted budgets are distinct failures.

verification-test: test_xir_stages
verification-test: test_xir_allocations
