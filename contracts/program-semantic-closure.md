# Program semantic closure identity contract

This contract freezes the target-neutral, pre-Xi semantic authority used by
the bounded executable families and the complete reachable source-module graph. PSC itself contains no layout,
storage, ABI, TargetProfile, executor recipe, or standalone artifact codec.
Its verified rows may enter Xi and then the pointer-free SemanticPlan/XSM
projection defined below; they never imply TargetPlan admission on their own.

1. ProgramSemanticClosure schema v8 has one mutable builder, one explicit
   `XrProgramSemanticFamily`, bounded module/dependency/type/type-field/
   function/function-parameter/call tables, and a typed failure kind. The
   builder canonicalizes all rows before freeze and cannot be reopened after
   verification or any terminal failure. Family is durable authority; table
   cardinality is not interpreted as a capability bit.
2. Every module row carries an exact stable module identity and complete source
   and export fingerprints. An opaque dependency names two present modules, a
   nonzero contract fingerprint, and no typed binding fields. The scalar
   module-graph family instead requires one selective-function dependency with
   an exact import locator, exported declaration/function identities, nonzero
   resolver binding, and independently reconstructable contract. The dependency
   graph is acyclic and every module is reachable from the canonical
   entry/export root module set. `SOURCE_MODULE_GRAPH` instead carries one
   exact source edge for every resolver-produced dependency edge, backed by a
   real import or re-export locator. Its resolver binding, dependency contract, module source
   fingerprint, and whole-source export fingerprint are independently
   reconstructable; its function, type, parameter, and call tables are empty.
3. Type rows are explicitly `OPAQUE`, registry-owned `EXACT_SCALAR`,
   `LEAF_VALUE_AGGREGATE`, or `LEAF_VALUE_PRODUCT`. Opaque rows retain
   producer-supplied nonzero shape and ownership fingerprints. Typed rows require exactly the frozen
   nonnullable, nongeneric, value, pointer-free flags and supply zero
   fingerprints; PSC derives their shape and trivial copy/move, no-drop,
   no-root ownership fingerprints. A leaf aggregate contains a nonempty,
   gap-free, declaration-ordered field range whose children are already-present
   exact-scalar rows. A leaf value product uses the same ordered child domain
   but has an empty declaration locator: its declaration and instance identity
   are reconstructed from module identity, source fingerprint, arity, ordinal,
   and member TypeIds rather than a synthetic class, name, or source locator.
   Dense indexes, insertion order, names, target profiles,
   layouts, and ABI facts never enter a type identity.
4. Every function row binds its exact module, declaration and concrete instance,
   a complete semantic signature/effect/capability contract, one complete
   1-indexed exclusive-end declaration locator, an optional typed return row,
   and a gap-free ordered parameter range. Typed parameters bind exact type,
   declaration ordinal, and language parameter mode. Entry/export flags are
   explicit; reserved bits, duplicate declaration locators, incomplete types,
   or parameters outside the selected family fail closed.
5. A resolved-call row binds the complete policy and one stable callsite to
   exact present caller/callee functions and a complete call-contract
   fingerprint. Scalar module-graph calls additionally carry the same nonzero
   resolver binding as their selective dependency; every other family carries
   zero. Its source kind and complete locator participate in the closed-world
   evidence. A cross-module call requires an exact dependency; every function
   and module must be reachable from an explicit entry/export root.
6. Freeze hashes schema v8, family, complete policy, every canonical row and
   locator, then derives `XrGenerationClosureId` from the full closure
   fingerprint. The independent verifier reconstructs typed fingerprints,
   identities, ordered type-field and function-parameter domains, graph
   closure, canonical order, fingerprint, and generation identity without
   calling the builder. Re-signing only the aggregate PSC/GCI cannot conceal a
   stale or contradictory typed inner join.
7. Canonical source identity has one domain- and length-framed owner.
   `xr_source_semantic_module_authority` derives module and empty-export
   authority from the canonical module spelling and installs the separately
   supplied complete source fingerprint. `xr_source_semantic_callsite_identity`
   binds source fingerprint, module identity, caller declaration, and exact
   locator. The same owner derives canonical scalar-i64 export and resolver
   identities from verified module/source/export, import-locator,
   declaration/function, signature, effect, capability, return, and
   `READ`-parameter facts. The independent PSC verifier reconstructs those
   formulas without calling the owner. AST node ids, pointers, filesystem
   spellings, names, dense indexes, target representations, and ABI selections
   are not durable identity inputs. The same owner exposes the fixed
   source-module-graph policy, derives each module's whole-source export
   fingerprint from module/source authority, and derives each source edge's
   resolver binding and dependency contract from both complete module rows plus
   its exact locator. The independent verifier duplicates these formulas.
   The source-module-graph family admits one through 256 analyzed source
   modules and zero through 4096 resolver-produced edges. It requires a DAG
   with exactly one zero-indegree entry and complete reachability. Publication
   validates the graph's topological inventory and each real top-level import
   or re-export through the resolver; it never selects an edge by module name
   or synthesizes per-module plans. The canonical positive case is
   `entry -> csv -> text` with three module rows and two dependency rows.
   Re-signed locator, source, resolver-binding, or dependency-contract drift
   fails PSC verification. This family grants no Xi, SemanticPlan, TargetPlan,
   XTP, VM, AOT, or generated-C authority; those consumers remain fail-closed
   until a complete module-set projection is implemented.
8. The scalar source family remains exactly one memory module, two sealed,
   nongeneric, nonsuspending functions and one direct `i64 -> i64` call. Its
   verified owned analyzer snapshot projects a scalar PSC with no type or
   parameter rows. The shared target-neutral scalar semantic owner supplies the
   exact signatures, pure effects, and call contract; opaque fingerprints are
   never interpreted as equivalent authority.
9. The leaf-value aggregate source family is exactly three top-level
   declarations: one nonexported, nonpacked, unaligned, nongeneric value struct
   and two nonexported pure functions. The struct is nonnullable, has no base,
   interface, method, overlay, static/weak field, or pointer-bearing field; all
   fields are exact scalars. The entry is nullary, the callee has one `READ`
   parameter, both return that exact struct, and exactly one resolved direct
   call transfers one struct argument. The canonical first KAT is
   `Pair{i64 left,i64 right} -> swap(Pair) -> Pair` plus a nullary `root` that
   makes the unique call.
10. A source outside every bounded structural predicate is `UNSUPPORTED` and
    publishes no partial authority. Once a predicate has matched, incomplete
    module identity, source fingerprint, resolved call, source locator, type,
    effect, or ownership evidence is `INVALID`; allocation exhaustion is a
    distinct resource failure. Neither condition is converted into a looser
    family, default identity, fallback, alias, or compatibility path.
    The additional leaf-value-product family is an explicit internal
    source-backed publication: exactly three nullary pure functions return
    `[i64, i64, u8, i64, i64, i64]`; two distinct same-module entry callers each
    make one direct-local call to the third function. Its independent PSC
    verifier requires exactly three type rows, six ordered member rows, three
    functions, two calls, one shared callee, and complete caller coverage. Xi may
    consume only that explicitly supplied PSC and replaces only its PSC-bound
    tuple construction/projection nodes with canonical value-product proof
    operations. The independent Xi verifier rechecks all six ordinals, the `u8`
    member, both caller/callee joins, exact operation coverage, and an
    authority-free module initializer. Ordinary TypedProgram publication and
    SemanticPlan and schema-55 TargetPlan preserve the exact six-member layout,
    two caller-storage calls, and ordinal-bound instructions. Typed VM and the
    hosted-fragment C projection execute only after independent TargetPlan
    verification grants the leaf-value-product family. Ordinary product
    execution, legacy per-module C plans, product `EXECUTABLE` artifacts, and
    freestanding product emission remain fail-closed.
11. Only the scalar family builds `XrScalarCallDecision`. It requires the exact
    verified scalar PSC/GCI/TargetProfile and freezes native ABI,
    `DIRECT_LOCAL`, `STATIC_DIRECT`, `I64`, register-only argument/result, and
    explicit absence of adapter, cleanup, error channel, suspension, and extra
    capability. The aggregate family must carry no CallDecision or
    TargetProfile at this stage.
12. A frozen verified closure is immutable and reference counted. Retain rejects
    collecting, failed, unverified, released, or saturated authority. Failed
    Xi transfer consumes nothing. XiModule owns one closure reference and, only
    for scalar, its own decision copy plus the exact profile reference; module
    destruction releases every installed owner exactly once.
13. Xi lowering consumes one `XiProgramSemanticInput` and mechanically joins
    functions/call through complete PSC locators before any name, member, body,
    or shape resolver. Xi functions retain PSC function row indexes and, for a
    typed family, return-type row indexes; parameters, phis, and values retain
    the exact applicable PSC type-row index or canonical `NONE`;
    the covered call retains its PSC call row and exact source locator. No PSC
    row pointer or duplicated stable identity is cached in Xi. Exact scalars
    retain the closed scalar-registry join. A leaf aggregate first resolves the
    expression type's analyzer class to one local `XiClassData`, then reads that
    declaration's already-bound `psc_type_index` and exact PSC locator; aggregate
    field shape only verifies eligibility after this join and never selects a
    row.
14. The independent Xi verifier dispatches on family. Scalar rechecks the
    decision/profile and exact i64 contracts. Leaf aggregate requires exactly
    one aggregate type, two functions, one call, no target facts, exact
    return/parameter/call bindings, and exact PSC metadata on every function
    return, parameter, phi, and value. Bound/unbound mixtures, wrong rows,
    locator drift, extra execution contracts, or post-optimization mutations
    fail closed. The verifier independently reconstructs the same unique local
    declaration/PSC join instead of trusting the publisher's annotation or
    scanning same-shaped PSC rows.
15. SemanticPlan schema 44 and program-provenance schema 4 admit the bounded
    scalar, leaf aggregate, leaf value-product, and graph families only after
    PSC-to-Xi verification. The plan stores pointer-free
    PSC schema/family/fingerprint/GCI/counts, its exact module row, and typed
    bindings; it retains no
    analyzer, AST, PSC pointer, TargetProfile, CallDecision bytes, callback, or
    executor-private fact. Scalar keeps zero type bindings; leaf aggregate
    carries the exact scalar and aggregate type rows plus ordered field rows.
    The graph family publishes one plan per Xi partition: the producer has one
    exact source-function export and no dependency, while the entry binds the
    ordered dependency, resolver identity, program call, and source-export
    target without pretending that the remote function is local.
16. Program type bindings join stable PSC type identity and row to one semantic
    type, kind, exact-scalar id, flags, and field range. A leaf-aggregate
    binding also freezes the unique semantic source-class identity; the
    external verifier joins that identity and the complete source-class record
    back to the exact PSC-bound Xi module declaration. Field bindings join
    owner/child PSC identities and rows to the semantic child type and
    declaration ordinal. Function bindings preserve PSC flags, including
    `ENTRY`. Single-module call bindings join stable call/callsite/caller/callee
    identities to one semantic operation and exact direct-local target. The
    graph entry instead binds its external operation through the exact program
    dependency, resolver, `SOURCE_EXPORT`, and callee stable identities, with
    no invented local target function. Public accessors are
    borrowed indexed views into the frozen plan, not a second authority or
    cache. Before type-table freeze, every supported aggregate `XrType` joins
    its unique source class to `XiClassData.psc_type_index` and the stable
    source-class identity; that declaration row is the only identity owner for
    return, parameter, construction, and call-result sites. Xi type-row
    annotations are checked against this join but never supply a missing row.
    Xi ownership and SemanticPlan parameter/result ownership read this verified
    declaration/PSC row rather than deriving a semantic conclusion from those
    annotations. The leaf type's builtin id and scalar carrier are fixed to
    `NULL`/`NONE`; a live type claiming a builtin or scalar carrier is rejected
    instead of minting another canonical key.
    A missing or ambiguous class, zero or different PSC row, foreign same-shape
    class, nullable/const/generic modifier, duplicate binding, reordered row, or
    mismatched join fails closed.
17. XSM exact-version encoding serializes counts and provenance followed by all program
    type, type-field, function, dependency, and call bindings. Their counts participate in
    decoder storage/payload budgets; every field participates in the
    SemanticPlan fingerprint. Decode requires schema 44, exact bounds and
    payload digest, reconstructs the frozen plan, and reruns generic semantic
    verification. A dependency-bearing graph entry is accepted only by the
    module-set decoder with the exact ordered producer plan; the zero-dependency
    producer remains independently decodable. Round-trip is byte deterministic;
    hostile count, range,
    ordinal, stable type or source-class identity, family, flag, reserved,
    fingerprint, or join mutations fail closed even when outer framing is
    otherwise valid.
18. The implemented execution boundary closes source -> PSC v8 -> Xi -> SemanticPlan 44 ->
    XSM for both single-module families and the bounded product graph. The graph
    path lowers two exact Xi partitions, verifies the complete resolved module
    set, verifies producer/entry SemanticPlans as one dependency set, and only
    then builds one schema-55 program TargetPlan from the complete canonical
    SemanticPlan module set. The plan has one program-graph row, two module
    partitions over global rows, one aggregate semantic fingerprint, and the
    exact `PROGRAM_DIRECT`/`CALL_DIRECT_I64` call and argument authority. Only
    after independent TargetPlan verification may per-module summary cache
    authority publish. AOT direct-call refinement may derive one lower binding
    from that same plan by resolving both global functions through their owning
    partitions and binding the unique `CALL_DIRECT_I64` row plus stable program
    function identities. `PROGRAM_DIRECT` cannot become a refusal or a legacy
    path. This grants graph TargetPlan, exact XTP materialization, bounded typed
    VM execution, bounded AOT lower-binding authority, and an exact program
    C-emission binding from global TargetPlan rows to unique PSC-bound Xi nodes.
    For the exact two-module scalar graph, that single binding also freezes the
    complete C value and ABI rows for caller, callee, and module initializers,
    and product CGen mechanically consumes it through native execution. It also
    grants the installed public `XrProgram` load/execute/unload facade backed
    by the same plan, live manifest, decoded cache, program fingerprint,
    module-set fingerprint, and GCI. It does not grant container, coroutine,
    another graph family, dynamic reload, concurrent unload, or general
    product-activation authority. No consumer may skip,
    stitch per-module plans, fall back, or reconstruct missing facts from names,
    function bodies, tags, local semantic indexes, or legacy readers.
19. The first single-module TargetPlan consumer admits only the exact
    leaf-aggregate family from the public SemanticPlan program provenance and
    type, field, function, and call bindings. It projects those rows into the
    existing TargetPlan schema rather than creating an aggregate-only plan.
    Layout is derived from the verified target profile and declaration ordinals;
    the canonical native `Pair<i64,i64>` case is 16-byte size, 8-byte alignment,
    and field offsets 0 and 8. The direct-local argument is `VALUE/READ`, caller
    and callee use one aggregate representation, and the result uses caller
    storage with no ownership transfer or adapter. The independent Target
    verifier reconstructs every program-to-semantic-to-target join. Missing or
    mutated bindings fail closed and cannot fall back to a SemanticPlan shape
    helper, Xi operation, source name, or function body. The projection by itself grants no VM, AOT,
    or C-emission execution authority; execution additionally requires the exact
    independently verified leaf instruction group and consumer gates.
20. The bounded W3 AOT consumer joins a Xi function to that execution authority
    only through the owning module's frozen PSC v8 and
    `XiFunc.psc_function_index`: the selected PSC function identity and locator
    must match one SemanticPlan 44 program-function binding, which must in turn
    match the verified TargetPlan semantic provenance and fingerprint. A mutable
    `XiFunc.semantic_plan` pointer is optional and, when present, can only confirm
    the same identity. Missing, duplicate, foreign same-shape, or re-signed
    mutations fail closed; no name, body, dense index, legacy value plan, or call
    resolver may reconstruct the join. PSC remains target-neutral: this rule
    governs only how a downstream executable consumer reaches TargetPlan
    authority and adds no W4 family.
21. The bounded source product predicate is exactly two resolver-admitted
    source modules and one dependency edge. The dependency module contains one
    exported, nongeneric, nonsuspending, pure unary `i64 -> i64` function; the
    entry contains one selective import and one nonexported pure nullary `i64`
    function whose unique resolved call targets that exact export with one
    exact `i64` argument. Publication independently checks the imported member,
    export-table symbol, and call-use typed facts, then reconstructs the same
    exported declaration from dependency module/source authority, declaration
    locator, and sealed signature; pointer equality and analyzer-local symbol
    ids never select the target. It emits only
    `SCALAR_MODULE_GRAPH_DIRECT_CALL`: two module rows, one typed selective
    dependency, one exact-i64 type, two typed functions with one `READ`
    parameter, one cross-module call carrying the same resolver binding, and
    one GCI. The dependency export fingerprint, dependency contract, resolver
    binding, call target, effect/capability, and entry empty-export fingerprint
    are independently rebuilt and rejoined. Topological enumeration order
    cannot change canonical identity. Missing or contradictory resolver,
    locator, export, dependency, contract, or foreign same-shaped source
    authority is `INVALID`; it cannot become GENERAL, a zero PSC, a name/path
    guess, allowlist, fallback, or compatibility path. A standalone verifier
    has no source bytes and therefore validates the frozen locator joins, not
    the authenticity of an attacker-recomputed self-consistent source span.
    The verified PSC/GCI is partitioned into producer and entry Xi modules. The
    independent Xi module-set verifier rejoins module/function rows, callable
    export type, resolver, import slot, dependency, call, callee, and exact pure
    i64 signatures. The SemanticPlan module-set verifier then rejoins root/local
    attachments and producer/entry plan authority. Per-partition SemanticPlans
    and their XSM round-trips close the same graph authority. Target construction
    consumes the full canonical plan set in `program_module_row` order while the
    entry fragment separately carries only its ordered direct dependency.
    It emits one verified schema-55 TargetPlan with global functions, slots,
    values, instructions, calls, arguments, debug facts, layouts, extents, and
    capabilities; module partitions are bounded pointer-free views and are not
    independent plans. The cross-partition edge is exactly one
    `PROGRAM_DIRECT` call with one `VALUE/READ` i64 argument and one
    `CALL_DIRECT_I64` instruction. Builder and verifier independently bind every
    global row back to the owning local SemanticPlan row, require the exact
    program module set and aggregate fingerprint, and reject missing, extra,
    overlapping, reordered, foreign, or re-signed authority. The exact graph
    XTP materializer rebuilds the same plan from the full module set and rejects
    ordinary/graph shape substitution. The typed VM admits only the verified
    graph entry as an external root, executes the cross-partition call from the
    same plan's global rows, and rejects direct producer entry. Cold execution
    reruns independent TargetPlan verification; warm execution requires the
    generation-owned decoded cache bound to that same plan object, intact plan
    and program fingerprints, freshly recomputed canonical module set, exact
    GCI, complete generation identity, and verified global graph rows.
    Wrong-generation, foreign same-fingerprint, and mutated graph or partition
    authority fail closed. The cache has no fixed two-partition array and never
    builds, stitches, or translates per-module executable plans. The AOT lower
    refinement consumes that same plan and independently re-derives the global
    caller, callee, instruction, argument, and stable program-function symbol
    joins. A transient C-emission binding independently validates the same
    global partitions, functions, call, argument, and instruction, joins only
    through SemanticPlan program bindings and unique PSC row indexes, and derives
    deterministic C symbols from stable function identities. The binding owns
    complete value and function-ABI projections for caller, callee, and every
    module initializer, independently verifies the exact `GET_SHARED` callee
    carrier and its unique operand-zero use, and constructs no executable plan.
    Product CGen consumes only those stable-indexed views, elides that carrier
    from C materialization and legacy shared/static reachability. Required
    module initializers remain lifecycle roots, but only the caller is an
    ordinary product-body root; the callee is reached through the verified
    direct edge alone. It performs
    no module-name, local-index, per-module emission-plan, or legacy ABI lookup.
    The source product publishes deterministic module summaries, emits real C,
    and executes native cold/warm/dependency-edit/dependency-revert results
    42/42/43/42. This native closure remains exclusive to the exact bounded
    graph. Its separate installed public facade admits only the same exact
    two-partition/two-function/one-call/one-argument direct-`i64` graph; other
    graph shapes, containers, coroutines, dynamic reload, and general product
    activation remain unavailable.

The runtime standard-library source loader is an input boundary, not semantic
or target authority. Its generated source-module descriptor set follows the
same build feature gates as native factories and private-leaf binders; a source
file that merely exists in the repository cannot enable a disabled module.
Loading and atomically publishing such a module does not grant PSC,
SemanticPlan, TargetPlan, XTP, typed-VM, or AOT admission. Those stages still
require the independently verified program authority frozen above.

## Digest anchors

anchor-sha256: CMakeLists.txt 7591d3887c1724d44814e04eb20c7197ef10af333e75494b047bfef5c2b9bfb3
anchor-sha256: src/module/xmodule_graph.h feb99f1e6afea7a84a38379fe7ebddeec4338998ade7753489d2313dff2fc0d2
anchor-sha256: src/module/xmodule_graph.c 0feddd8fc10e769aa4b13ca824046c227ba6dbd0037dc2979788d14c7a94da5e
anchor-sha256: src/frontend/parser/xparse.c f3a9f5e4699fa00ef5653399e17754f71dac3cb2ca27e6029f89f62da75f0365
anchor-sha256: src/frontend/parser/xparse_decl.c 956af54b931477adfe03b61de2271af8951a2241a9e0fa8ec8875855be2a5b06
anchor-sha256: src/frontend/parser/xparse_import.c 6b82bda85a81a59c90d9ed04a71a86092525fc64b05d27d7e89ec10453003bec
anchor-sha256: src/frontend/analyzer/xanalyzer.h c12244d5565e4e0ceb65177b8bf24f08c7161b6145d6c5d41fe4555d03f74988
anchor-sha256: src/frontend/analyzer/xanalyzer.c ea9243e7fae24e07022423822848abf3060ddb444b86202e96f221d7e3f8bd61
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_call.c 9307147e728fe4048079ae1fce4f70050d2f1a64c1e84b3ef5e6cf6391f8b09c
anchor-sha256: src/frontend/analyzer/xa_typed_program.h f9ba2decb49ea560631bdb90a6504d509ee4503f83f36088d4a1aab83c659f62
anchor-sha256: src/frontend/analyzer/xa_typed_program.c c140dacd979291c08de4f5e25c74b0feb09a615d909d680bc3a9cb0cb487875a
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority.h beff336ef272509ea3a8a136b1cb85a53ef6dcb47730b1121e56e83b738741a4
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority_internal.h d26adf137848c56d923d52b7bd85439105bcae1a48de736f989fc64685bcea17
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority.c 57e0d760a7d12f1cc5763ba3768eba48ae6859356018c37b9353d3d1e329113a
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority_verify.c 19d2eab8e1b2b36e6ef2bdc6452e246a63a056704d7785cec8c5d25274900f40
anchor-sha256: src/frontend/analyzer/xa_program_semantic_closure.h a25c554ab9571613a8fd26185147595375490c1cc1c02ff26a831d99cd751c85
anchor-sha256: src/frontend/analyzer/xa_program_semantic_closure.c 5ae18976177eebe503e643644649517fa0c23d8c57aa38a0a5e3b131ce2382e2
anchor-sha256: src/plan/semantic/xr_program_semantic_closure.h a7b9d78946d077e7d7563f6f896b3fedcf0f781bf0182d94f0353d9a99fe2a41
anchor-sha256: src/plan/semantic/xr_program_semantic_closure_internal.h 006da5f6b47f362c7e0296f648638bb396db27d6bcd9df65f58ae9cde1d5b03e
anchor-sha256: src/plan/semantic/xr_program_semantic_closure.c 4443024edf4523d6c676147a77aa365bbe7ac96852eea1941038d1c5b24e165a
anchor-sha256: src/plan/semantic/xr_program_semantic_closure_verify.c 3120c7a0a497ef3e2cade1e5c2025fc9cc959eb0084ab4719719edef5c2f81e1
anchor-sha256: src/plan/semantic/xr_source_semantic_identity.h 5153e5d6a791b9d4b6ed83e914f380003a0c813128b1bc4fe890d606b8f4d2d3
anchor-sha256: src/plan/semantic/xr_source_semantic_identity.c 833ecf4f6e42ebe14fe6c17eae216118a482418e38e8ef2f82f3726ab622990c
anchor-sha256: src/plan/semantic/xr_scalar_call_semantics.h ea50939c04efe67bdd8c885f002275e12ecfb2863acf7bfe4596e6fe28d9a99c
anchor-sha256: src/plan/semantic/xr_scalar_call_semantics.c e7951de7ce36a6facece99304f2de817381b6e4051b2b66fd7b336bb1c66d493
anchor-sha256: src/plan/format/xr_xsm_decode.c b31bf1696bacd3b435ea1383da4f92df51bb6692c45f28e7d22ab829154db8f4
anchor-sha256: src/plan/format/xr_xsm_encode.c 35840e929f9e86086cd57790af43eb4df6b84060704eba9045bdc9b40f579f2c
anchor-sha256: src/plan/format/xr_xsm_schema.h f5e6d875255f73803545a9cf99450e6b140e6282ee19233048afd4e0ce41362b
anchor-sha256: src/plan/semantic/xr_semantic_builder.c 5e4f7e56f858cc23f93ff2bea86f8fd93cc8858e1e55d1ff1fdb126aafc35c6e
anchor-sha256: src/plan/semantic/xr_semantic_ids.h 0940af8bed7477c4974f094eed12d77e65f5369830a1425c3c49ccd1fc61f150
anchor-sha256: src/plan/semantic/xr_semantic_plan.c 0f78c911fd05636a4717ec9d4d0b8b5db3d8a669a5a680b367960cc8d7923d66
anchor-sha256: src/plan/semantic/xr_semantic_plan.h 102b975dbb921bc1cf4f6c5f6bc3c6f34d2a5656b3dc904970eb8e9fd64b4948
anchor-sha256: src/plan/semantic/xr_semantic_plan_internal.h fbe1eb29e08425a629dda4c281f7a681ab48512c599cae9b63b379f4db338d2e
anchor-sha256: src/plan/semantic/xr_semantic_verify.c 0e7210c12a41485da9ef904a7ff258a39ad5cb8d8d2f9d88a6dad139bbee2eee
anchor-sha256: src/plan/target/xr_scalar_call_decision.h 35d3f167734562525e36406f61ddb794f7308311453a29f5695aa3e24a1c8153
anchor-sha256: src/plan/target/xr_scalar_call_decision.c 642eff61fb0b06185d2aec4c0ce8d91919148de3d29d74a17e68bd1a5016b3fa
anchor-sha256: src/plan/target/xr_scalar_call_decision_verify.c 838f7adc60c3de52fda23ed25c07edb678a6274eb87d0deec2da033e701dca10
anchor-sha256: src/ir/xi.h ee962981bf6c73f01aa2b508ce38a71e4249572e71030eb3348ea569a4447b29
anchor-sha256: src/ir/xi.c 26d3ceab5e7653af4dde2c3af459dd68fc59b65d9824c667acf96a0812671960
anchor-sha256: src/ir/xi_module.h 2ee8794e21ef18162b5c045e795a62a952762829c19af775a7bb7de73a873eb7
anchor-sha256: src/ir/xi_lower.h 297d6165a47ac164a12df522091e970093fcc2ce35153378d5fd717019a8da72
anchor-sha256: src/ir/xi_lower.c 5c83d887833b6197c71328aa2f126d4a18bd8dfc318f73cdc9e745fd2b4408e7
anchor-sha256: src/ir/xi_lower_expr.c c2bbc5ac57a5d51cd3907315ac651a230b1626b0fa02fcecf4af3a7c2cf64a57
anchor-sha256: src/ir/xi_lower_stmt.c 49fafd4fe4f39ec34bb8b763d4cfe96d30efe3d8c71d881abbf693f054490a93
anchor-sha256: src/ir/xi_pipeline.c 385756c5c598a45b7caa4ebef4b88f2fcc744133dc72ecb390975d8edbd4b4db
anchor-sha256: src/ir/xi_program_semantic.h 090db1d96c7fe1d2102c24293669ea1bee060a390fa8bfcb9cc63985a8c4955e
anchor-sha256: src/ir/xi_program_semantic.c c31bd64d20c470df18e1a52f2ed19ee8bd0b778f5bc747e28cf7af1bb836f8ae
anchor-sha256: src/ir/xi_program_semantic_verify.c 0de956d180198ede7286f78e8eed70ec031b364b9e53f38ea19d26199fabaa6d
anchor-sha256: src/ir/xi_own.c d36d0c84c81224d366afa7971390af9b2a58e65f06fc95e29a25a38c74979d7d
anchor-sha256: src/ir/xi_program_semantic_plan.h c516db30bd0b7ec8fe0749bfd6fac4aecb8cfac217dad32f1954a7a66a7ee7ae
anchor-sha256: src/ir/xi_program_semantic_plan.c 4fbab5834aea89beebef20edd7607afc2f8e5c3c711e99f2a41d4d2c4f369eba
anchor-sha256: src/aot/emit_c/xr_c_program_emission.h 4668c1afc4791eb36f23eb585f6de9ce4a1311a20deebe8ae2ca355a90ac985e
anchor-sha256: src/aot/emit_c/xr_c_program_emission.c b21cb6814dad7e5d808cb97046dfa514d4242c6b6563c32cc2ac89c82734b076
anchor-sha256: tests/unit/plan/test_program_semantic_closure.c dd2818bab1ed859e62c5fcf6d5cec0a529b0b00a8851266a9fd8aec8053aa227
anchor-sha256: tests/unit/plan/test_scalar_call_decision.c 01a96bd0b8bf666d48bdf7f533873e290fa3ac2e2d266895baf43f68dcae9285
anchor-sha256: tests/unit/plan/test_semantic_plan.c 27ab3b82f661a015457638eec0c778eacdcd9f9b29ddc18f67242a03d258fa07
anchor-sha256: tests/unit/frontend/test_xa_program_semantic_closure.c 570675d165cc6cdd58bd82ca089cbf73a7a8106cfc5d8c66087350d13b93864e
anchor-sha256: tests/unit/frontend/test_parser.c d106777632742a1a1187c95c93c0515143ed4d61f4ed42105b0180377bda6cec
anchor-sha256: tests/unit/module/test_module_identity.c f2054fb4d6e514c740fc635ec4204031c589ba8ff1d0b1c377fab4f4d45f2cfe
anchor-sha256: tests/unit/ir/test_xi_program_semantic.c 5e2952fa1eee3c98ab8c64968a3224df90ed637fd3fb0f5ef25b4557f89d277c
anchor-sha256: tests/unit/ir/test_xi_pipeline.c 238976d247a3099fcc85c60a43f33c420075e75e599ae89bd5d083bd875b5f48
anchor-sha256: tests/unit/CMakeLists.txt 72cbc0b766d4298bfa96a0ad4dc39b568b24923768f43a41f2579bfa4d0e389e
anchor-sha256: src/aot/xaot_boundary.h e36d4576dbd11c6b321bb22d339a779820ed4962304bab20840a83b25c1085da
anchor-sha256: src/aot/xaot_boundary.c f12690bc5c41ec4989fc3a7465ecf3a5d0c6304eaaadc509e5b1d7a410076472
anchor-sha256: src/aot/xaot_bundle.c 889e0ec0ae231d634c279ff844097667eb47957515e5b87a8cd22db18f511993
