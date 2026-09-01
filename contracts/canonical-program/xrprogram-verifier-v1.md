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
3. constants, function signatures, entry point and bounded allocation;
4. canonical value definitions and block-local SSA use;
5. block-parameter CFG, terminators, successor arity and reachability;
6. per-operation result, operand, immediate and successor rules;
7. declared effect and capability closure, including sealed direct callees;
8. immutable validated type-state construction.

Values cannot flow implicitly between blocks. A successor receives all cross-block values through
typed block arguments. This makes dominance local and keeps verification work linear in records,
operands and edges. Current CoreSpec types are copy scalars; ownership/coroutine/import/boundary rows
remain inactive and must be rejected rather than treated as verified.

Diagnostics have a stable kind plus section/function/block/instruction/value coordinates. They do
not expose compiler pointers, source paths or backend implementation details. Every allocation,
record, operation, operand, successor and work unit is bounded.

The reference evaluator consumes only `XrValidatedProgram`. It implements checked and wrapping i64
arithmetic without C signed overflow, explicit traps/errors, block arguments, branches, calls and
the pointer-width profile query. It does not include or call compiler planners, VM handlers, AOT
lowering or generated-C helpers.
