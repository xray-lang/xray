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

anchor-sha256: xisa/xi/ops.def 6f2bd51b3f0d05df0daffce0ac2ab20959b2dadc17c4f16da0493a585a70e07e
anchor-sha256: xisa/xi/lowering.def 3af6584f78bcd752cb1af5681b016461b1d774b102303b037604513d74de3ede
