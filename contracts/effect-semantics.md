# Effect and assertion semantics contract

Status: frozen by task 220.

1. Every function-like entity publishes one canonical `XaEffectSummary`
   product. Its source-semantic dimensions are typed errors, semantic
   allocation, suspend, may-block, thread-block, panic, abort, IO, foreign,
   synchronization, contained unsafe operations, and caller-unsafe
   requirements. Backend heap residue is not a source-semantic allocation.
2. Each product dimension has fail-closed completeness. Error-set completeness
   and error unknown reasons remain independently queryable so an unknown
   allocation or suspend result cannot falsify a proven no-throw result.
   `unknown_semantic_effects` identifies exactly which non-error dimensions are
   unproved; the aggregate completeness query rejects any unknown dimension.
3. Function throw effect is the tri-state internal dimension `NO_THROW`,
   `MAY_THROW`, or `POLY`. `NO_THROW` is assignable where `MAY_THROW` is
   accepted; the reverse is rejected. `POLY` is specialized at concrete
   callback call sites. Error sets remain outside structural function identity.
4. Address stability is not an effect bit. Every function also publishes one
   root-relative `XaMemoryEffectSummary` over Param, Receiver, Return, and
   ForeignHandle roots. Writes, descriptor rebinding, relocation, shortening,
   and invalidation compose transitively after call-site root substitution.
   Missing or dynamic evidence invalidates live-view permission fail-closed.
5. Analyzer database IDs are process-local. Stable effect and memory-effect
   fingerprints are the cache and verifier identity. TypedProgram exposes both
   immutable sidecars, and Xi carries their IDs, fingerprints, completeness,
   and source-semantic product bits without re-inferring them from Xi op names.
6. `contains_unsafe_op` is an audit fact and normally does not propagate as a
   caller requirement. `requires_unsafe_at_call` is a capability boundary that
   must be discharged at each call site; a safe wrapper may contain unsafe work
   while remaining safe to call.
7. Dynamic/open/native unknown evidence propagates conservatively. Allocation,
   capacity, serialization, or other analysis resource failure is a compiler
   error, is never interned as an ordinary summary, and grants no optimization,
   move, sharing, borrowing, or boundary permission.
8. During the pre-cutover implementation wave, source `@no_throw`,
   `@no_suspend`, `@no_alloc`, and `@zero_cost` declarations remain assertions,
   never optimization hints. They do not change inferred effects or generated
   code. The clean-slate public-surface cutover removes these spellings only
   after the external verify-contract path consumes the same summaries.
9. Changing product membership, root substitution, completeness handling,
   resource-failure behavior, subtype direction, or assertion equivalence is a
   contract change.

## Digest anchors

anchor-sha256: src/frontend/analyzer/xa_assertion_attr.def 82cf5e56e8fedd3bc9ea6f824ef6ccf86534dbfb5934291cae1a15bc75652f54
anchor-sha256: src/frontend/analyzer/xa_effect_db.h 79039270e1063a7f5ed9f426fe91fa4e511ec76bf64709c8554bc323bdf0f672
anchor-sha256: src/frontend/analyzer/xa_effect_db.c 538b1b1bdbb637b7f3675f56881c81d15fbaa84c9dd3f01f3b9281e2c03d7ce5
anchor-sha256: src/frontend/analyzer/xa_memory_effect_db.h 4a2527c4da62c7238c5df9f13b4fbcf9e210bb3555745425ace07b3704e674c3
anchor-sha256: src/frontend/analyzer/xa_memory_effect_db.c 1c3b0121cb1d9814189b615c7a5314a4dc873d1ef7ab87d86ed6deb7ba51a5e0
anchor-sha256: src/frontend/analyzer/xanalyzer_errorset.c 54e07424fba7fb1121db7bd9f4e9b302b029bfb3b946829a06c72d2c6fa0136e
anchor-sha256: src/frontend/analyzer/xanalyzer_allocation.c 6e07fdbd22866e6ed7c02b8b70262c5f70b86acccb9bc618cf0668bca5028d5c
anchor-sha256: src/frontend/analyzer/xanalyzer_suspend.c e0826a6b71f1c9d2f70211ac583132bc53e9f4f6cbbeb51d53b17e4478348cf0
anchor-sha256: src/frontend/analyzer/xanalyzer_memory_effect.c 417b3cf8e7a789cb38a6ec387f4c435c89809bdc6ec93bdf71d1b26952d7f3fe
anchor-sha256: src/frontend/analyzer/xa_typed_program.c a0cdf15d4053af6bdcce2952cecd6b0e56dadb0ff88426272bf156b2160f6628
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_internal.h 8af8ffd27ee5a6942da6f444203a8a758918aaa5c12f7003d6aea5900af99196
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_decl.c f06668c72d61170e72180c07e1e69a2a00b2018c8b916936bc78899190e7e633
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_stmt.c 532907454aab7a963d5940f12fa0b71d23982ce7d7fdf7e419b24e5d90c3d059
anchor-sha256: src/runtime/value/xtype.h 92a93667068ff198b48b7b51e3641bf1e9e64d6c9fb10c96e55ddb977c4e0f8b
anchor-sha256: src/ir/xi.h 1d3594142d00d14f9a8f07b57398031c17f2e1512f1a09be15bdcef21e3a653b
anchor-sha256: src/ir/xi_lower.c f731e289e7866fe9fd0a1f90cd524eaa667ab2725f9aa31b97c95bfb86fc72ef
anchor-sha256: tests/aot/run_no_throw_contract_tests.sh df115e95a79d8101345c574e025fcf629208c8a6e00ae4415b1333b622de0d4a
anchor-sha256: tests/unit/analyzer/test_analyzer.c 29be1548d2d946ae5dd38b288ff3dbc3f66e37231565bc74fcd2b2237675f58b
anchor-sha256: tests/unit/analyzer/test_effect_db.c d444b5476930dcda7f57fc331faea237abecba18021ea28091c032cde3d4d865
anchor-sha256: tests/unit/ir/test_xi_lower.c e626a845aa0bde8ef286bada6be4da57d5c6ac93fb80cfb9b45d22b8f1e4c023
