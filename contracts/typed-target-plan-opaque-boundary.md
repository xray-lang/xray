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

anchor-sha256: src/plan/target/xr_target_plan.h 824f17196a96d034c3bb1a4cce36d9c11333d178ac8aa6bea95bbb7d29492cb0
anchor-sha256: src/plan/target/xr_target_builder.c cebbc76efed9d03bf3f911200396a6f1231a848c895ed327abc07c0380651468
anchor-sha256: src/plan/target/xr_target_verify.c 7a5d4806c35afcc0241306bf97fcafd1a629ea6097664ffc6ec0518951415ece
anchor-sha256: src/runtime/xr_dynamic_entry_runtime.c 8915b084f4a1de025340c94a2132d6d257c8b2ff3273faf3fd200bd2d5f6580f
anchor-sha256: src/vm/xr_typed_frame.h 1a139fbf8e4dfe08169fa67186c889c79665639f28674f5ecf53babd4f83120c
anchor-sha256: src/vm/xr_typed_frame.c f7cbfdc3dc805bcfdf9c8a75e2a8293c9770f9873ba036402959980959ae13c9
anchor-sha256: tests/unit/vm/test_typed_opaque_boundary.c cefad5d23ea5544b24d5060767b62a0d9bc1e8232211192cd0b2a0dfbcc9fc68
anchor-sha256: tests/unit/CMakeLists.txt abf9964aedf5ac6fd6902d908f5c59a200701f5f204831b96044b925f004d86a
