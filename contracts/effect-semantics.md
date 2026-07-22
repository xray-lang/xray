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
5. Analyzer database IDs are analyzer-local. Cross-analyzer publication must
   re-intern semantic summaries into the destination databases; copying a
   numeric ID is invalid. Stable effect and memory-effect fingerprints are the
   cache and verifier identity. TypedProgram exposes both immutable sidecars,
   and Xi carries their IDs, fingerprints, completeness, and source-semantic
   product bits without re-inferring them from Xi op names.
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
anchor-sha256: src/frontend/analyzer/xa_effect_db.h 24f9175683e2d780e9a45f8f54e9349577845333d888e44a99815e953b6321e5
anchor-sha256: src/frontend/analyzer/xa_effect_db.c 4c26c5da9762ceb45390900880c8590fb2ca712ccae3174f4b3673541b204111
anchor-sha256: src/frontend/analyzer/xa_memory_effect_db.h 4a2527c4da62c7238c5df9f13b4fbcf9e210bb3555745425ace07b3704e674c3
anchor-sha256: src/frontend/analyzer/xa_memory_effect_db.c 1c3b0121cb1d9814189b615c7a5314a4dc873d1ef7ab87d86ed6deb7ba51a5e0
anchor-sha256: src/frontend/analyzer/xanalyzer_errorset.c 8ae45cba84d910429ee54cc2761c22cc638f6547dc4bcbd70c496e04b1b0b9a2
anchor-sha256: src/frontend/analyzer/xanalyzer_allocation.c a63af0da08dadee819eca8a9e093a2fcec04e87a36bb589ca1976d2383c0bbe1
anchor-sha256: src/frontend/analyzer/xanalyzer_suspend.c ed488f80893471a0699c5faf842994d652154cc3321b062eecdbb9c04d9faf77
anchor-sha256: src/frontend/analyzer/xanalyzer_memory_effect.c e6ae96486a5998c577f651bbfcbc19f3364e71ef137664591d33b8f55f2ffdd0
anchor-sha256: src/frontend/analyzer/xa_typed_program.c a0cdf15d4053af6bdcce2952cecd6b0e56dadb0ff88426272bf156b2160f6628
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_internal.h 1c51acae26a462f485aba54b7e74ed28663bc35849f5496c0752dc76e74c9a6d
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_decl.c 2c2612044cad3649278fc784dbe11e358e7d1a0e3e07cbcf8eab82f3d1287ee1
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_stmt.c 56d2e4f203a2235bbf099602b423052b27c4ff85bd53d3e842b108446d2a2e30
anchor-sha256: src/runtime/value/xtype.h b476be059c1cd1c0e4f77711e7d39e29c47fc9c7b03ab7fc73f96f903830bea0
anchor-sha256: src/ir/xi.h a3035c4eceb29ebc6fb0517f4e563a3c62ab4efa44cebb54657e1ae4bff0ae05
anchor-sha256: src/ir/xi_lower.c 0201a416be823e9c1cab4aae4ca6469e7f1531d6ffd92fdc71d67e2e50c7fc50
anchor-sha256: tests/aot/run_no_throw_contract_tests.sh df115e95a79d8101345c574e025fcf629208c8a6e00ae4415b1333b622de0d4a
anchor-sha256: tests/unit/analyzer/test_analyzer.c eef1ba27a6700dc927e131f12434141e7cec3b48afaa37d8af9e74c3163d87cb
anchor-sha256: tests/unit/analyzer/test_effect_db.c d444b5476930dcda7f57fc331faea237abecba18021ea28091c032cde3d4d865
anchor-sha256: tests/unit/ir/test_xi_lower.c ee11c1c61f6c47ceb941e6300fa56d0b16a01e08d3865c7c003e054957bba285
