# Ownership and RC contract

Status: frozen by task 220.

For every RC-managed value, including registered identity aliases:

- C1: a release must not precede a later use on the same path without a
  compensating retain.
- C2: reference-count changes are path-balanced for local death, return
  transfer, and move-out; double release is invalid.
- C3: a borrowed view's owner remains live through every view use, including
  uses flowing through PHI edges. BOX, UNBOX, and CONVERT representation
  adapters preserve borrow provenance; an implicit error edge must never
  release such an adapter as a callee-owned cleanup.
- C4: release placement is dominated by the owner definition; a non-dominating
  join cannot assign one predecessor's owner to all paths.
- C5: ownership metadata is explicit and consistent, and SSA users reference
  live definitions.
- C6: reclamation is reference counting alone — an object dies when its last
  strong reference is released, and nothing collects reference cycles at
  runtime. What a cycle costs is bounded rather than reclaimed: every coroutine
  owns its heap and that heap is released whole when the coroutine ends, so an
  unreclaimed cycle leaks no further than the lifetime of the coroutine that
  built it. Only the MODULE_STATIC, CONST_SHARED, and SYNC_SHARED ownership
  domains, plus the main execution's own lifetime, can leak for the life of the
  process. Cycles are prevented statically (L0 type graph), broken explicitly
  with a `weak` field (L1), and capped by this boundary (L2); the development
  detector reports them and never reclaims. See LANGUAGE_SPEC 16.3.

The independent verifier must not reuse ARC closure/alias implementation logic.
It runs after ARC insertion in every build and reports violations as ICEs with
the contract identifier and counterexample path. Per-pass deep verification may
be enabled explicitly; it does not replace the mandatory post-ARC run.

## Digest anchors

anchor-sha256: src/ir/xi_arc_verify.c b3ea18ee26e32a7f176cd843aa52d67c7321929812f1d10ac43c89f5e900d9e0
