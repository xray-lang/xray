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
move, drop, MOVE calls and affine returns consume owners. In this slice affine aggregate/variant
payloads and local places are intentionally restricted to trivial child types, so nested managed
storage cannot acquire hidden obligations. Sealed-invoke cleanup, coroutine, import and boundary
rows remain inactive and must be rejected rather than treated as verified.

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
value-cell budget used for ordinary construction. The evaluator does not include or call compiler
planners, VM handlers, AOT lowering or generated-C helpers.
