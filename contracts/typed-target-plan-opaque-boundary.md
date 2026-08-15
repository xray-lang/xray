# Typed TargetPlan opaque boundary contract

The typed VM transports only object representations admitted by the frozen
TargetPlan. It never walks an arbitrary object graph and never forms or
dereferences a C pointer from bytes stored in a typed frame.

An exact, unaliased SemanticPlan `Ptr` or `MutPtr` is the sole opaque pointer
carrier. Target construction and independent verification both require its
machine representation to be `RAW_PTR`, pointer-sized and pointer-aligned,
with `root=NONE`, `ownership=TRIVIAL`, and zero as the null encoding. Frame
store and load copy exactly that complete representation. The carried bits do
not grant allocation, object, root, lifetime, retain/release, call, FFI, or
hosted execution authority. Reclassifying the same row as borrowed or rooted
is rejected by both TargetPlan verification and frame admission.

Channel values have the canonical rooted dynamic representation selected by
their frozen allocation identity. They have no general typed-frame lifecycle
contract and therefore fail frame admission. Mutex, socket, and foreign-handle
instance types, plus an arbitrary graph-node instance, remain rooted semantic
object types, but the current TargetPlan emits no value representation or
executable function family for an identity function over any of them. They
cannot enter a typed frame, be traversed, or be passed to an executor. A future
proxy must add one new exact compiler-owned contract atomically; a class name,
selector, live runtime tag, or pointer cast is never authority.

The current direct-call partition is adapter-free. A fabricated `BOX_DYNAMIC`,
`UNBOX_DYNAMIC`, `FFI`, or `HOSTED` adapter makes the whole TargetPlan invalid.
Persistent dynamic-entry expectations accept only the exact identity adapter
whose fingerprint is independently derived from the exact scalar entry ABI;
an invalid, unknown, or non-identity adapter is rejected before resolution or
execution. There is no compatibility adapter and no fallback to legacy or AOT
declaration interpretation.

`test_typed_opaque_boundary` builds real SemanticPlans and TargetPlans. It
proves byte-exact round-trip of the deliberately invalid address bit pattern
`1` through a production typed frame, then proves borrowed and rooted
raw-pointer mutations fail after recomputing the plan fingerprint. It proves a
real Channel allocation is rooted and refused by frame admission, and that
rooted Mutex, Socket, ForeignHandle, and GraphNode semantic types receive
neither a TargetPlan value representation nor an executable family. It also
mutates each forbidden adapter kind and a valid source-export entry
expectation's identity adapter. Every mutation reaches the structural verifier
rather than passing because of stale fingerprint bytes.

anchor-sha256: src/plan/target/xr_target_plan.h 16f1be1bfc85a85eeb663254f08e5f3e0c2c56430ea8f702870cb8cac3303487
anchor-sha256: src/plan/target/xr_target_builder.c 481ce816c3615c140497e2d868c59a9e8435f404b01e2fee649b5fcbad36a9dc
anchor-sha256: src/plan/target/xr_target_verify.c 96539b22fa959f382a48549ae800b060480a59e59ea7adcab94a0a5335093ef2
anchor-sha256: src/runtime/xr_dynamic_entry_runtime.c 27ff412fb778fd0b9ce603e52c44e39763c42bc12e1c099f2c0a747c014299a4
anchor-sha256: src/vm/xr_typed_frame.h 4d6f311477b86539dd14fba88834a89ebc87fced5f031726bc442bf69bc51d06
anchor-sha256: src/vm/xr_typed_frame.c 80ac935291096963179c8f6c58b3105835426c87b2b693d2a62e1d5c16fc913b
anchor-sha256: tests/unit/vm/test_typed_opaque_boundary.c cefad5d23ea5544b24d5060767b62a0d9bc1e8232211192cd0b2a0dfbcc9fc68
anchor-sha256: tests/unit/CMakeLists.txt a7c215609f83b49d59a650ebe657636f76a4eeb4f1f30b5fe0b0e73b986fca98
