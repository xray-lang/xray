# Xi canonical operation contract

Status: re-frozen after the xxHash work made raw pointer-backed slices an
explicit caller-proven unsafe view rather than a backend fallible operation.

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
7. Runtime SIMD selection is represented by
   `xi.target.simd.runtime-selected`; the operation reports compilation mode,
   while `xi.target.simd.bytes` remains the runtime CPU/OS width query in a
   dispatch build.
8. `xi.slice.from_ptr` requires explicit unsafe pointer-layout and owner-lifetime
   assumptions. Once the analyzer accepts that boundary, the operation is
   non-throwing in VM and AOT; a backend must not recreate a pending-error or
   bounds check that contradicts the caller-proven contract.

## Digest anchors

anchor-sha256: xisa/xi/ops.def eb2503ed38e863cfddaf6b3c9af3691b641cc4ce1e8c78d4d9636764041459eb
anchor-sha256: xisa/xi/lowering.def 3f497bdba9f651908ba7d7e5d500eb82806b0861244638a282b737eb6893fdf9
