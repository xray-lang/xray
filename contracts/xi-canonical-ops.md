# Xi canonical operation contract

Status: re-frozen after the non-lowerable `xi.bounds.check` registry row and
its dead pass were removed. Bounds behavior belongs to the real
`xi.index.get` / `xi.index.set` family consumed by VM and AOT; there is no
compatibility opcode, reserved hole, or second bounds owner.

1. `xisa/xi/ops.def` is the canonical operation table. Opcode semantics,
   effects, result ownership, operand ownership, memory scope,
   synchronisation edge, and the unique language-semantic owner category are
   generated from it.
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
12. Mutable capture identity is represented by canonical `xi.cell.new`,
    `xi.cell.get`, and `xi.cell.set` values before ownership analysis. Weak
    field promotion/storage likewise uses `xi.weak.load.field` and
    `xi.weak.store.field`. No backend target attribute, lowering flag, hidden
    register table, or ARC exception may reconstruct either meaning.
13. Source `defer { ... }` lowers to explicit static cleanup regions delimited
    by `xi.cleanup.enter`, `xi.cleanup.leave`, and `xi.cleanup.err_check`.
    Registration is a program-point-sensitive control-flow frontier, and every
    edge crossing the owning lexical scope runs the active bodies in LIFO order.
    There is no Xi defer-push/pop/invoke operation, closure or callback
    representation, dynamic cleanup stack, or backend reconstruction. Panic and
    coroutine-cancellation paths dispatch to the same static regions.
14. Every Xi operation is listed exactly once in one of four owner groups:
    `declarative-primitive`, `shared-semantic-kernel`, `capability-provider`,
    or `generated-specialization`. Missing categories, missing operations,
    duplicate membership, and unknown operations are generator errors.
15. `xr_semantic_ops_gen.h` contains only target-neutral fields. Target support,
    C/native spelling, backend rewrite, and lowering policy remain outside the
    SemanticPlan registry. The independent registry verifier checks every row,
    and the SemanticPlan verifier reads arity, effects, result ownership,
    and operand ownership from this registry instead of backend metadata.
16. The operation-registry fingerprint covers every target-neutral contract
    field and owner category. It is part of the SemanticPlan fingerprint and
    the exact-version `.xsm` header. A missing or incompatible registry fails
    before artifact allocation or execution; there is no compatibility shim.
17. Observable owners have canonical 128-bit operation and owner IDs generated
    from `ops.def`. The generated JSON registry fingerprints production
    consumers and adapter bindings; VM, AOT, CGen, and runtime use the stable
    owner ID for migrated families instead of reconstructing an owner from a
    source or C spelling.
18. Source array indexing lowers directly to `xi.index.get` or `xi.index.set`.
    A standalone bounds guard that has no source lowering and no VM/AOT handler
    is not a canonical operation. Removing such a row compacts subsequent Xi
    opcode numbers and requires an exact schema cutover; consumers must not
    preserve a hole or translate the retired number.
19. `xi.tail.call` is a native operation in both VM bytecode and portable AOT
    C lowering. AOT must preserve the frozen opcode through prepare and dispatch
    it through the generated `xicgen_call` row; rewriting it to `xi.call`,
    accepting that rewrite as an alias, or selecting a backend from live names
    is not a supported lowering path.
20. Source assertions lower to the single canonical `xi.assertion` operation.
    Its owned typed auxiliary plan is deep-cloned, verified, dumped, hashed, and
    destroyed with the Xi value. VM and AOT consume the same plan and stable
    assertion owner identity. Retired assertion opcodes, spelling-based adapter
    selection, and shallow source-location pointers are not compatible forms.
21. `xi.value.product.construct` and `xi.value.product.project` are
    verifier-only proof operations for an exact PSC-bound anonymous leaf value
    product. Only the explicit product PSC finalizer may replace matching
    managed tuple nodes; arity, spelling, encoded identity, or an unbound tuple
    cannot select this family. These operations provide no TargetPlan, VM, AOT,
    or ordinary-product execution route by themselves.

## Digest anchors

anchor-sha256: xisa/xi/ops.def cd873e45558cba139d9675bcf9dce00223a514d3b4d811b494c55da65779de7d
anchor-sha256: xisa/xi/lowering.def b928743b1e7b3d0cc50bcf2d9779f4c755f189bcb0f6e710e7fa232bce7aa517
