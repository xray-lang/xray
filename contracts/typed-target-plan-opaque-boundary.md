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

anchor-sha256: src/plan/target/xr_target_plan.h 910393bc95d3436c33e5f05702fb2fa7b7811585b102200b1db83a54dd6e8f8e
anchor-sha256: src/plan/target/xr_target_builder.c 751153cfe4bb4a9159ed7dee7c0cb84f93fb86de6d1423083d48819ab24a4f80
anchor-sha256: src/plan/target/xr_target_verify.c a0e87d994c3cc174773ddf9d276a5c89c13cb5ee49a30e9f5a6e75d416a8ebc8
anchor-sha256: src/runtime/xr_dynamic_entry_runtime.c 48ec9d693c6bc32c8d08933006363d1a530518c29950886fa2537c3f0a65b456
anchor-sha256: src/vm/xr_typed_frame.h 1a139fbf8e4dfe08169fa67186c889c79665639f28674f5ecf53babd4f83120c
anchor-sha256: src/vm/xr_typed_frame.c 749f45bf957f82be3142e9aa9565b7bf9020b0f29ff494709bb4c5a900edea53
anchor-sha256: tests/unit/vm/test_typed_opaque_boundary.c 3eba7703a43c05965130416f144ca399c1971f5e147f55efe0c8381d9c99ce66
anchor-sha256: tests/unit/CMakeLists.txt 61e458926b236ce69bcdae5795055ad67fd3d6b11bb251ecef53c0d81ef00343
