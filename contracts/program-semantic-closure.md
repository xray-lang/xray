# Program semantic closure identity contract

This contract freezes the target-neutral, pre-Xi semantic authority used by
the first two bounded closed-program families. PSC itself contains no layout,
storage, ABI, TargetProfile, executor recipe, or standalone artifact codec.
Its verified rows may enter Xi and then the pointer-free SemanticPlan/XSM
projection defined below; they never imply TargetPlan admission on their own.

1. ProgramSemanticClosure schema v4 has one mutable builder, one explicit
   `XrProgramSemanticFamily`, bounded module/dependency/type/type-field/
   function/function-parameter/call tables, and a typed failure kind. The
   builder canonicalizes all rows before freeze and cannot be reopened after
   verification or any terminal failure. Family is durable authority; table
   cardinality is not interpreted as a capability bit.
2. Every module row carries an exact stable module identity and complete source
   and export fingerprints. Every dependency row names two present modules and
   a nonzero contract fingerprint. The dependency graph is acyclic and every
   module is reachable from the canonical entry/export root module set.
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
   fingerprint. Its source kind and complete locator participate in the
   closed-world evidence. A cross-module call requires an exact dependency;
   every function and module must be reachable from an explicit entry/export
   root.
6. Freeze hashes schema v4, family, complete policy, every canonical row and
   locator, then derives `XrGenerationClosureId` from the full closure
   fingerprint. The independent verifier reconstructs typed fingerprints,
   identities, ordered type-field and function-parameter domains, graph
   closure, canonical order, fingerprint, and generation identity without
   calling the builder. A resigned but semantically mutated closure is rejected.
7. Canonical source identity has one domain- and length-framed owner.
   `xr_source_semantic_module_authority` derives module and empty-export
   authority from the canonical module spelling and installs the separately
   supplied complete source fingerprint. `xr_source_semantic_callsite_identity`
   binds source fingerprint, module identity, caller declaration, and exact locator. AST
   node ids, pointers, filesystem spellings, names, dense indexes, target
   representations, and ABI selections are not durable identity inputs.
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
10. A source outside either bounded structural predicate is `UNSUPPORTED` and
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
15. SemanticPlan schema 41 and program-provenance schema 2 admit both bounded
    families only after PSC-to-Xi verification. The plan stores pointer-free
    PSC schema/family/fingerprint/GCI/counts and typed bindings; it retains no
    analyzer, AST, PSC pointer, TargetProfile, CallDecision bytes, callback, or
    executor-private fact. Scalar keeps zero type bindings; leaf aggregate
    carries the exact scalar and aggregate type rows plus ordered field rows.
16. Program type bindings join stable PSC type identity and row to one semantic
    type, kind, exact-scalar id, flags, and field range. A leaf-aggregate
    binding also freezes the unique semantic source-class identity; the
    external verifier joins that identity and the complete source-class record
    back to the exact PSC-bound Xi module declaration. Field bindings join
    owner/child PSC identities and rows to the semantic child type and
    declaration ordinal. Function bindings preserve PSC flags, including
    `ENTRY`; call bindings join stable call/callsite/caller/callee identities to
    one semantic operation and exact direct-local target. Public accessors are
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
17. XSM exact-version encoding serializes provenance followed by all program
    type, type-field, function, and call bindings. Their counts participate in
    decoder storage/payload budgets; every field participates in the
    SemanticPlan fingerprint. Decode requires schema 41, exact bounds and
    payload digest, reconstructs the frozen plan, and reruns generic semantic
    verification. Round-trip is byte deterministic; hostile count, range,
    ordinal, stable type or source-class identity, family, flag, reserved,
    fingerprint, or join mutations fail closed even when outer framing is
    otherwise valid.
18. This W1 boundary closes source -> PSC v4 -> Xi -> SemanticPlan 41 -> XSM
    only. It adds no aggregate layout, slot, CallDecision, TargetPlan, VM, AOT,
    C-emission, execution-output, container, coroutine, cross-module, or public
    ABI authority. Those capabilities require later verified slices and may not
    recover missing facts from names, function bodies, tags, or legacy readers.
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
    only through the owning module's frozen PSC v4 and
    `XiFunc.psc_function_index`: the selected PSC function identity and locator
    must match one SemanticPlan 41 program-function binding, which must in turn
    match the verified TargetPlan semantic provenance and fingerprint. A mutable
    `XiFunc.semantic_plan` pointer is optional and, when present, can only confirm
    the same identity. Missing, duplicate, foreign same-shape, or re-signed
    mutations fail closed; no name, body, dense index, legacy value plan, or call
    resolver may reconstruct the join. PSC remains target-neutral: this rule
    governs only how a downstream executable consumer reaches TargetPlan
    authority and adds no W4 family.

## Digest anchors

anchor-sha256: CMakeLists.txt 5890e07bb0689c9dbdb1b72616bde8f4481a719b58b87f8ad553830cab622407
anchor-sha256: src/module/xmodule_graph.h feb99f1e6afea7a84a38379fe7ebddeec4338998ade7753489d2313dff2fc0d2
anchor-sha256: src/module/xmodule_graph.c 1e6cd17cde6de2031f99308217f02c6c43a0f0df0ba4d3f461b83c87e1d3d6ed
anchor-sha256: src/frontend/parser/xparse.c f3a9f5e4699fa00ef5653399e17754f71dac3cb2ca27e6029f89f62da75f0365
anchor-sha256: src/frontend/parser/xparse_decl.c 956af54b931477adfe03b61de2271af8951a2241a9e0fa8ec8875855be2a5b06
anchor-sha256: src/frontend/analyzer/xanalyzer.h c12244d5565e4e0ceb65177b8bf24f08c7161b6145d6c5d41fe4555d03f74988
anchor-sha256: src/frontend/analyzer/xanalyzer.c ea9243e7fae24e07022423822848abf3060ddb444b86202e96f221d7e3f8bd61
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_call.c d1c86a031eb0f9fdbbbacb2680e25bb9953b1dac3f50549035a9d1a01a8279a6
anchor-sha256: src/frontend/analyzer/xa_typed_program.h f9ba2decb49ea560631bdb90a6504d509ee4503f83f36088d4a1aab83c659f62
anchor-sha256: src/frontend/analyzer/xa_typed_program.c baa142bcb0f1841e2553d405204468c668e00782429ba32478e479af9d398680
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority.h beff336ef272509ea3a8a136b1cb85a53ef6dcb47730b1121e56e83b738741a4
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority_internal.h d26adf137848c56d923d52b7bd85439105bcae1a48de736f989fc64685bcea17
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority.c 57e0d760a7d12f1cc5763ba3768eba48ae6859356018c37b9353d3d1e329113a
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority_verify.c 19d2eab8e1b2b36e6ef2bdc6452e246a63a056704d7785cec8c5d25274900f40
anchor-sha256: src/frontend/analyzer/xa_program_semantic_closure.h 147b24ae25210d707b3e2962fd856e5b9497975606d97768bc199088f8a9f04a
anchor-sha256: src/frontend/analyzer/xa_program_semantic_closure.c 80dfdf7a062651f38d874ad6431813e408df24d4ba06926028c1082d03601b65
anchor-sha256: src/plan/semantic/xr_program_semantic_closure.h 6b53eb59868c8608b36704f3a7448960b2e4d5bf3f88f109ec1d7a387ff8442d
anchor-sha256: src/plan/semantic/xr_program_semantic_closure_internal.h 006da5f6b47f362c7e0296f648638bb396db27d6bcd9df65f58ae9cde1d5b03e
anchor-sha256: src/plan/semantic/xr_program_semantic_closure.c 7b545703eff42c71172124874b7316aa882db46a04dc09eda319137bacfe2e84
anchor-sha256: src/plan/semantic/xr_program_semantic_closure_verify.c 4a0e26364ce518dc8db35837a50f60675623040602b0b062e2a78e241a39cd70
anchor-sha256: src/plan/semantic/xr_source_semantic_identity.h ffd6bd6f8f57c44fbfe282addd337290ff0c90cc230e53cc0bfd8e089c40071a
anchor-sha256: src/plan/semantic/xr_source_semantic_identity.c e992247da4b0376c186296f939ddc0c6bf73af4818303c2196cd1393bbbaeaeb
anchor-sha256: src/plan/semantic/xr_scalar_call_semantics.h ea50939c04efe67bdd8c885f002275e12ecfb2863acf7bfe4596e6fe28d9a99c
anchor-sha256: src/plan/semantic/xr_scalar_call_semantics.c e7951de7ce36a6facece99304f2de817381b6e4051b2b66fd7b336bb1c66d493
anchor-sha256: src/plan/format/xr_xsm_decode.c 6ff4b7c3b998c73a8d7451989145d58d2d6745406c766acd40ea1bc02614bc61
anchor-sha256: src/plan/format/xr_xsm_encode.c 9e8659c4a7c586112cdef2235937b999f8600887754fe6f259a296e70bd16172
anchor-sha256: src/plan/format/xr_xsm_schema.h 98fc9a9c8f4627de81075e25905a55189ce82f5b985b190a6bfaa6ce72810242
anchor-sha256: src/plan/semantic/xr_semantic_builder.c e11bd235b1f93bde31f59b489e6644779a5897e3c245f4076451daa475f59c86
anchor-sha256: src/plan/semantic/xr_semantic_ids.h 5ccc28182a03acc01e6ab52fc150d97267b1c78da04913b3f69fd69c4a50d46c
anchor-sha256: src/plan/semantic/xr_semantic_plan.c 95ebd25bb49476d4b9e4f6a374b5e1adabe242a5668073bee90843271d0f80c7
anchor-sha256: src/plan/semantic/xr_semantic_plan.h a1743ffdc68b1f47bca570c8835c27e8e187bff29d0fb4c9fb2f373c64fb3cc6
anchor-sha256: src/plan/semantic/xr_semantic_plan_internal.h fa57d053153138212827855c8fc391ca879cfce0a0fa510e04ae11d1e4e79f63
anchor-sha256: src/plan/semantic/xr_semantic_verify.c a7d9b71745ac71f3682dbcb59998bed5cce29a81d2e954a336102de034dfdf6b
anchor-sha256: src/plan/target/xr_scalar_call_decision.h 35d3f167734562525e36406f61ddb794f7308311453a29f5695aa3e24a1c8153
anchor-sha256: src/plan/target/xr_scalar_call_decision.c 642eff61fb0b06185d2aec4c0ce8d91919148de3d29d74a17e68bd1a5016b3fa
anchor-sha256: src/plan/target/xr_scalar_call_decision_verify.c 838f7adc60c3de52fda23ed25c07edb678a6274eb87d0deec2da033e701dca10
anchor-sha256: src/ir/xi.h 159f1232d5604c2478e9c9593e4a209a9c845b45268bad17775f6e3c9387dce6
anchor-sha256: src/ir/xi.c 153f8495d91e3c886072d3283cabf62bc3e86852a66ed247a25966500e21113f
anchor-sha256: src/ir/xi_module.h 9c915818d425917ef0caf730c09774a67909abeaaef319df093aedb2d4e35794
anchor-sha256: src/ir/xi_lower.h 297d6165a47ac164a12df522091e970093fcc2ce35153378d5fd717019a8da72
anchor-sha256: src/ir/xi_lower.c 822b42c535af92571928845c185ed344cefaab451c096dc88d31ba759345f43d
anchor-sha256: src/ir/xi_lower_expr.c 05f4a5e451614770dc9ed255d0f5689343d4bd5ac673d5eefcf209fb2d2bf61d
anchor-sha256: src/ir/xi_lower_stmt.c 16c87830bf9473823d2718143eb94c42cd56f6d189e9bec879b1c724d398deee
anchor-sha256: src/ir/xi_pipeline.c 662e98afc50d25f45c13d7671c558c90679e3ea2923809df56fcbcc9a1b2035c
anchor-sha256: src/ir/xi_program_semantic.h 2d521c0bd34597e3bf3e386600e87983c11d25d217762c1bb767cf7994bcf9f4
anchor-sha256: src/ir/xi_program_semantic.c b56674c379ca65197f16f2bf79178ae81991e774003b6e381bbab1799163418a
anchor-sha256: src/ir/xi_program_semantic_verify.c ddb8dc0c03b390ffcff1795e68613d09d9aeab8c2607a454e64d1fc238bd8671
anchor-sha256: src/ir/xi_own.c c8dda6cd586f03723baf676601306338d0cd7b6675a514670c558d96091a681e
anchor-sha256: src/ir/xi_program_semantic_plan.h c69c6864a8ba090a947f64c445adafcf756685a5581b00183ef2d758ffa81efd
anchor-sha256: src/ir/xi_program_semantic_plan.c 7c10bf3d69c4e7bbd2a3245246aefd9cd7d874729695d07b39b91f0a9a0cc054
anchor-sha256: tests/unit/plan/test_program_semantic_closure.c a70207fc78e278308c8471733157017864ffbc85054575f5893ec98312c3b141
anchor-sha256: tests/unit/plan/test_scalar_call_decision.c 01a96bd0b8bf666d48bdf7f533873e290fa3ac2e2d266895baf43f68dcae9285
anchor-sha256: tests/unit/plan/test_semantic_plan.c a02d1f10431993c9efd64538f1eb8f0c0dc61999c9977ac5684e3aefda3b564d
anchor-sha256: tests/unit/frontend/test_xa_program_semantic_closure.c 706228d7e49b9283705f21f9ed94e3f72cff3d701e47804bc2c581dabcb00943
anchor-sha256: tests/unit/frontend/test_parser.c 2f0f249085f1f8d685f5460701c47aa348e1db6dd6249648792aafb0bc850a69
anchor-sha256: tests/unit/module/test_module_identity.c f2054fb4d6e514c740fc635ec4204031c589ba8ff1d0b1c377fab4f4d45f2cfe
anchor-sha256: tests/unit/ir/test_xi_program_semantic.c e052a97632a0e2f33acba67b1ec07fdb4f193d00a46a1a8efe9fbe82984b10cf
anchor-sha256: tests/unit/ir/test_xi_pipeline.c 1626ab33972df02d651bda9d2133b41128dd6633caffa0fc9df9ca58a316c721
anchor-sha256: tests/unit/CMakeLists.txt 405e5d564669aeb8e1ad1ac31e14613a8530fd89b63247940a6d38748bdd1ba8
anchor-sha256: src/aot/xaot_boundary.h 465de1d73d5ec9cb3819fc9506405a5116567a54a048eeef38fca697f5cf8ca7
anchor-sha256: src/aot/xaot_boundary.c 22b97cdfac8d0b22905dfe036190560ab29f4beb32abd660cb9a49dc2f4c6b09
anchor-sha256: src/aot/xaot_bundle.c 4fb0d68766ccbbd70ec14a084c6055e25c245d3298774e79f45d8eb85799acda
