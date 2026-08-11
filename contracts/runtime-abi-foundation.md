# Runtime ABI descriptor and extent foundation contract

Status: preparatory runtime leaf frozen by task 274.

This contract freezes the target-plan-independent representation consumed by a
future canonical runtime ABI cutover. It does not claim that object headers,
value families, VM, or AOT materialization have already switched.

1. Stable IDs and fingerprints are pointer-free byte value types shared across
   compiler, plan, runtime, and audit layers. Runtime code does not depend on
   analyzer, Xi, planner, or C generator data structures.
2. One layout/extent pair describes one physical allocation. Every external or
   multi-buffer backing allocation therefore has its own pair. A single
   descriptor cannot prove group completeness; the runtime group verifier
   validates a complete set for dense part indexes, unique extent/descriptor
   identities, one group identity, and a canonical order-independent summary.
3. Layout uniquely owns fixed prefix size and memory alignment. Extent uniquely
   owns tail offset, stride, operand, provider, and group/part facts. Evaluated
   bytes are derived data and retain the source extent ID and fingerprint.
4. `descriptor_id` names the immutable runtime descriptor. `layout_id` names
   the verified source layout-plan identity from which it was generated.
   `object_kind_id` is a stable identity, not a prematurely frozen runtime tag
   registry or a private VM/AOT enum.
5. Semantic ownership domain and backend materialization remain independent
   axes from `xstorage.h`. An actual domain additionally has a stable contract
   ID and nonzero runtime instance ID. Category allowlists never erase exact
   identity equality.
6. Fingerprints use `xray-runtime-abi-v1\0`, a record discriminant, fixed-width
   little-endian integers, and field bytes in declaration-independent order.
   C struct bytes and padding are never hashed or serialized. Layout
   fingerprints bind the referenced extent fingerprint, not only its ID.
7. Fixed, inline-tail, external, multi-buffer, and provider-defined formulas
   reject malformed shapes, missing operands/providers, overflow, alignment
   overflow, and configured allocation/alignment limits. There is no legacy
   formula fallback.

## Digest anchors

anchor-sha256: src/base/xstable_id.h 3a7abe4d53ba0771a8b064e5d7c395d883253a1a9c65cc46a284872f7119c3b1
anchor-sha256: src/plan/semantic/xr_semantic_ids.h 7ec819570b47e2a3f01132fc729eb73f91dda65cf2d343cb9bee34ad229b4284
anchor-sha256: src/runtime/abi/xr_runtime_descriptor.h 19744b87e3b564ebec4938a3fa4906bf8884b3524af65d89f30c5cc1cfa2a7e4
anchor-sha256: src/runtime/abi/xr_runtime_descriptor.c 769caf23dd04e87d8100182db30936e47a13f4d5e56f3f63bec1b4132ac90fb5
anchor-sha256: tests/unit/runtime/test_runtime_descriptor.c a4c40fbdd8d1725e30fbe513f6c7a571cebd45ab4f50ef59b8f620e18ec18219
