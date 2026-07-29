# Xi canonical operation contract

Status: re-frozen after implicit error propagation gained late cold-edge ARC
operands, and raw-pointer loads plus ordinary slices were made explicit
borrowed views instead of accidental owners.

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
9. A named SIMD shuffle pattern such as adjacent-lane exchange is preserved as
   a semantic Xi shape bit. Backends must not depend on packing every lane
   index into `aux_int`; explicit arbitrary shuffle lists fail closed when the
   canonical representation cannot carry their width.

## Digest anchors

anchor-sha256: xisa/xi/ops.def 3c9676f5351ac63aeac0160f9e26a4c4a3bec9ac720426d06e8abe1fa294f30b
anchor-sha256: xisa/xi/lowering.def e57b4b7cf84e3dadf3e03f39c68279c87b298820cf1cfa5e4b687826d2bc98b0
