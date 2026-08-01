# Xi canonical operation contract

Status: re-frozen after every memory-effecting operation gained a mandatory
memory scope (`:tbaa-group`) and every synchronising operation gained its
language-level happens-before edge (`:sync`).

1. `xisa/xi/ops.def` is the canonical operation table. Opcode semantics,
   effects, result ownership, operand ownership, memory scope, and
   synchronisation edge are generated from it.
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
9. An operation that declares a `memory-read` or `memory-write` effect must
   declare which memory it touches via `:tbaa-group`. `none` means "touches no
   memory" and is rejected for such an operation: alias queries answer no-alias
   for it, so a store carrying `none` would stop killing loads. Unclassified
   memory is `top`; storage the operation itself allocates and nothing else can
   yet reach is `fresh`.
10. An operation that establishes a language-level happens-before edge
   (spec §16.9.2) declares it via `:sync`. Where the edge's strength is chosen
   at run time by an `Ordering` argument, the declaration is the strongest edge
   the operation can carry. Alias disjointness never licenses moving an
   ordinary memory operation across such an operation; that is decided by
   `xi_op_is_ordering_barrier()`, not by `xi_tbaa_may_alias()`.
11. A named SIMD shuffle pattern such as adjacent-lane exchange is preserved as
   a semantic Xi shape bit. Backends must not depend on packing every lane
   index into `aux_int`; explicit arbitrary shuffle lists fail closed when the
   canonical representation cannot carry their width.

## Digest anchors

anchor-sha256: xisa/xi/ops.def c6ed4ebdf26a291fffcceae52bd2ab3a0d3daf816f88e98e2a896987aa9e0732
anchor-sha256: xisa/xi/lowering.def e57b4b7cf84e3dadf3e03f39c68279c87b298820cf1cfa5e4b687826d2bc98b0
