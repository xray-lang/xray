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
4. `xisa/xi/lowering.def` is the canonical target-implementation table. An AOT
   target entry is one generated `driver`, direct `consumer`, or explicit
   `reject`. A driver row with a live source selector additionally carries an
   `:aot-c-consumer` terminal witness; this records the source owner without
   replacing its generated dispatch driver. A consumer is valid only for
   `aot-c`; each terminal
   emission or storage owner binds its canonical repository path and C symbol
   to an operation-derived positive selector, a leading fail-closed guarded
   selector, one or more router-to-terminal call edges, or one source-backed
   structural category. Those bindings contribute canonical
   target coverage and backend legality but never enter a generated dispatch
   table or synthesize a stub. The current table contains 33 direct-consumer
   operations, 44 terminal owner bindings, six router witnesses, 36 exact
   terminal-emitter witnesses, six explicit selector-predicate witnesses, two
   explicit predicate-domain witnesses, one
   guarded selector, 13 activation-edge
   records covering 15 exact calls, three exact output sequences, and 11
   statement drivers. Routers are witnesses rather
   than owners. Router-to-owner and terminal-selector censuses are independent:
   a declared router that also emits terminal output is rejected as a second
   owner even when its canonical owner call remains present. `xi.phi` has
   exactly four structural owners for edge copies,
   synchronous storage, coroutine-frame storage, and coroutine-local storage.
   Backend rewrites and verifier-only operations remain illegal, and every
   special AOT operation requires the same explicit consumer authority. For
   every other operation, `aot-c` and `aot-c-stmt` are one canonical AOT
   support domain, so a reject in either form overrides every implementation
   in that domain. The normalized lowering coverage must exactly match
   `ops.def`; an `ops.def` target alone is not an independent legality owner or
   compatibility fallback. A recursive repository AOT-source census is
   discovery-only: it rejects undeclared selector, router, guarded-selector,
   activation, and structural behavior, but never promotes an unrelated C
   function to semantic authority. `lowering.def` remains the sole declaration
   owner; there is no inferred allowlist or second consumer registry. Both the
   compile closure and discovery census bind every repository component before
   reading it: POSIX uses descriptor-relative no-follow opens plus `fstat`, and
   Windows holds non-reparse-point component handles without write/delete
   sharing. A file or directory swapped to a link at the former check/open
   boundary is rejected rather than read through the alternate target. A
   governed selector cannot be stored in an initializer alias or passed through
   a macro or helper whose body performs operation selection; macro names are
   derived from their replacements rather than capitalization. A selector used
   only as an argument to a non-routing observation is not an inferred owner and
   does not fail the discovery-only census. The recursively captured validation
   domain contains only repository-local quoted includes resolved through the
   ordered repository `XRAY_COMMON_INCLUDES` roots; the build-tree generated
   include root is intentionally outside that domain, and any quoted include
   that would require it fails closed.
   Trigraphs, directive-token line splicing, comments, and the `%:` digraph are
   normalized before local-C includes enter the same canonical path gate. All
   non-newline C directive whitespace is recognized, while macro-expanded
   include operands are forbidden rather than interpreted by a second
   preprocessor. A
   frontend spelling may drift, but its semantic
   lowering must remain mapped to the same Xi meaning unless this contract
   changes.
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
    A declaration inside a cleanup body owns a distinct execution-local place in
    every statically emitted reason path. Captured outer bindings retain one
    promoted place; a sibling cleanup emission cannot reuse a cleanup-local place.
    Each emitted boundary has an arena-owned identity shared by its enter and
    leave, its frontier head, and the next remaining boundary with a strictly
    decreasing rank. A statically fatal body has no fabricated normal leave.
    Lowering records identities after the complete frontier exists. Verification
    checks record ownership before dereferencing it, then validates live marker
    membership and every relation. Cloning remaps complete frontiers and pairs;
    it never retains source-arena identity pointers. Cleanup edits participate in
    both value and CFG fingerprints and invalidate dependent ownership and effect
    evidence. These identity checks do not prove arbitrary cleanup CFG termination.
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
22. Every Xi operation projects one exact `observable_contract` source into the
    semantic-owner registry. Generic operations default to this contract; an
    explicit source-backed operation names its current canonical source. Regex
    literals do not define an Xi operation: post-analysis canonicalization
    rewrites them to the module-qualified source construction
    `regex.Regex(pattern, flags)`. A regex-specific opcode, backend rewrite, or
    native constructor is not a compatible form.

23. `xi.sum.inject` retains the closed optional's ordinal and consuming payload
    edge through backend preparation. The canonical Program maps it to variant
    construction. The retained bytecode and C consumers project the same
    contract into their own storage: None is a null tag or null pointer, while
    Some transfers the exact payload. Scalar zero remains distinct from None.
    C support is declared by its generated driver in `lowering.def`; backend
    verification is not relaxed and no replacement semantic operation is
    synthesized by C emission.

24. Module declaration metadata has one owner on `XiFunc.module_slots`.
    The declaration name, kind, source coordinates, exact type, const flag and
    export flag are published together during lowering. Optimization may remove
    uses but cannot change a declaration's identity or type. Exports project
    that record, and semantic detachment copies its types into the same owned
    snapshot as instruction and capture types. Module state cannot be inferred
    from a surviving load, store or optimizer literal table. The former
    parallel name, const and export-name arrays are retired.
    A data declaration marks its first shared-slot store explicitly; subsequent
    assignments and REPL replacement publication do not carry that fact. The
    verifier confines it to data slots in the module initializer, metadata
    cloning preserves it, and value/memory fingerprints detect changes. Constant
    folding learns a declaration value only from this initialization witness,
    never from an arbitrary store.
    An open generic class retains its declaration, fields, method identities
    and exports without a runtime class creation or publication. Concrete
    specializations retain their ordinary initialization. An emitted bytecode
    child records its exact parent Xi child ordinal until atomic IR attachment;
    emission order does not imply declaration identity. Unemitted template
    bodies remain owned by the Xi parent. Missing ordinary bodies, duplicate or
    invalid child bindings, and graphs below the Repped stage fail attachment
    before any ownership transfer.
    A derived class publication may carry exactly one parent declaration
    operand. Program elision requires the exact resolved parent class and
    inherited field count; a runtime object or an unmatched declaration is
    not an erasable parent carrier.

25. A panic catch defines one nonnullable `PanicInfo` instance owner. Both
    source catches and compiler-emitted cleanup handlers retain this exact
    type through Xi transformations; unknown, scalar and class-declaration
    types cannot stand in for the payload. The catch names a live local
    `XI_TRY` whose target is its block, or an exact local call,
    assertion, or integer division/remainder under `XI_CATCH_AUX_POINT_PANIC`.
    The latter has one receiver and one predecessor naming the producer's
    block. Xi establishes an incoming ownership frontier before that producer;
    dead prefix owners end before either outcome and live owners cross an
    ordinary edge. Lexical cleanup graphs are cloned and composed in Xi with
    explicit `END_TRY` operations, remapped boundary identities and error
    regions. Normalized panic blocks retain their exit reason across normal
    edges; entry blocks and ordinary successors cannot acquire it implicitly.
    Program consumes these facts without claiming a lexical cleanup graph for
    panic. Normal, business-error and panic outcomes remain distinct.
    A call with only a panic channel occupies an isolated block with an
    explicit normal continuation; it does not manufacture a business-error
    check. A suspended fallible call retains the unique ERR_CHECK in its
    resume block. Ownership balancing includes the declared panic and trap
    successors, preserving borrowed owners until the selected continuation
    and excluding references already transferred to a callee.
    The handler defines exactly one payload. Verification establishes pointer
    membership before reading the registration. Program projection consumes the typed result and must not
    recover its type or ownership from a backend's cleanup-edge inventory.
    Explicit parent constructor and method calls use the same constructive
    error-check decision as ordinary calls. A proven no-error parent call
    has no business-error check; a possibly throwing parent call retains it.
    This does not resolve its runtime receiver view or erase panic effects.

## Verification

verification-test: test_xi_cleanup
verification-test: test_xi_allocations
verification-test: test_xi_cleanup_lower
verification-test: test_xi_cleanup_clone
verification-test: test_xi_cleanup_integration
verification-test: test_xi_pipeline
verification-test: test_xi_emit
verification-test: test_xi_opt
verification-test: test_xi_lower
verification-test: test_xi_verify_ext
verification-test: test_module_graph
