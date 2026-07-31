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

The independent verifier must not reuse ARC closure/alias implementation logic.
It runs after ARC insertion in every build and reports violations as ICEs with
the contract identifier and counterexample path. Per-pass deep verification may
be enabled explicitly; it does not replace the mandatory post-ARC run.

## Digest anchors

anchor-sha256: src/ir/xi_arc_verify.c b3ea18ee26e32a7f176cd843aa52d67c7321929812f1d10ac43c89f5e900d9e0
