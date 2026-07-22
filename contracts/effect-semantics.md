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
anchor-sha256: src/frontend/analyzer/xanalyzer_errorset.c faf4dc1b0bfc17b4630f0a02f66d397b49573a898bae2e663aca84a246222d02
anchor-sha256: src/frontend/analyzer/xanalyzer_allocation.c 1665b5ffe41333187b326a44ba1332fd7e8358863671a8bc04dfa5789dcf659c
anchor-sha256: src/frontend/analyzer/xanalyzer_suspend.c 36b5a94cd0ec600b9be7f5261df79295245f26c5c4a2eed61b90f67fccac9d6a
anchor-sha256: src/frontend/analyzer/xanalyzer_memory_effect.c cead442f0a7a8366c4c04f8ad65632620e17ed6bc385be6e9f7f2940179111e5
anchor-sha256: src/frontend/analyzer/xa_typed_program.c 0458d5f96063073ac2be8af1c4549d600638978c30cfffabec584206ac0fa9fe
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_internal.h a2c2860867bbdb8b29e407ec18f20291965131dece31715888468eb041150879
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_decl.c b224cbac94a5aaa69dfaa0af5d9387dd88cf8977a5dbb3b488f1697bfdb59c88
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_stmt.c d35621aca786e478845b66ef885eb3ea127dc0f3cdf2fc51da4405005d144f79
anchor-sha256: src/runtime/value/xtype.h a69879874fb21375346041e84a065fd46f5f59a9eca97356b69e66cb265a41fe
anchor-sha256: src/ir/xi.h 76dfc7535ef24cf19aebcd598785f73c7f9c5779183228aebb92ab11da033d31
anchor-sha256: src/ir/xi_lower.c 1866fb09040dc870ddbf62c01cb3fb5db0d01d158e8812a7afc52ec4fa2b8e5e
anchor-sha256: tests/aot/run_no_throw_contract_tests.sh df115e95a79d8101345c574e025fcf629208c8a6e00ae4415b1333b622de0d4a
anchor-sha256: tests/unit/analyzer/test_analyzer.c 4ca92610d995f9bb56f95f5d7015997d67731fa2a733cf71bac218157c700dce
anchor-sha256: tests/unit/analyzer/test_effect_db.c d444b5476930dcda7f57fc331faea237abecba18021ea28091c032cde3d4d865
anchor-sha256: tests/unit/ir/test_xi_lower.c f62c3afa6f76df906885f4287a4a758e1ad3d3e459e8a8176811d4317ab3ef74
