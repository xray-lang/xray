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
  transfer, and move-out; double release is invalid.
- C2a: every reference-capable function return publishes a complete ownership
  summary. `OWNED` is a fresh +1 result and may be consumed or dropped by the
  caller; `BORROWED_PARAM(n)` aliases parameter `n`; `BORROWED_STATIC` has
  non-local lifetime. A recursive source SCC reaches one fixed point before
  ARC uses any member summary. Native reference returns declare the same fact
  explicitly. Missing, mixed, dynamic, or foreign evidence is `UNKNOWN`: ARC
  may preserve such a result conservatively, but must never release it as an
  owned result merely because its runtime representation is reference-counted.
- C3: a borrowed view's owner remains live through every view use, including
  uses flowing through PHI edges. BOX, UNBOX, and CONVERT representation
  adapters preserve borrow provenance; an implicit error edge must never
  release such an adapter as a callee-owned cleanup.
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
  built it. Only MODULE_STATIC, CONST_SHARED, and SYNC_SHARED domains, plus the
  main execution's own root domain, can live for the process lifetime. Cycles
  are prevented statically (L0 type graph), broken explicitly with a `weak`
  field (L1), and capped by this backend-neutral boundary (L2); the development
  detector reports them and never reclaims. See LANGUAGE_SPEC 16.3.

The independent verifier must not reuse ARC closure/alias implementation logic.
It runs after ARC insertion in every build and reports violations as ICEs with
the contract identifier and counterexample path. Per-pass deep verification may
be enabled explicitly; it does not replace the mandatory post-ARC run.

The verifier trusts frontend source-root loan evidence at desugaring boundaries,
including `defer` call snapshots and block captures. After lowering has split one
source root into independently balanced SSA values, the IR cannot reconstruct
their former alias identity. The frontend must therefore reject any `move` or
return of an owner held by a live `defer` loan (`E0382`) before IR generation.

## Digest anchors

anchor-sha256: src/ir/xi_arc_verify.c b3ea18ee26e32a7f176cd843aa52d67c7321929812f1d10ac43c89f5e900d9e0
