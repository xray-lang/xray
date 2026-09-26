# XIR scalar layout and execution contract

This contract admits internal scalar leaf execution only. Source-language admission,
Managed string calls are governed by `xir-managed-values.md`; module instances
remain unimplemented. Resumable XIR calls are
governed separately by `xir-resumable-calls.md`; leaf tests do not certify them. The current
target is x86_64 little-endian with value boundary ABI version 3. Other targets,
ABI versions, and layout contexts reject explicitly.

The layout service takes type, target, context, and ABI identity. Storage bool is
one byte; i64 is eight bytes aligned to eight. SSA and frame scalar lanes are
eight bytes. Boxed, parameter, and result boundary values are sixteen bytes,
aligned to eight, with u32 type, zero reserved u32, and signed i64 payload. Bool
payloads are exactly zero or one. Unit has no SSA/frame storage; a unit result is
the canonical zero boxed value. Unit parameters are outside the initial subset.

Lowering stores the target identity, parameter/result boundary layouts, and each
function's slot offsets and frame byte count in the immutable artifact. Unit
instructions have no slot. VM and C emission consume these decisions and cannot
derive a different layout. Checked artifacts are never executable. Layout
construction and validation participate in metadata, work, and frame budgets.

Scalar execution validates argument count, type, reserved fields, and canonical
bool payloads before allocating a frame. Each attempted instruction consumes one
step; zero remaining steps fails before the instruction. Frame admission is
bounded and OOM fails without a partial result. Signed arithmetic follows `xir-integer-arithmetic.md`, using unsigned operations
and guarded division to avoid host undefined behavior. Equality yields normalized
bool. Every return and error frees the scalar frame. Results own their inline
payload independently of artifact and execution storage; failure clears output.
Execution counters distinguish live bytes, peak bytes, allocations, and frees.
Execution context is exclusively borrowed during a run; no concurrency claim is
made for concurrent access to that mutable context.
The result is an exclusive output location, disjoint from argument and context
storage. Context counters start at zero; inconsistent live/peak or allocation/free
counts reject before frame allocation. Metadata budgets include the owned artifact
header as well as copied tables and physical layouts.

The C backend emits portable C11 from Lowered blocks and typed operations, using
the same scalar runtime rules as the VM. It does not run an interpreter
inside generated C, recover language semantics from C shape, or call the old
executor. Every translation unit passes the existing always-on W1-W4 verifier
before publication; malformed generated output remains an ICE. Generation is
bounded, and write/allocation failures publish no source buffer. Real generated
output must compile and run under MSVC /W4 /WX and match hand-authored expected
values and errors independently of VM output.

verification-test: test_xir_execution
verification-test: test_xir_emit
verification-test: test_xir_native
verification-test: test_xir_allocations
