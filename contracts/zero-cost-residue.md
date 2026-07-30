# Zero-cost residue contract

Status: re-frozen after Task 245 introduced a small semantic-neutral native
code-shape control layer. Ordinary optimization remains automatic and inferred;
source controls never assert zero cost, effects, allocation, safety, ownership,
linkage, or ABI. A request may alter one native shape dimension, while typed
verification assets determine whether the requested stage was actually
reached. Residue categories and allowance semantics are unchanged.

Backend shape requirements in a `xray verify --contract` asset inspect emitted
function bodies after optimization and CGen. They do not rewrite code. Residue
categories are:

- R1: non-whitelisted runtime-helper calls.
- R2: heap allocation.
- R3: pending-error checks.
- R4: bounds-panic branches.
- R5: tagged `XrValue` box/unbox traffic.
- R6: aggregate/native-vector lane round trips.

Header-inline raw helpers are not runtime-helper residue by themselves. A
checked fixed-width byte-Slice load/store contributes R4 when its generated
bounds-panic branch remains; when the AOT plan proves the same Slice receiver,
non-negative affine offset, dominating relational length guard, constant width,
and no clobber, CGen consumes that evidence and emits the raw load/store without
the redundant branch. The proof never changes the checked source boundary.
An explicitly source-marked `unsafe` SIMD load/store, fixed-width integer
byte-Slice load, strict `Slice.window` construction, or byte/POD
`Slice.copyFrom` is a separate boundary: the analyzer records the unsafe scope
on the resolved call, Xi preserves that fact, and native CGen may emit an
unchecked memory access, borrowed view, or bounded copy with an `XR_ASSUME`
precondition and an audit marker. Safe calls remain checked unless ordinary AOT
proof removes only a redundant branch.

The header-inline provider adapters for `codegen.opaque` and
`codegen.compilerFence` are likewise not R1 runtime-helper residue. Their Xi
nodes must lower without boxing, heap allocation, error edges, or hosted runtime
objects. `opaque` preserves the exact integer/pointer type and provenance while
blocking exact-value propagation; `compilerFence` is a compiler scheduling
barrier, not a hardware or synchronization fence.

Small external-linkage leaf bodies may carry a force-inline hint so native LTO
can specialize a cross-module caller. A fixed-width vector wrapper explicitly
marked `@inline` keeps that contract in a static SIMD build, where caller and
callee share the selected ISA. A runtime-dispatch wrapper never inherits the
feature target or its instructions into a baseline caller; it reaches a
separately attributed leaf instead. The hint does not remove the exported
symbol, does not assert effects, and is withheld when the ordinary inline
policy observes loops, allocation, suspension, runtime calls, or local stack
aggregates; reads of already-materialized module/global fixed arrays are not
local stack aggregates.

The default allowance set is empty. A backend contract may name explicit
categories in its typed `shape.allow` list; unknown names are rejected.
Allowances are narrow, reviewable semantic exemptions, not pattern-based
suppression. Ordinary code cannot unlock optimization by asserting a desired
performance result. The only public shape requests are `@inline`, `@noinline`,
`codegen.opaque`, and `codegen.compilerFence`; each is semantic-neutral,
provider-aware, and independently explainable. `xray verify --contract` checks
their lowered/realized evidence but never enables or rewrites them. Category
definitions, scanner boundaries, allowance semantics, and the post-CGen
measurement point are frozen. A change migrates shape filetests and all ports
that cite the affected category.

## Digest anchors

anchor-sha256: src/aot/xi_cgen.h f830e12e06f1cc4934c368144e4e79acda88b7c5a8b3130bbbfbb8627842c434
anchor-sha256: src/aot/xi_cgen.c aa280bd25e296511c32c1ff9f32e03d06cebf87d25e793d2291c84851e5daea1
anchor-sha256: src/aot/xi_cgen_ctx_impl.inc.c dad9f53f56cf97455681e906ce3316bbc1ca52bc485211f7ee7dc98da5574d3c
anchor-sha256: src/app/cli/xcmd_verify.c ba8e85fbba500afe3968839f2734386b1e125cf2a78ceea49b7ec28e1e1129fc
