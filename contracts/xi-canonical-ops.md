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

anchor-sha256: xisa/xi/ops.def d1dd57ba0ac2807dff0b5f88ead185d321e27b0f4bd813eafe5fd3559ee68fd9
anchor-sha256: xisa/xi/lowering.def eda366f279b32588277877704ce4c203dbfc726f059824bb37d2501f801c8087
