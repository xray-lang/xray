# XrProgram verifier v1 contract

`XrValidatedProgram` is the only semantic execution admission type. It owns an exact copy of the
canonical bytes and can only be constructed by `xr_program_validate` after structural and semantic
validation succeed. A digest or successful structural decode is not an execution capability.

The CoreSpec semantic fingerprint excludes consumer coverage, KAT routing, editorial descriptions
and tombstone prose. Advancing a verifier/VM/AOT implementation therefore cannot invalidate a
program whose language meaning did not change; changing a normative type, operation, effect,
ownership, control-flow or trap rule must change the fingerprint.

The v1 verifier is ordered and fail closed:

1. canonical structural decode and resource admission;
2. known CoreSpec semantic identity, computed from normative semantics only;
3. logical type graph, constants, function signatures, entry point and bounded allocation;
4. canonical value definitions and block-local SSA use;
5. block-parameter CFG, terminators, successor arity and reachability;
6. per-operation result, operand, immediate and successor rules;
7. monotone SSA ownership: every affine owner is consumed once or transferred once on every
   successor edge, while READ calls create only call-bound non-consuming borrows;
8. declared effect and capability closure, including sealed direct callees;
9. immutable validated type-state construction.

Values cannot flow implicitly between blocks. A successor receives all cross-block values through
typed block arguments. This makes dominance local and keeps verification work linear in records,
operands and edges. Dynamic aggregate and variant rows are declaration-ordered logical value shapes:
their IDs are canonical semantic-key order, every referenced type must exist, and recursive-by-value
cycles are rejected. No offset, alignment, slot, register class or target ABI fact is admitted.
Every type has an explicit logical ownership class (`TRIVIAL` or `AFFINE`) and copy contract
(`TRIVIAL`, `EXPLICIT` or `FORBIDDEN`). Every SSA definition carries `NON_OWNER` or `OWNER`.
`core.owner.copy` admits only a permitted copy contract and creates an independent affine owner;
move, drop, MOVE calls and affine returns consume owners. Aggregate and variant construction copy
trivial operands and consume each affine operand once into the result. Nested ownership and copy
contracts propagate through the logical type graph. A non-owner affine operand cannot become an
owning field implicitly; repeated transfer or use after construction is rejected. Copy-forbidden
values can move into containers whose derived contract remains forbidden, but explicit copy of
either the value or such a container is rejected. Calls, cleanup, coroutine and boundary rows must
satisfy their explicit active operation contracts; supported opcode spelling is not a substitute
for those semantic checks.

Provider-failed and cancellation successors enter reason-preserving control-flow subgraphs, not
necessarily terminal blocks. Reason is derived from the exact operation and successor ordinal;
there is no serialized reason flag or runtime cleanup stack. A bounded per-function traversal
propagates normal execution, provider-failed trap 7, and cancellation independently. Ordinary
branches and locally handled typed invoke outcomes preserve the incoming reason. An exact
provider/call refusal edge enters the trap-7 domain, including from cancellation. Distinct reason
domains cannot share blocks, and a normal path cannot enter cancellation through an intermediate
adapter. Every explicit cleanup exit preserves its reason: trap 7 cannot return or publish error,
panic, or cancellation; cancellation cannot become an explicit trap along an ordinary edge.
Existing edge types, exact operand segments, unique owner transfers, and safepoint facts are still
verified. An adapter may drop owners not used by the remaining cleanup and branch onward, but may
not omit or duplicate an incoming owner. Loops inside a cleanup body are legal; finite graph
validation proves explicit exit closure, not termination of arbitrary language programs. Coroutine
yield or a suspending child call inside cleanup is rejected even when its resume edge would
eventually return to the same reason domain. Escaping language error/panic during a defer retains
the separate fatal E0443 rule and is not converted into
provider refusal or cancellation recovery.

Diagnostics have a stable kind plus section/function/block/instruction/value coordinates. They do
not expose compiler pointers, source paths or backend implementation details. Every allocation,
record, operation, operand, successor and work unit is bounded. Reference and VM execution also
bound aggregate value cells independently from instruction steps, so a small looping program cannot
turn a wide logical aggregate into unbounded executor memory growth.

The reference evaluator consumes only `XrValidatedProgram`. It implements checked and wrapping i64
arithmetic without C signed overflow, explicit traps/errors, logical aggregate/variant values,
block arguments, branches, calls and the pointer-width profile query. Aggregate update produces a
fresh logical value, and variant projection publishes the named tag-mismatch trap before reading a
payload. Explicit owner copy recursively clones logical aggregate carriers under the same bounded
value-cell budget used for ordinary construction. Before a one-shot call releases its arena, an
aggregate return or uncaught typed error is recursively detached into evaluator-owned logical
storage; callers use the typed aggregate view and explicitly dispose the outcome. The evaluator
does not include or call compiler planners, VM handlers, AOT lowering or generated-C helpers.
