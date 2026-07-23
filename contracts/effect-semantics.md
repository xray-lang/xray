# Effect and assertion semantics contract

Status: re-frozen by task 237.

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
8. Source `@no_throw`, `@no_suspend`, `@no_alloc`, and `@zero_cost` spellings
   are removed. `xray verify --contract <file>` consumes the same immutable
   summaries after ordinary analysis and optimization; a contract can reject a
   build but cannot change inference, storage, optimization, ABI, or generated
   code. Semantic requirements are checked dimension by dimension: an unrelated
   unknown effect cannot erase a proven allocation/suspend/throw fact.
9. Changing product membership, root substitution, completeness handling,
   resource-failure behavior, subtype direction, or assertion equivalence is a
   contract change.

## Digest anchors

anchor-sha256: src/frontend/analyzer/xa_effect_db.h 24f9175683e2d780e9a45f8f54e9349577845333d888e44a99815e953b6321e5
anchor-sha256: src/frontend/analyzer/xa_effect_db.c 4c26c5da9762ceb45390900880c8590fb2ca712ccae3174f4b3673541b204111
anchor-sha256: src/frontend/analyzer/xa_memory_effect_db.h 4a2527c4da62c7238c5df9f13b4fbcf9e210bb3555745425ace07b3704e674c3
anchor-sha256: src/frontend/analyzer/xa_memory_effect_db.c 1c3b0121cb1d9814189b615c7a5314a4dc873d1ef7ab87d86ed6deb7ba51a5e0
anchor-sha256: src/frontend/analyzer/xanalyzer_errorset.c a4ee09f3b37f5ad8c2e008d271d7ab3590203876801e27841a1fa152b8fef3ee
anchor-sha256: src/frontend/analyzer/xanalyzer_allocation.c 517f7455374269545bd9f0291872931b51c262016eb5ec3d58d7ccd948769975
anchor-sha256: src/frontend/analyzer/xanalyzer_suspend.c 06e64d18fc91182ea07577202c8e42c189da7be0e4f9c62e7b2042056aa31e41
anchor-sha256: src/frontend/analyzer/xanalyzer_memory_effect.c e6ae96486a5998c577f651bbfcbc19f3364e71ef137664591d33b8f55f2ffdd0
anchor-sha256: src/frontend/analyzer/xa_typed_program.c a0cdf15d4053af6bdcce2952cecd6b0e56dadb0ff88426272bf156b2160f6628
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_internal.h b68c60f6af4d1597021f2d06d3716007487242b3f578f94a5533f8808780a52f
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_decl.c 02ebef6c605ac3b8cbfa627eb17dd1e1dea3227d6aa5cd054a456b887e2f497c
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_stmt.c da3689ac8d93cc869641b71f85b0eb0cb08c6b730f64c27aba8e61569511c702
anchor-sha256: src/runtime/value/xtype.h 021d8fbf254a44aa8ba8776f4054314303043d04e51dddbeef0146b5de7a4685
anchor-sha256: src/ir/xi.h 5a821575558dd97fe8a3dc474fc2567c4d2a93cda9d757377cef92b11e304f7a
anchor-sha256: src/ir/xi_lower.c 85f777ba16d19ee4d8d84d8034104c2594bbcae30f421bc090d7cc503d87e49c
anchor-sha256: src/app/cli/xcmd_verify.c d8e373d7b1d8023015b4afe6b1260b0f880c76e6759679f6376f20ad74a1effa
anchor-sha256: tests/cli/run_verify_contract_tests.sh f9c80d60d50dac65eaa2b593de586b56078b907dc0a1e48ef61ce389ae21e3d1
anchor-sha256: tests/unit/analyzer/test_analyzer.c b8e9aba9a7e9b0962f44d0f23efa9d023c0950601394a615ac576ca0fdeff7cc
anchor-sha256: tests/unit/analyzer/test_effect_db.c d444b5476930dcda7f57fc331faea237abecba18021ea28091c032cde3d4d865
anchor-sha256: tests/unit/ir/test_xi_lower.c ee11c1c61f6c47ceb941e6300fa56d0b16a01e08d3865c7c003e054957bba285
