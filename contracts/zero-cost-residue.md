# Zero-cost residue contract

Status: frozen by task 220.

`@zero_cost` checks emitted function bodies after optimization and CGen. It
does not rewrite code. Residue categories are:

- R1: non-whitelisted runtime-helper calls.
- R2: heap allocation.
- R3: pending-error checks.
- R4: bounds-panic branches.
- R5: tagged `XrValue` box/unbox traffic.
- R6: aggregate/native-vector lane round trips.

The default allowance set is empty. A source annotation may name explicit
categories in `allow:`; unknown names are rejected. Whitelists are narrow,
reviewable semantic exemptions, not pattern-based suppression. Category
definitions, scanner boundaries, allowance semantics, and the post-CGen
measurement point are frozen. A change migrates shape filetests and all ports
that cite the affected category.

## Digest anchors

anchor-sha256: src/aot/xi_cgen.h e048903cca587a4dae6de3df312a86f16cdec3572bb492490fe92d9f71dc3991
anchor-sha256: src/aot/xi_cgen.c 37b042e3b91f26a9ec774741aa07909366567b2e6928b802599e9571c20d3842
anchor-sha256: src/aot/xi_cgen_ctx_impl.inc.c f3e49f77dfcf8517fa11652da16e2ce71ec895d069e40c469787d8763589b1b4
