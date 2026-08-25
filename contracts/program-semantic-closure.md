# Program semantic closure identity contract

This contract freezes the target-neutral identity foundation for a closed
program's instantiation graph. It is not a complete program-semantic-closure
implementation: no member or fixed-point summary, export manifest, artifact
codec, Xi, TargetPlan, runtime layout, storage class, call ABI, or aggregate
admission may be inferred from these rows.

1. The program semantic closure has one versioned schema and one mutable
   builder. The builder accepts bounded module, direct dependency, concrete
   type, concrete function, and resolved-call rows. It canonicalizes all rows
   before freezing and cannot be reopened after either verification or failure.
2. Every module row carries an exact stable module identity and complete source
   and export fingerprints. Every dependency row names two present modules and
   a nonzero contract fingerprint. The dependency graph is acyclic and every
   module is reachable from the canonical entry/export root module set.
3. Concrete type identities are domain-separated hashes of the complete policy,
   exact module and declaration, canonical concrete instance identity, semantic
   shape, and ownership fingerprints. Concrete function identities additionally
   bind their canonical concrete instance, complete semantic signature,
   finalized effects, and capability mask. Non-generic instances still require
   an explicit canonical non-generic instance identity. Dense indexes,
   insertion order, target profiles, layouts, and ABI facts never enter these
   identities. Schema v3 preserves the published v1 type, function, and call
   row identity frames; adding source locators changes the closed-world
   evidence, not an existing semantic row identity algorithm. Each concrete
   function also carries one nonzero-kind, complete 1-indexed, exclusive-end
   declaration locator that is unique within its source module. PSC validates
   locator structure but does not interpret producer-specific kind values.
4. A resolved-call row binds the complete policy and one stable callsite to
   exact present caller and callee function identities and a complete
   call-contract fingerprint. It also carries the source-backed call kind and
   complete 1-indexed, exclusive-end start/end locator. The stable callsite must
   independently rederive from the caller module's full source fingerprint,
   stable module identity, caller declaration identity, and that exact framed
   locator. A cross-module call requires the matching direct dependency row.
   Entry and exported functions are explicit root kinds. At least one root is
   required, multiple canonical roots are allowed, and every concrete function
   and module is reachable from the root set.
5. Freeze hashes the canonical rows, schema, and complete policy fingerprint.
   The v3 closure fingerprint includes every framed function-declaration and
   call locator coordinate.
   `XrGenerationClosureId` is a distinct domain-separated digest of that full
   closure fingerprint. Zero identities, incomplete fingerprints, duplicate
   function-declaration or call locators within one source module, empty or
   out-of-range coordinates, non-exclusive ends, locator/callsite mismatches,
   unknown flags, cycles, unreachable rows, and exhausted hard budgets fail
   closed.
6. The verifier is implemented independently from the builder. It reconstructs
   every derived row identity, canonical order, dependency and call closure,
   aggregate fingerprint, and generation identity. A frozen closure with a
   mutated state, row, fingerprint, or generation identity is rejected.
7. This schema has no artifact codec and no executor consumer. Adding a
   serialization format, cache certificate, SemanticPlan interpretation, or
   aggregate ABI decision requires a separately frozen authority and independent
   validation; none may treat this foundation as implicit Target admission. The
   bounded scalar CallDecision, Xi binding, and SemanticPlan construction proof
   described below are the only admitted readers; none makes PSC fingerprints
   self-describing.
8. The first source-backed producer is deliberately bounded to one memory
   module containing exactly two sealed, non-generic, non-suspending language
   functions and one direct `i64 -> i64` call. Analyzer publication owns a
   copied, pointer-free snapshot of the module, two function declarations and
   concrete instances, the resolved call, complete language signature and
   effect fingerprints, and zero capabilities. Foreign/C ABI, defaulted or
   variadic signatures, exports, aggregate values, and every other program
   shape are outside this family and publish no partial snapshot.
9. Source content has one full domain- and length-framed fingerprint owner.
   Function and callsite identities bind that full fingerprint, exact stable
   module and declaration/instance identities, and complete exclusive-end
   source spans. The PSC bridge mechanically copies each function-declaration
   locator and the call locator, including kind and all four source coordinates,
   from its verified owned snapshot; the plan does not
   retain an AST node, node id, name, path, or analyzer reference. Node ids,
   dense table indexes, names, filesystem paths,
   analyzer pointers, target representations, and ABI selections never enter
   durable identity. Missing or duplicate coordinates, missing resolved-call
   facts, or a mutation after the family has matched invalidate typed
   publication rather than degrading to an unsupported-family result.
10. The analyzer-layer bridge consumes only the verified owned snapshot. It
    independently verifies the snapshot, resolves caller and callee through
    their stable declaration and concrete-instance identities, and then invokes
    the plan-owned builder. It does not consult the live analyzer, graph, AST,
    semantic revision, Xi, or Target state; projection remains deterministic
    after those publication inputs are destroyed.
11. Sealed nullary `i64` and unary `i64 -> i64` language signatures, their
    complete no-throw/no-suspend/no-allocation effect contract, and the direct
    call-contract fingerprint have one target-neutral semantic owner. Analyzer
    publication validates live source facts before invoking that owner, and the
    independent analyzer verifier invokes the same semantic owner rather than
    maintaining a second fingerprint formula. Opaque PSC fingerprint bytes are
    evidence coordinates, not permission to infer a scalar contract.
12. The bounded scalar CallDecision builder accepts only a verified PSC with
    one module, two functions, one direct call, no dependency or concrete type,
    one nullary entry caller, and one unary callee. Both functions must match the
    exact semantic owner, have zero capabilities, and bind the exact direct-call
    contract. The caller-supplied GenerationClosureId and the verified
    TargetProfile fingerprint are frozen into the decision. Stale generations,
    profiles, opaque signatures, opaque call contracts, and capability growth
    fail closed.
13. A scalar CallDecision is a sealed, pointer-free value. It reuses the
    TargetPlan taxonomy for `DIRECT_LOCAL` convention and target kind, `I64`
    machine representation, by-value call mode, and no ownership action. It
    additionally freezes the TargetProfile native ABI, `STATIC_DIRECT` entry
    policy, and register-only argument/result slots, with no entry cell, adapter,
    cleanup, error channel, suspension point, or capability. Its independent
    verifier re-resolves the PSC call and functions, rechecks the target and
    generation bindings, reconstructs the exact semantic contracts, and
   independently recomputes the decision fingerprint. This authority is
   consumed only by the bounded Xi and SemanticPlan bindings below. VM, AOT, C
   emission, and the existing TargetPlan builder do not consume it.
14. The bounded source-backed producer is created by a compiler session, module
    resolver, module graph, and analyzer. Parsing and source-graph construction
    require that compiler authority, not an installed VM header, VM handle, or
    runtime constructor. The producer test owns and destroys each compiler-side
    object directly while retaining the analyzer/graph teardown proof above.
15. A frozen and independently verified closure starts with one reference and
    may be retained by additional compiler-stage owners. Retain rejects mutable,
    failed, unverified, released, and reference-saturated closures. Free drops
    one reference and destroys the pointer-free rows only when the last owner is
   released. XiModule holds one ordinary closure reference together with its
   own heap copy of the sealed pointer-free CallDecision and one reference to
   the exact frozen TargetProfile used to verify it. Failed transfer does not
   consume or partially install any authority, and module destruction releases
   all three owned values exactly once.
16. The bounded analyzer authority activates a closed Xi lowering lane only
    when the current compiler session supplies the exact independently verified
    TargetProfile. Lowering joins function declarations and the call
    mechanically by the complete PSC source locators before any generic, name,
    member, body, or shape resolver. XiFunc stores only the complete pointer-free
    declaration locator and a 32-bit PSC function-row index; the Xi call stores
    only its complete pointer-free call locator and a 32-bit PSC call-row index.
    No wide stable identity or dynamic-row pointer is copied into Xi.
17. The independent Xi scalar verifier resolves every stored row index back
    through PSC, checks exact declaration and call locators, caller, callee,
    callsite, GenerationClosureId, CallDecision, target profile, and nullary or
    unary `i64` contracts, and requires the callee preserve-call policy. The Xi
    call retains the generic conservative CALL effects and carries no unmodeled
    tail contract. Verification runs immediately after authority transfer and
    again on the post-close, post-ARC, post-optimization graph before
    SemanticPlan construction. Missing profile, locator drift, row mutation,
    decision drift, added execution contracts, or unsupported family shape fail
    closed without fallback, alias, compatibility API, TargetPlan, or
    scalar-only certificate.
18. The generic SemanticPlan builder admits this bounded family only from the
    independently verified XiModule authority triple. PSC, CallDecision, and
    TargetProfile are typed external construction inputs; the target-neutral
    plan retains no compiler pointer, target profile, decision bytes, callback,
    or opaque metadata. Instead schema 40 freezes pointer-free PSC schema,
    fingerprint, GenerationClosureId, stable function/call identities, PSC row
    indexes, semantic function/operation indexes, and the direct-local target.
    Every field is serialized by XSM, included in the SemanticPlan fingerprint,
    and checked by the generic verifier. Function bindings are ordered by
    semantic function and call bindings by semantic operation; PSC row indexes
    form exact, duplicate-free domains.
    The covered `XI_CALL` resolves caller and callee only through its Xi row
    indexes, PSC identities, and CallDecision; source exports and all generic
    direct, native, namespace, indirect, class, method, name, and body resolvers
    are bypassed for that operation. Missing, duplicate, ambiguous, or
    mismatched rows fail closed.
    A separate verifier consumes the external typed authorities and rechecks
    the Xi graph, PSC closure fingerprint and GCI, decision, exact TargetProfile,
    serialized provenance, SemanticPlan fingerprint, function and parameter
    shapes, operation/value coordinates, and the unique direct-local call-target
    row. XSM decode and round-trip preserve the same pointer-free provenance and
    generic verification remains sufficient after XiModule destruction. This
    slice adds no TargetPlan, VM, AOT, or C-emission consumer; generic unbound
    call families retain their existing resolver.

## Digest anchors

anchor-sha256: CMakeLists.txt 5ae971f2233b55857a288bbce020d3ac1ee2384f9b2bd798df20f59f782e8d49
anchor-sha256: src/module/xmodule_graph.h eee9a6baf1b55b99df35751b634eb7cc8ca7f367762f0f8be61eb9e0d80de00b
anchor-sha256: src/module/xmodule_graph.c a568f09394bbb820944df12da1199fe9ba09e685fb525c5108274eda7ed9c921
anchor-sha256: src/frontend/parser/xparse.c f3a9f5e4699fa00ef5653399e17754f71dac3cb2ca27e6029f89f62da75f0365
anchor-sha256: src/frontend/parser/xparse_decl.c 956af54b931477adfe03b61de2271af8951a2241a9e0fa8ec8875855be2a5b06
anchor-sha256: src/frontend/analyzer/xanalyzer.h c12244d5565e4e0ceb65177b8bf24f08c7161b6145d6c5d41fe4555d03f74988
anchor-sha256: src/frontend/analyzer/xanalyzer.c ea9243e7fae24e07022423822848abf3060ddb444b86202e96f221d7e3f8bd61
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_call.c d1c86a031eb0f9fdbbbacb2680e25bb9953b1dac3f50549035a9d1a01a8279a6
anchor-sha256: src/frontend/analyzer/xa_typed_program.h 8b9899bc1d3e8746463fc038d2cf2f91ce484837ae3fe69c8d75f3da3b118a4c
anchor-sha256: src/frontend/analyzer/xa_typed_program.c 2505aed722c398fb3e1ee451171c6172f845b5da7c3206b1fb33f594b27f4167
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority.h beff336ef272509ea3a8a136b1cb85a53ef6dcb47730b1121e56e83b738741a4
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority_internal.h d26adf137848c56d923d52b7bd85439105bcae1a48de736f989fc64685bcea17
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority.c ca5b60beaebf14416a86beb8c2543a0326eea7cbfe780f8742f8f7717b31d2e4
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority_verify.c 481f30070bf6d5ae7ff279dad94e1f518568d028d07a07cb2113a949f4e94375
anchor-sha256: src/frontend/analyzer/xa_program_semantic_closure.h c7da8686d42b22e56e353eca73730bd888b7f3a9c2042d152c69b10476bc7971
anchor-sha256: src/frontend/analyzer/xa_program_semantic_closure.c faff8c7a575f560bcb82159f5d98f5200a4220ca66a5fe3059f867086a4cbb66
anchor-sha256: src/plan/semantic/xr_program_semantic_closure.h 3d9173321c2e19de24894403dab4763cdb59ccb2e19dfaeebbc60eb8cde1119f
anchor-sha256: src/plan/semantic/xr_program_semantic_closure_internal.h 4706d79f3acc1345f7183e9d04ddb590c80506995fa2c108f071abfe50bc2931
anchor-sha256: src/plan/semantic/xr_program_semantic_closure.c 040ced3d57b22a96bec7d590301893e0ebb30f44e96926f44b15fe678142eab1
anchor-sha256: src/plan/semantic/xr_program_semantic_closure_verify.c 626fca9a624892525d95af3b716c0d57e4ddfc0f128a579ab42dca0ac20177b8
anchor-sha256: src/plan/semantic/xr_scalar_call_semantics.h ea50939c04efe67bdd8c885f002275e12ecfb2863acf7bfe4596e6fe28d9a99c
anchor-sha256: src/plan/semantic/xr_scalar_call_semantics.c e7951de7ce36a6facece99304f2de817381b6e4051b2b66fd7b336bb1c66d493
anchor-sha256: src/plan/format/xr_xsm_decode.c 06a7d32656f16bd4eedfdd9dcbcb02288c1d4640f44e6ca971e45773990307de
anchor-sha256: src/plan/format/xr_xsm_encode.c 9319a82c86ab2c9a2497421495839e4035d676ee167edf1d37eed4b7c9e92d38
anchor-sha256: src/plan/format/xr_xsm_schema.h 98fc9a9c8f4627de81075e25905a55189ce82f5b985b190a6bfaa6ce72810242
anchor-sha256: src/plan/semantic/xr_semantic_builder.c 5ce9c529691a34e7ea0af843e20ce2418b5df8c4dcaf587f0614f7bd3296d2ae
anchor-sha256: src/plan/semantic/xr_semantic_ids.h 0eef55790a8ae23e21ff100d638ffa98c8fefb8c7032edd7bf733f81895a8c5e
anchor-sha256: src/plan/semantic/xr_semantic_plan.c 055762048b5c69444636112658475a041a3f8e5c788f7d44431eb533fbb04011
anchor-sha256: src/plan/semantic/xr_semantic_plan.h 7a79fc5acd10e00336b248c830c70cfbdedb0028f341c1efa4719f480ea76c3b
anchor-sha256: src/plan/semantic/xr_semantic_plan_internal.h 5b2ae3086afb01f923a603dba8d80aca4cbc82b28c7c0088987d34ef35acd3a0
anchor-sha256: src/plan/semantic/xr_semantic_verify.c b4857e7dd5e58779f928c27ad8f07f98f27359753910d7692a9b66a8e1e8813f
anchor-sha256: src/plan/target/xr_scalar_call_decision.h 35d3f167734562525e36406f61ddb794f7308311453a29f5695aa3e24a1c8153
anchor-sha256: src/plan/target/xr_scalar_call_decision.c 764bee44083ff3b6622cf5a57051fc5ac558e1bdde899522274be7e4e0049b41
anchor-sha256: src/plan/target/xr_scalar_call_decision_verify.c 9d20284c377a6be00d4154193e1160dfde87389cc141fa7cb41d55d6a9bc28a0
anchor-sha256: src/ir/xi.h 79ece671a55f114ab3334071c9e21d3a72a00a80b0945772ecd103b402756ce3
anchor-sha256: src/ir/xi.c 04f5ca922145c0ef0761e5cf54cc826fa93403c183cfb84e6b3a24975d3c9d3a
anchor-sha256: src/ir/xi_module.h 42e5334390ec007c6af9e1eb69f43ee5a5cc7935677dfaa787ff0d2a786bc8ad
anchor-sha256: src/ir/xi_lower.h fcd45df9157610149721205e7f630506797e53d67d07c75febd4de77efd2b4d3
anchor-sha256: src/ir/xi_lower.c 5f7177c244562b8a23ab8cc36d5cf82c638e86b6574783f0da45a2f0a3985baa
anchor-sha256: src/ir/xi_lower_expr.c 0307e72180774953f1e9f8c26910f7eaa536b34e38e261e3783c0d554d4e0d60
anchor-sha256: src/ir/xi_lower_stmt.c 16c87830bf9473823d2718143eb94c42cd56f6d189e9bec879b1c724d398deee
anchor-sha256: src/ir/xi_pipeline.c 0693ff7ff7825f346db7d53fefaed1d792e182f501de442d085e7d41d8549cd6
anchor-sha256: src/ir/xi_scalar_program.h a213e7babc4cc683ad394fdb0a7b456fb0b2efc89078ad063e6af624ace63505
anchor-sha256: src/ir/xi_scalar_program.c 1013a8766707acd2998da9f5bd11927b1f3c3ee31b0106a9f6ee8f5922d65e50
anchor-sha256: src/ir/xi_scalar_program_verify.c 07318cf7896c87a6c68af9a501416b2c2224a8aaf461526a67e959d65a0272d5
anchor-sha256: src/ir/xi_scalar_semantic_plan.h a96e42e0bce85201022ea35aca86267eab106c830793113e2b94f5d369da9cfc
anchor-sha256: src/ir/xi_scalar_semantic_plan.c 350d8248e7c821b1c5a93a11824c09d9a9d6986ee3476448f13d78b31ccd9b20
anchor-sha256: tests/unit/plan/test_program_semantic_closure.c 31d918226ca8f369f61f1b0b6a73d6e1585a7854eeba5d0f3c7b993fc5d94bcb
anchor-sha256: tests/unit/plan/test_scalar_call_decision.c 5752882ed5842dff1006a349f79e114e33a9ccb6e1378cdfd612811fddd2ab87
anchor-sha256: tests/unit/plan/test_semantic_plan.c 39f0eaad5b9ee680975076c62a56a1bbf0a169b09e430ef319d3673242f76a0e
anchor-sha256: tests/unit/frontend/test_xa_scalar_program_closure.c 8c5f8e46818ad8d8892b39c67f017f1fe9189d2600bc734767625473c5deec43
anchor-sha256: tests/unit/frontend/test_parser.c 2f0f249085f1f8d685f5460701c47aa348e1db6dd6249648792aafb0bc850a69
anchor-sha256: tests/unit/module/test_module_identity.c 4bbb43d8d3e3296d02b62ff1669ebdeb6ea5018f3b181dd8297922046ebf3c94
anchor-sha256: tests/unit/ir/test_xi_scalar_program.c e2a914535c4787f09691124b0b9a0e1f95a218b1b060aa3ea598af30156da0f8
anchor-sha256: tests/unit/ir/test_xi_pipeline.c d2b3608b8e446464fa9611256c84d0b0e34237d48ae782c17c0f86588fb54e48
anchor-sha256: tests/unit/CMakeLists.txt 42e546f6288d61ae5cf9724504cebe9e474cc42fc53d961e9f62002cf3efb341
