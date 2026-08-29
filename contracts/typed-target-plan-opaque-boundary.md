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

anchor-sha256: src/plan/target/xr_target_plan.h 448eb17905d31ae29329e934eb22b61a3aabaefd98bbfadf155e179d0d77ca2b
anchor-sha256: src/plan/target/xr_target_builder.c 14433a79bb4c1f15d4db3c57eb1c0839178b45ea3ff03fb7410ccc12d17466dd
anchor-sha256: src/plan/target/xr_target_verify.c 2547b1b8a96ec834c7176b755aa705fda253b20d935cab001e2f0048b9afb415
anchor-sha256: src/runtime/xr_dynamic_entry_runtime.c 48ec9d693c6bc32c8d08933006363d1a530518c29950886fa2537c3f0a65b456
anchor-sha256: src/vm/xr_typed_frame.h 1a139fbf8e4dfe08169fa67186c889c79665639f28674f5ecf53babd4f83120c
anchor-sha256: src/vm/xr_typed_frame.c 749f45bf957f82be3142e9aa9565b7bf9020b0f29ff494709bb4c5a900edea53
anchor-sha256: tests/unit/vm/test_typed_opaque_boundary.c 95f901461ae9c8103700db5aec2b72797811a01a93cf8d6d5c4c4b312d5fb799
anchor-sha256: tests/unit/CMakeLists.txt e2085ed8a2099f94c4fca59e10da1e71c575337478b8f4cea4de8c8fb699cc81
