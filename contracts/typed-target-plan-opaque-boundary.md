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

anchor-sha256: src/plan/target/xr_target_plan.h 1f44a60860526a33ea0a585d3a14530abc40f6280625c5fcc0145d95eabf684e
anchor-sha256: src/plan/target/xr_target_builder.c 9e794c5c80893cb90c3c94d0b7b9fd09f9686f2cc87c0c57b57221c198c7316f
anchor-sha256: src/plan/target/xr_target_verify.c 6e71bd80d6dcd1eec77acbd9cbd08f08d08b35c6da22ad66b5a697e3f6a833be
anchor-sha256: src/runtime/xr_dynamic_entry_runtime.c d6cff74156a07c9a7751f3e7d5857f65d3d6d05ca1dbc862605f6cc4fa2c5c16
anchor-sha256: src/vm/xr_typed_frame.h fa468887f8d87a5ff1fdf6c82b863cb7a62a204b4a30542f9fd0c62693486196
anchor-sha256: src/vm/xr_typed_frame.c 397f46fa8647614c06745dc365676989abdc9e079f25a89994b96d9d34fbd405
anchor-sha256: tests/unit/vm/test_typed_opaque_boundary.c cefad5d23ea5544b24d5060767b62a0d9bc1e8232211192cd0b2a0dfbcc9fc68
anchor-sha256: tests/unit/CMakeLists.txt 08db8bd5e51152ec5afcc1aff72b9a9c29fea351619522fbbcf340b873a673dd
