# Zero-cost residue contract

Status: re-frozen after Task 259 split reference-count traffic out of the
runtime-helper category into its own R7. Task 245's semantic-neutral native
code-shape control layer is unchanged: ordinary optimization remains automatic
and inferred; source controls never assert zero cost, effects, allocation,
safety, ownership, linkage, or ABI. A request may alter one native shape
dimension, while typed verification assets determine whether the requested
stage was actually reached.
Task 254 removes backend-only mutable-capture cell maps and emits the explicit
Xi cell graph directly; this changes neither the residue categories nor their
measurement point.
Task 256 adds an explicit hosted-fragment artifact and source-generated VM
stdlib entries. A fragment has no program entry or runtime ownership, and an
entry is admitted only after the ordinary post-CGen W1-W4 verifier plus its
typed ABI gate pass. The residue categories, allowance set, and measurement
point remain unchanged.
Direct stdlib helpers that return a payload-free enum may carry only its
ordinal across the native shim ABI. CGen keeps that ordinal unboxed in an I64
storage plan and, only at a tagged boundary, attaches one immutable
translation-unit enum-layout sidecar. The boundary is R5; the sidecar is not
R2, creates no per-call map or enum object, and does not alter the R1/R7
partition.

Backend shape requirements in a `xray verify --contract` asset inspect emitted
function bodies after optimization and CGen. They do not rewrite code. Residue
categories are:

- R1: non-whitelisted runtime-helper calls.
- R2: heap allocation.
- R3: pending-error checks.
- R4: bounds-panic branches.
- R5: tagged `XrValue` box/unbox traffic.
- R6: aggregate/native-vector lane round trips.
- R7: reference-count traffic (`xrt_retain` / `xrt_release`; weak-promote
  helpers join the needle set when AOT weak lowering lands).

R1 and R7 partition the runtime-call surface. R1 is pure runtime dispatch;
literal retain/release tokens are counted only as R7, never as R1 (the same
carve-out allocation helpers already have into R2). Reference counting hidden
inside a composite runtime helper — a tagged property store, a container
operation — is accounted by that helper's own R1 token. The two categories are
therefore jointly closed: a function with R1 == 0 and R7 == 0 performs no
reference-count traffic in either form. That pair, together with
`no_reference_cycles` (leak-freedom over the L0 type graph), `runtime_heap`,
and `box`, is the machine-checkable zero-overhead statement for RC-managed
code. R7 counts the residue of incomplete borrow-signature or last-use
evidence — the shape names an inference gap, not a policy knob.

Header-inline raw helpers on the R1 whitelist are not runtime-helper residue
by themselves; retain/release are header-inline too, but they are never
exempt — they are R7 by definition. A
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

anchor-sha256: src/aot/xi_cgen.h 696ddc204e161c42bee708528a2eddc90eaaa8dfc1f8f2bb9590cc5b798371b0
anchor-sha256: src/aot/xi_cgen.c 1b4b203fc3a8fff026cd776ad3ae10c030b945f55a5fee7c28eab248c0380135
anchor-sha256: src/aot/xi_cgen_ctx_impl.inc.c 94d85a40ae5a6b8d3b237984a7457ce15d76f6b1fd5e6095719969a7515ad582
anchor-sha256: src/app/cli/xcmd_verify.c ca122153d89cbc4fa2b2fcffe08604c109729941a5a9b34ff0d72b1d0226e8a8
