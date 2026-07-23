# Zero-cost residue contract

Status: re-frozen by task 237.

Backend shape requirements in a `xray verify --contract` asset inspect emitted
function bodies after optimization and CGen. They do not rewrite code. Residue
categories are:

- R1: non-whitelisted runtime-helper calls.
- R2: heap allocation.
- R3: pending-error checks.
- R4: bounds-panic branches.
- R5: tagged `XrValue` box/unbox traffic.
- R6: aggregate/native-vector lane round trips.

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
anchor-sha256: src/aot/xi_cgen.c b51664e97f2af6c8269619a4d01629543e6e57fff4276777670c4b028fa0e005
anchor-sha256: src/aot/xi_cgen_ctx_impl.inc.c 4299def31e4313978dedfda4ff099fbb32de24bb1fe59c6186b137df849bd158
anchor-sha256: src/app/cli/xcmd_verify.c d8e373d7b1d8023015b4afe6b1260b0f880c76e6759679f6376f20ad74a1effa
