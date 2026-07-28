# Zero-cost residue contract

Status: re-frozen after fixed-width x86 SIMD values became first-class CGen
expressions across module-isolated feature islands. Cross-module declarations
retain ordinary internal linkage without promising unavailable force-inline
bodies, while generated target attributes and the W1-W4 verifier remain
mandatory. The xxHash KAT, source/generated-C/assembly shape gates, and focused
CGen fixtures were rerun; residue categories and allowance semantics are
unchanged.

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

anchor-sha256: src/aot/xi_cgen.h f830e12e06f1cc4934c368144e4e79acda88b7c5a8b3130bbbfbb8627842c434
anchor-sha256: src/aot/xi_cgen.c 515bfbb5cd3c9e547cfbe89e1c1493efeda5566d292a5ca5e74e059fa9c8b01d
anchor-sha256: src/aot/xi_cgen_ctx_impl.inc.c dad9f53f56cf97455681e906ce3316bbc1ca52bc485211f7ee7dc98da5574d3c
anchor-sha256: src/app/cli/xcmd_verify.c f890d8419073137ec0bdc74595c555d3ffaf7bbb866a1b1432337e5e7df66fd2
