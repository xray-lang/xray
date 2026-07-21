# Xi canonical operation contract

Status: frozen by task 220.

1. `xisa/xi/ops.def` is the canonical operation table. Opcode semantics,
   effects, result ownership, and operand ownership are generated from it.
2. Every operation declares `:own-use` explicitly. Missing ownership metadata
   is a generator error; no consumer may supply a default guess.
3. `borrow`, `consume`, `pass`, `stored-value`, and `method-args` retain the
   meanings checked by the ARC verifier and used by ARC insertion.
4. `xisa/xi/lowering.def` is the canonical generated-lowering dispatch table.
   A frontend spelling may drift, but its semantic lowering must remain mapped
   to the same Xi meaning unless this contract changes.
5. Changes migrate generated-file sync tests, Xi verifier/optimizer tests,
   backend differential cases, and any affected AOT shape evidence.

## Digest anchors

anchor-sha256: xisa/xi/ops.def 6b08cce19b5a208cfc30e0695c7f476f558cfece087fec10fa3425f114af4d06
anchor-sha256: xisa/xi/lowering.def 62a555d8accc8d1b30813c9ece540e774dc2a6f54b12efe055af1343d5f71fd6
