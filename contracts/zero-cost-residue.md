# Zero-cost residue contract

Status: re-frozen after the xxHash parity work made module-owned immutable
fixed arrays available as hosted static data, kept unhinted return branches
neutral, made caller-proven raw slices free of pending-error residue, and kept
AVX-512F values native inside attributed feature islands. The CGen matrix and
xxHash shape contracts were rerun; residue categories and allowance semantics
are unchanged.

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

Small external-linkage leaf bodies may carry a force-inline hint so native LTO
can specialize a cross-module caller. The hint does not remove the exported
symbol, does not assert effects, and is withheld when the ordinary inline
policy observes loops, allocation, suspension, runtime calls, or local stack
aggregates; reads of already-materialized module/global fixed arrays are not
local stack aggregates.

The default allowance set is empty. A backend contract may name explicit
categories in its typed `shape.allow` list; unknown names are rejected.
Allowances are narrow, reviewable semantic exemptions, not pattern-based
suppression. Source code has no production performance annotation and cannot
change code generation by asserting a desired result. Category definitions,
scanner boundaries, allowance semantics, and the post-CGen measurement point
are frozen. A change migrates shape filetests and all ports that cite the
affected category.

## Digest anchors

anchor-sha256: src/aot/xi_cgen.h 5edc7b4c5c67b6232610bfb0356d38b8eaa02dfdaa82a35ede8accab75f1e4ab
anchor-sha256: src/aot/xi_cgen.c 7e0f0f1b146a0737988266bb5752d4751145eda052971261020009fb0ab47c19
anchor-sha256: src/aot/xi_cgen_ctx_impl.inc.c 4299def31e4313978dedfda4ff099fbb32de24bb1fe59c6186b137df849bd158
anchor-sha256: src/app/cli/xcmd_verify.c f890d8419073137ec0bdc74595c555d3ffaf7bbb866a1b1432337e5e7df66fd2
