# XIR local definite initialization

LOCAL_UNINIT declares a typed local place without a value. It cannot be passed,
returned, captured or read as a value. LOCAL_WRITE establishes initialization;
LOCAL_READ and every other access through the place require initialization on
all incoming paths. A block meet is intersection, including loop backedges. The
entry state is uninitialized; executing the declaration resets it. Unreachable
blocks and invalid dominance are rejected by the existing CFG verifier first.

This is constructor storage support, not a default value or an initialized
nominal object. Constructors must assemble a complete STRUCT_NEW before this
can escape. Const field assign-once and declaration-local parameter inference
remain separate source checks; this operation grants neither field access nor
construction permission.

The verifier computes a bounded fixed point for each declared place, using the
existing CFG scratch after reachability and ordinary operand checks. It checks
reads only after convergence, avoiding transient loop-order decisions. Budget
exhaustion fails closed. Checked packets and Lowered are independently checked.

On re-execution, owned storage clears any prior initialized value; scalar
storage has no observable value until written. Existing owned slot state handles
partial cleanup, suspension, cancellation and physical release. The same opcode
survives lowering; VM and C generation implement identical storage lifetime.
Schema8/semantic21 adds opcode77 (OP_COUNT78). Program13/Call14/Value10 remain.
Earlier semantic packets reject; no alternate reader or runtime path.

verification-test: test_xir_locals
verification-test: test_xir_local_native
verification-test: test_xir_checked

verification-test: test_xir_execution
verification-test: test_xir_emit
verification-test: test_xir_native

Constructor extension frozen before implementation: LOCAL_UNINIT immediate zero
is mutable storage; immediate one is initialize-once storage. Other immediates
reject. The verifier also computes may-initialized using union, starting empty;
a write to initialize-once storage rejects when any predecessor path has written.
Redeclaration resets both must and may states. Other write-through operations
cannot mutate initialize-once storage. This advances the semantic revision;
packet shape and runtime ABI remain unchanged. Source constructor admission is
qualified only by its source tests, not these internal storage tests.
