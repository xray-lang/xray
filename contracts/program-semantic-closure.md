# Program semantic closure identity contract

This contract freezes the target-neutral, pre-Xi semantic authority used by
the bounded single-module families and the first bounded source product graph. PSC itself contains no layout,
storage, ABI, TargetProfile, executor recipe, or standalone artifact codec.
Its verified rows may enter Xi and then the pointer-free SemanticPlan/XSM
projection defined below; they never imply TargetPlan admission on their own.

1. ProgramSemanticClosure schema v5 has one mutable builder, one explicit
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
   entry/export root module set.
3. Type rows are explicitly `OPAQUE`, registry-owned `EXACT_SCALAR`, or
   `LEAF_VALUE_AGGREGATE`. Opaque rows retain producer-supplied nonzero shape
   and ownership fingerprints. Typed rows require exactly the frozen
   nonnullable, nongeneric, value, pointer-free flags and supply zero
   fingerprints; PSC derives their shape and trivial copy/move, no-drop,
   no-root ownership fingerprints. A leaf aggregate contains a nonempty,
   gap-free, declaration-ordered field range whose children are already-present
   exact-scalar rows. Dense indexes, insertion order, names, target profiles,
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
6. Freeze hashes schema v5, family, complete policy, every canonical row and
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
   are not durable identity inputs.
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
15. SemanticPlan schema 42 and program-provenance schema 3 admit all three
    bounded families only after PSC-to-Xi verification. The plan stores pointer-free
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
    SemanticPlan fingerprint. Decode requires schema 42, exact bounds and
    payload digest, reconstructs the frozen plan, and reruns generic semantic
    verification. A dependency-bearing graph entry is accepted only by the
    module-set decoder with the exact ordered producer plan; the zero-dependency
    producer remains independently decodable. Round-trip is byte deterministic;
    hostile count, range,
    ordinal, stable type or source-class identity, family, flag, reserved,
    fingerprint, or join mutations fail closed even when outer framing is
    otherwise valid.
18. The semantic boundary closes source -> PSC v5 -> Xi -> SemanticPlan 42 ->
    XSM for both single-module families and the bounded product graph. The graph
    path lowers two exact Xi partitions, verifies the complete resolved module
    set, verifies producer/entry SemanticPlans as one dependency set, and only
    then publishes deterministic per-module XSM/cache authority. This does not
    grant graph TargetPlan, VM, native AOT, container, coroutine, or public-ABI
    execution authority. TargetPlan build and verification reject the graph
    family with `XR_TARGET_1001`; no consumer may skip, fall back, or reconstruct
    missing facts from names, function bodies, tags, or legacy readers.
19. The first TargetPlan consumer admits only the exact leaf-aggregate family
    from the public SemanticPlan program provenance and type, field, function,
    and call bindings. It projects those rows into the existing TargetPlan
    schema rather than creating an aggregate-only plan. Layout is derived from
    the verified target profile and declaration ordinals; the canonical native
    `Pair<i64,i64>` case is 16-byte size, 8-byte alignment, and field offsets 0
    and 8. The direct-local argument is `VALUE/READ`, caller and callee use one
    aggregate representation, and the result uses caller storage with no
    ownership transfer or adapter. The independent Target verifier reconstructs
    every program-to-semantic-to-target join. Missing or mutated bindings fail
    closed and cannot fall back to a SemanticPlan shape helper, Xi operation,
    source name, or function body. The projection by itself grants no VM, AOT,
    or C-emission execution authority; execution additionally requires the exact
    independently verified leaf instruction group and consumer gates.
20. The bounded W3 AOT consumer joins a Xi function to that execution authority
    only through the owning module's frozen PSC v5 and
    `XiFunc.psc_function_index`: the selected PSC function identity and locator
    must match one SemanticPlan 42 program-function binding, which must in turn
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
    and their XSM round-trips close the same graph authority before module-summary
    cache publication. TargetPlan, VM, and native AOT execution remain deliberately
    outside this family and fail closed at the Target boundary.

## Digest anchors

anchor-sha256: CMakeLists.txt 5890e07bb0689c9dbdb1b72616bde8f4481a719b58b87f8ad553830cab622407
anchor-sha256: src/module/xmodule_graph.h feb99f1e6afea7a84a38379fe7ebddeec4338998ade7753489d2313dff2fc0d2
anchor-sha256: src/module/xmodule_graph.c 1e6cd17cde6de2031f99308217f02c6c43a0f0df0ba4d3f461b83c87e1d3d6ed
anchor-sha256: src/frontend/parser/xparse.c f3a9f5e4699fa00ef5653399e17754f71dac3cb2ca27e6029f89f62da75f0365
anchor-sha256: src/frontend/parser/xparse_decl.c 956af54b931477adfe03b61de2271af8951a2241a9e0fa8ec8875855be2a5b06
anchor-sha256: src/frontend/parser/xparse_import.c 6b82bda85a81a59c90d9ed04a71a86092525fc64b05d27d7e89ec10453003bec
anchor-sha256: src/frontend/analyzer/xanalyzer.h c12244d5565e4e0ceb65177b8bf24f08c7161b6145d6c5d41fe4555d03f74988
anchor-sha256: src/frontend/analyzer/xanalyzer.c ea9243e7fae24e07022423822848abf3060ddb444b86202e96f221d7e3f8bd61
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_call.c d1c86a031eb0f9fdbbbacb2680e25bb9953b1dac3f50549035a9d1a01a8279a6
anchor-sha256: src/frontend/analyzer/xa_typed_program.h f9ba2decb49ea560631bdb90a6504d509ee4503f83f36088d4a1aab83c659f62
anchor-sha256: src/frontend/analyzer/xa_typed_program.c baa142bcb0f1841e2553d405204468c668e00782429ba32478e479af9d398680
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority.h beff336ef272509ea3a8a136b1cb85a53ef6dcb47730b1121e56e83b738741a4
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority_internal.h d26adf137848c56d923d52b7bd85439105bcae1a48de736f989fc64685bcea17
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority.c 57e0d760a7d12f1cc5763ba3768eba48ae6859356018c37b9353d3d1e329113a
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority_verify.c 19d2eab8e1b2b36e6ef2bdc6452e246a63a056704d7785cec8c5d25274900f40
anchor-sha256: src/frontend/analyzer/xa_program_semantic_closure.h 15ce506179036ab9708aa8cd1fb983ba4067149b0aa0c5b33241bd9c2e3b7f3b
anchor-sha256: src/frontend/analyzer/xa_program_semantic_closure.c 8f7403b7918cd9ed6ec595cd9a3b4047d964d1ccdf19c5762d4692c88212807c
anchor-sha256: src/plan/semantic/xr_program_semantic_closure.h 32016f420b187487a6ba15f1966c16a17f9743e24b00dc57279ba7841a7e619e
anchor-sha256: src/plan/semantic/xr_program_semantic_closure_internal.h 006da5f6b47f362c7e0296f648638bb396db27d6bcd9df65f58ae9cde1d5b03e
anchor-sha256: src/plan/semantic/xr_program_semantic_closure.c 561413c6e6335cbff878b8007c64a0d70cbb199ad337f58ac20dab1c2eeedb8f
anchor-sha256: src/plan/semantic/xr_program_semantic_closure_verify.c 2792df0bd7d046e226971e2e8f459c069de93f2cdb4cda6429c349d7e636ab71
anchor-sha256: src/plan/semantic/xr_source_semantic_identity.h fec2785aa50e9285d3071c8463555f03f0dfdf99c6dfd21bb3cd84fa4f00e836
anchor-sha256: src/plan/semantic/xr_source_semantic_identity.c 3978515423419fbf4e83b35893d709ab3c6bb487fae2093b1cb1ce26f0b34f95
anchor-sha256: src/plan/semantic/xr_scalar_call_semantics.h ea50939c04efe67bdd8c885f002275e12ecfb2863acf7bfe4596e6fe28d9a99c
anchor-sha256: src/plan/semantic/xr_scalar_call_semantics.c e7951de7ce36a6facece99304f2de817381b6e4051b2b66fd7b336bb1c66d493
anchor-sha256: src/plan/format/xr_xsm_decode.c 2dee1764076536b87cf00258cb58758cb799c0ffea1c8525d4ecd1a7fefcfcc7
anchor-sha256: src/plan/format/xr_xsm_encode.c 35840e929f9e86086cd57790af43eb4df6b84060704eba9045bdc9b40f579f2c
anchor-sha256: src/plan/format/xr_xsm_schema.h 98fc9a9c8f4627de81075e25905a55189ce82f5b985b190a6bfaa6ce72810242
anchor-sha256: src/plan/semantic/xr_semantic_builder.c 0d16128c1ef6a38a61d3bf921273bbaa19ccdecf74415327ce36c4f959101abb
anchor-sha256: src/plan/semantic/xr_semantic_ids.h a5efe76b603958304a7a6a873aeb9129702ac5f7994db4e840f584d1a6a3ce67
anchor-sha256: src/plan/semantic/xr_semantic_plan.c 35f8e61f8cc20d8023ba346c09161891dcd7d35bdb345b7f6e11437d3dfea895
anchor-sha256: src/plan/semantic/xr_semantic_plan.h c5423501903b883343219e51191b6120e5e926e6e8179f7f625721ee9feec140
anchor-sha256: src/plan/semantic/xr_semantic_plan_internal.h fbe1eb29e08425a629dda4c281f7a681ab48512c599cae9b63b379f4db338d2e
anchor-sha256: src/plan/semantic/xr_semantic_verify.c 5fcd1df0951db1fbe73661f3a6f3b69e2514f3544f1d30b2ea20846add5215a2
anchor-sha256: src/plan/target/xr_scalar_call_decision.h 35d3f167734562525e36406f61ddb794f7308311453a29f5695aa3e24a1c8153
anchor-sha256: src/plan/target/xr_scalar_call_decision.c 642eff61fb0b06185d2aec4c0ce8d91919148de3d29d74a17e68bd1a5016b3fa
anchor-sha256: src/plan/target/xr_scalar_call_decision_verify.c 838f7adc60c3de52fda23ed25c07edb678a6274eb87d0deec2da033e701dca10
anchor-sha256: src/ir/xi.h aeec8ba6571c3a53872d3d16d75bd32ab548abd28543284780910cd6284b96e7
anchor-sha256: src/ir/xi.c 0a8386b9b6f09578571726a6117444e9a60c4bb08b58f8dce59fadbae3417bd2
anchor-sha256: src/ir/xi_module.h 7d4be4a43230d300e8090b59db559ecf76f29934560682d1d8e1bbe4da75d3a8
anchor-sha256: src/ir/xi_lower.h 297d6165a47ac164a12df522091e970093fcc2ce35153378d5fd717019a8da72
anchor-sha256: src/ir/xi_lower.c e76300335b9c149808a2bd3c4c55d783cce6848e4f3d11b8db557078b7caa1a6
anchor-sha256: src/ir/xi_lower_expr.c 81236b56bc9c8e4a725c6d37e581c3592da0969584c896e673aeed7a26cf4670
anchor-sha256: src/ir/xi_lower_stmt.c 063e8ead6151fd583b84554e1e4d8c23892d53c537f1b287d541337b72845c32
anchor-sha256: src/ir/xi_pipeline.c fcd087786a75ad823d651f1e8d3632ab9f13516a7356eff3a1300ec8c3e2b639
anchor-sha256: src/ir/xi_program_semantic.h e21715517c57d349d83fe202f3e9cfc2348779eeaf5b2795e6b900e12bfd58c4
anchor-sha256: src/ir/xi_program_semantic.c 343744073110f6a559ba1b78d5ca277c6becc05d867d5eacb7308e3e932e94cf
anchor-sha256: src/ir/xi_program_semantic_verify.c b5f5ac6d1fd126ba35bbfb4cf49fdf1e0bb297ba8a6ecb90b21054a9adbbd298
anchor-sha256: src/ir/xi_own.c c8dda6cd586f03723baf676601306338d0cd7b6675a514670c558d96091a681e
anchor-sha256: src/ir/xi_program_semantic_plan.h c516db30bd0b7ec8fe0749bfd6fac4aecb8cfac217dad32f1954a7a66a7ee7ae
anchor-sha256: src/ir/xi_program_semantic_plan.c 42e2fa994d177b8e37aa88fdb7a300b9ef0429cebbd22e5524a36d5143c632fe
anchor-sha256: tests/unit/plan/test_program_semantic_closure.c aa2bff8af087b730168b25faf26ffb08cd69ace09963fb8c762c1071bac6796e
anchor-sha256: tests/unit/plan/test_scalar_call_decision.c 01a96bd0b8bf666d48bdf7f533873e290fa3ac2e2d266895baf43f68dcae9285
anchor-sha256: tests/unit/plan/test_semantic_plan.c 83dd97975c5b7a9e3def44f72a5f2f5e39cd1f009972365c4c55d7b859a63b6c
anchor-sha256: tests/unit/frontend/test_xa_program_semantic_closure.c 09bc04101ca2437d4ff61a1a93aea59c2228b2c44cab1a19608588f3588926fa
anchor-sha256: tests/unit/frontend/test_parser.c d106777632742a1a1187c95c93c0515143ed4d61f4ed42105b0180377bda6cec
anchor-sha256: tests/unit/module/test_module_identity.c f2054fb4d6e514c740fc635ec4204031c589ba8ff1d0b1c377fab4f4d45f2cfe
anchor-sha256: tests/unit/ir/test_xi_program_semantic.c 61f1adff92feace6edca40e6b0683ead3c38dbf69f6b68b61dc38773ce50d35f
anchor-sha256: tests/unit/ir/test_xi_pipeline.c 238976d247a3099fcc85c60a43f33c420075e75e599ae89bd5d083bd875b5f48
anchor-sha256: tests/unit/CMakeLists.txt 405e5d564669aeb8e1ad1ac31e14613a8530fd89b63247940a6d38748bdd1ba8
anchor-sha256: src/aot/xaot_boundary.h 465de1d73d5ec9cb3819fc9506405a5116567a54a048eeef38fca697f5cf8ca7
anchor-sha256: src/aot/xaot_boundary.c 22b97cdfac8d0b22905dfe036190560ab29f4beb32abd660cb9a49dc2f4c6b09
anchor-sha256: src/aot/xaot_bundle.c 4fb0d68766ccbbd70ec14a084c6055e25c245d3298774e79f45d8eb85799acda
