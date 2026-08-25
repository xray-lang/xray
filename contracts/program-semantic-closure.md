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
   identities.
4. A resolved-call row binds the complete policy and one stable callsite to
   exact present caller and callee function identities and a complete
   call-contract fingerprint. A cross-module call requires the matching direct
   dependency row. Entry and exported functions are explicit root kinds. At
   least one root is required, multiple canonical roots are allowed, and every
   concrete function and module is reachable from the root set.
5. Freeze hashes the canonical rows, schema, and complete policy fingerprint.
   `XrGenerationClosureId` is a distinct domain-separated digest of that full
   closure fingerprint. Zero identities, incomplete fingerprints, duplicate
   coordinates, unknown flags, cycles, unreachable rows, and exhausted hard
   budgets fail closed.
6. The verifier is implemented independently from the builder. It reconstructs
   every derived row identity, canonical order, dependency and call closure,
   aggregate fingerprint, and generation identity. A frozen closure with a
   mutated state, row, fingerprint, or generation identity is rejected.
7. This schema has no artifact codec and no executor consumer. Adding a
   serialization format, cache certificate, Xi/SemanticPlan reference, target
   reader, or aggregate ABI decision requires a separately frozen authority and
   independent validation; none may treat this foundation as implicit Target
   admission.
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
   source spans. Node ids, dense table indexes, names, filesystem paths,
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

## Digest anchors

anchor-sha256: src/module/xmodule_graph.h 63cbeb6f425310bcf8daf72e827481d5fe594ed3a3ef4db4f0931ecb1bbc2672
anchor-sha256: src/module/xmodule_graph.c 0bf67850a8d8cc4043fb7ce91356934a45f90d2f2dadac34014a4a15407ef623
anchor-sha256: src/frontend/parser/xparse_decl.c 956af54b931477adfe03b61de2271af8951a2241a9e0fa8ec8875855be2a5b06
anchor-sha256: src/frontend/analyzer/xanalyzer.h c12244d5565e4e0ceb65177b8bf24f08c7161b6145d6c5d41fe4555d03f74988
anchor-sha256: src/frontend/analyzer/xanalyzer.c ea9243e7fae24e07022423822848abf3060ddb444b86202e96f221d7e3f8bd61
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_call.c d1c86a031eb0f9fdbbbacb2680e25bb9953b1dac3f50549035a9d1a01a8279a6
anchor-sha256: src/frontend/analyzer/xa_typed_program.h 8b9899bc1d3e8746463fc038d2cf2f91ce484837ae3fe69c8d75f3da3b118a4c
anchor-sha256: src/frontend/analyzer/xa_typed_program.c 2505aed722c398fb3e1ee451171c6172f845b5da7c3206b1fb33f594b27f4167
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority.h beff336ef272509ea3a8a136b1cb85a53ef6dcb47730b1121e56e83b738741a4
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority_internal.h d26adf137848c56d923d52b7bd85439105bcae1a48de736f989fc64685bcea17
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority.c b7b68856147bafce68c7ecc024d217d59f739a2a742c2f9d169668a92f489227
anchor-sha256: src/frontend/analyzer/xa_scalar_program_authority_verify.c f80f807ed2c0fb215a4c1a6f95fcd8b40e7f4cb04403b46c1327c300824c821d
anchor-sha256: src/frontend/analyzer/xa_program_semantic_closure.h c7da8686d42b22e56e353eca73730bd888b7f3a9c2042d152c69b10476bc7971
anchor-sha256: src/frontend/analyzer/xa_program_semantic_closure.c 9a749faa39f26e89e8fc5a610b57ea11cd41da6b9113aff188002c930c73dff8
anchor-sha256: src/plan/semantic/xr_program_semantic_closure.h 96469d5046d5eeeb6d5d24c71eed77f47a42b291549fd8df2810e4164f979259
anchor-sha256: src/plan/semantic/xr_program_semantic_closure_internal.h 6d4079b9c940eaa12c0e98d5bb0e4c12d0bda9a2e38103c065f1c3ac7a032aa0
anchor-sha256: src/plan/semantic/xr_program_semantic_closure.c 509a0ad8ffd0fdb87bf5ac4456cf41335db7b48bcaf74c645e45b97fb206c9ed
anchor-sha256: src/plan/semantic/xr_program_semantic_closure_verify.c 14d93b8218d406127b36469ba6efc3c36a30c1465fe3302d58e4888341482532
anchor-sha256: tests/unit/plan/test_program_semantic_closure.c f7af9b0276b5ee725f601997ad67cb53a81adbfb712338610d9a030df30c9c2e
anchor-sha256: tests/unit/frontend/test_xa_scalar_program_closure.c b0acbbc5d52a9f918d36d7b8eb0178a7fdbf243952bf2b557760a8388294ca99
anchor-sha256: tests/unit/frontend/test_parser.c 2f0f249085f1f8d685f5460701c47aa348e1db6dd6249648792aafb0bc850a69
anchor-sha256: tests/unit/module/test_module_identity.c 4bbb43d8d3e3296d02b62ff1669ebdeb6ea5018f3b181dd8297922046ebf3c94
anchor-sha256: tests/unit/CMakeLists.txt 110c3de244f25372ec62ecb037c1b62765877789ab14d905271c3d2593e53e9c
