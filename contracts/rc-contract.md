# Ownership and RC contract

Status: frozen by task 220.

For every RC-managed value, including registered identity aliases:

- C1: a release must not precede a later use on the same path without a
  compensating retain. A mutable closure capture is represented by one
  first-class cell value: `cell.new` owns that RC object, `cell.get` borrows
  from it, `cell.set` stores a new owned payload, and every closure capture of
  it is an ordinary consume/retain path. Emitters and ARC may not synthesize a
  hidden cell, cell destination, or cell-specific release exception.
- C2: reference-count changes are path-balanced for local death, return
  transfer, and move-out; double release is invalid. An `XI_COPY` tagged
  `VALUE_CLONE` realizes source-level value semantics by allocating independent
  storage, so its result is a fresh +1 owner rather than the borrowed alias
  described by the ordinary `XI_COPY` opcode contract. A branch-control owner
  dies on its outgoing edges, after the terminator read. A distinct PHI owner
  transfer retains the PHI owner and drops the predecessor owner on that exact
  edge; a self-PHI loop edge carries the same owner and must not drop it.
  A source-level panic handler observes the ownership state at its `XI_TRY`
  registration point. An owner used in the protected body stays live through
  every matching `XI_END_TRY`, with a physical release on both the normal exit
  and the executable panic-handler path.
- C2a: every reference-capable function return publishes a complete ownership
  summary. `OWNED` is a fresh +1 result and may be consumed or dropped by the
  caller; `BORROWED_PARAM(n)` aliases parameter `n`; `BORROWED_STATIC` has
  non-local lifetime. A recursive source SCC reaches one fixed point before
  ARC uses any member summary. Native reference returns declare the same fact
  explicitly. Missing, mixed, dynamic, or foreign evidence is `UNKNOWN`: ARC
  may preserve such a result conservatively, but must never release it as an
  owned result merely because its runtime representation is reference-counted.
  A source-defined function may not publish `UNKNOWN`: when no stable borrowed
  provenance is proved, its ABI is exactly `OWNED` and the return edge retains
  a borrowed value or moves an existing local owner.
- C2b: a payload-enum constructor returns a fresh owned inline aggregate. The
  dedicated record-construction plan evaluates named initializers once in
  source order, then installs them in declaration slot order. The aggregate has
  no object header, but every active payload lane is an ordinary owner:
  aggregate retain/release visits those lanes, a consuming box transfers them
  into the box, a borrowed box retains them, and consuming unbox clears the
  wrapper lanes before releasing the wrapper. Runtime field-name lookup and a
  call-shaped constructor fallback are forbidden. In particular, an aggregate
  sent through a tagged `READ` call slot uses a borrowed box with independent
  payload owners; transfer slots use the consuming box. A statically resolved
  local call reads the callee's fixed-point return summary before falling back
  to callsite metadata; a backend may not discard aggregate ARC operations
  merely because the aggregate itself is unboxed.
- C3: a borrowed view's owner remains live through every view use, including
  uses flowing through PHI edges. BOX, UNBOX, CHECKTYPE, and reference-to-
  reference CONVERT representation adapters preserve borrow provenance;
  semantic conversions that allocate a new reference do not. An implicit
  error edge must never release a borrowed adapter as a callee-owned cleanup.
- C4: release placement is dominated by the owner definition; a non-dominating
  join cannot assign one predecessor's owner to all paths.
- C5: ownership metadata is explicit and consistent, and SSA users reference
  live definitions.
- C6: reclamation is reference counting alone — an object dies when its last
  strong reference is released, and nothing collects reference cycles at
  runtime. What a cycle costs is bounded rather than reclaimed: every physical
  coroutine owns one execution-local reclamation domain, and that domain
  disposes its complete residual object graph when the coroutine ends. The VM
  realizes the domain as a per-coroutine Region heap; hosted AOT realizes it as
  an execution arena that keeps ordinary RC for acyclic objects and owns only
  the residual graph at teardown. Publishing a shared or transferred root must
  detach its complete owned graph before the source domain can end. Thus an
  unreclaimed cycle leaks no further than the lifetime of the coroutine that
  built it. Only the MODULE_STATIC and SYNC_SHARED ownership domains, plus the
  main execution's own lifetime, can leak for the life of the process
  (CONST_SHARED is constructively acyclic: no forward references, publication
  requires an explicit move/copy of a until-then-unique graph, and there is no
  implicit freeze — see LANGUAGE_SPEC 16.3). Cycles are prevented statically
  (L0 type graph), broken explicitly with a `weak` field (L1), and capped by
  this boundary (L2); the development detector reports them — coroutine heaps
  per teardown, the shared domain at main-execution exit — and never reclaims.
- C7: root-execution and module-static objects may reference each other, so
  teardown is a three-stage lifetime barrier rather than an arbitrary heap
  ordering. Fixed-heap finalization runs every module-static destructor while
  root storage is alive; root-heap teardown then runs while all finalized fixed
  bodies and sticky headers remain addressable; fixed-heap reclaim frees those
  bodies only after root finalization is complete. Each stage is idempotent,
  allocation is closed as soon as fixed finalization begins, and no combined
  finalize-and-free shortcut may reintroduce order-dependent destruction.
- C8: a stack allocation is a function-frame extent, not an RC heap owner.
  Its logical ownership path starts at `ALLOC`, ends exactly once in `DESTROY`
  on every reachable function terminal, and cannot return, publish, move, or
  cross its function. A stack closure's physical capture cleanup is normalized
  to that same logical `DESTROY`; it is not an ordinary heap `RELEASE`.

`Array.reserve` borrows its receiver, consumes the scalar capacity value, and
returns an owned alias of that receiver under its stable intrinsic identity.
ARC and lowering do not reconstruct this ownership contract from a selector or
legacy builtin auxiliary string.

An exact scalar parse borrows its String receiver. A successful parse returns
an unowned scalar; a required failure publishes one owned
`NumberParseError` aggregate through the existing error channel; an optional
failure returns null and publishes nothing. A pending error is never
overwritten, and backend adapters cannot manufacture a second owner while
moving the typed error through `XI_ERR_CHECK`.

The independent verifier must not reuse ARC closure/alias implementation logic.
It runs after ARC insertion in every build and reports violations as ICEs with
the contract identifier and counterexample path. Per-pass deep verification may
be enabled explicitly; it does not replace the mandatory post-ARC run.

The verifier trusts frontend source-root loan evidence at lowering boundaries,
including every outer owner read by a live static cleanup block. A cleanup read
is late-bound: it loans the original source root from the `defer` registration
point until the owning lexical scope exits; it creates no argument snapshot,
capture, upvalue, or cell. After lowering has split one source root into
independently balanced SSA values, the IR cannot reconstruct their former alias
identity. The frontend must therefore reject any `move` or return of an owner
held by a live cleanup-read loan (`E0382`) before IR generation.

A call-bound `REF` projection may use a scalar copy-in/copy-out slot, but a
place-backed aggregate receiver is never carried as a borrowed `PLACE_LOAD`
snapshot across suspension. Copy-out reloads the aggregate from its stable
place after the callee outcome, so multiple field writebacks preserve one
another and static cleanup observes the reason-appropriate field values.

A reference-capable `PLACE_LOAD` is a borrowed snapshot, not a frame owner.
Identity copies, representation adapters, borrowed projections, PHIs, SELECTs,
and receiver aliases preserve that provenance and may not hide it across a
proven suspension point. A retrying suspension operation may hold the exact
borrow only as one of its evaluated retry operands. Otherwise the continuation
must reload from the stable place after resumption or cross the boundary with
an explicit `VALUE_CLONE`; scalar place loads contain no reference and remain
ordinary by-value snapshots. A scoped existential READ reborrow is a separate
explicit Program contract whose unique owner is carried at the caller
safepoint, not an implicit exception for a place-load derivative.

Trust premise. C1-C5 verify the path balance of the ARC INSERTION RESULT: the
verifier consumes the same owned/borrow classification the inserter consumed,
so a systematic upstream misclassification in xi_own (an owned use read as a
borrow) would satisfy the contract and still be wrong. That layer is guarded by
different assets — the VM/AOT differential suite and the ASan corpus — not by
this one. A contract names what it proves; this line names what it does not.

## Verification

verification-test: test_xi_arc_verify
verification-test: test_fixed_heap_teardown
verification-test: test_rc_container_release
