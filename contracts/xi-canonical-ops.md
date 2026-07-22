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

anchor-sha256: xisa/xi/ops.def 7cfc96685221e5c1605962a028da36b6db4b6c5c66caaf02b94d039a2530efed
anchor-sha256: xisa/xi/lowering.def b2edb8105755b7a2eff9d14734f92501e61f42507f83bac6e5826d777bcf8ea6
