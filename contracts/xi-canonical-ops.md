# Xi canonical operation contract

Status: re-frozen by task 237.

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
6. Raw storage becoming a typed value is represented by an explicit canonical
   Xi operation with initialization evidence. It must not be reconstructed from
   a cast, helper spelling, or backend pattern.

## Digest anchors

anchor-sha256: xisa/xi/ops.def 033150dadde24cbb4ef91df3d741807b1f79843d36db9c6159f3c52f798ce33a
anchor-sha256: xisa/xi/lowering.def 2d6c3d382c90e6d77fbfa02a5a932d4d2320ee35e81207a7bedfef9c8aa9a0a9
