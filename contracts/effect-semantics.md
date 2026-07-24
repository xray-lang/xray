# Effect and assertion semantics contract

Status: re-frozen by task 239 after numeric scalar identity canonicalization.

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
   unknown effect cannot erase a proven allocation/suspend/throw fact. Contract
   subjects resolve deterministically: qualified names use exact exports;
   unqualified names first identify a unique package declaration in its file
   scope, including private hot paths, and ambiguity fails closed. Each passing
   item is reported independently even when an earlier item failed.
9. Changing product membership, root substitution, completeness handling,
   resource-failure behavior, subtype direction, or assertion equivalence is a
   contract change.

## Digest anchors

anchor-sha256: src/frontend/analyzer/xa_effect_db.h 24f9175683e2d780e9a45f8f54e9349577845333d888e44a99815e953b6321e5
anchor-sha256: src/frontend/analyzer/xa_effect_db.c 4c26c5da9762ceb45390900880c8590fb2ca712ccae3174f4b3673541b204111
anchor-sha256: src/frontend/analyzer/xa_memory_effect_db.h 4a2527c4da62c7238c5df9f13b4fbcf9e210bb3555745425ace07b3704e674c3
anchor-sha256: src/frontend/analyzer/xa_memory_effect_db.c 1c3b0121cb1d9814189b615c7a5314a4dc873d1ef7ab87d86ed6deb7ba51a5e0
anchor-sha256: src/frontend/analyzer/xanalyzer_errorset.c a4ee09f3b37f5ad8c2e008d271d7ab3590203876801e27841a1fa152b8fef3ee
anchor-sha256: src/frontend/analyzer/xanalyzer_allocation.c 2e6fb0ffaac126bccd8426946d96db3403c4c674fea2e2e4d331f4c1f0a58480
anchor-sha256: src/frontend/analyzer/xanalyzer_suspend.c 06e64d18fc91182ea07577202c8e42c189da7be0e4f9c62e7b2042056aa31e41
anchor-sha256: src/frontend/analyzer/xanalyzer_memory_effect.c 5d29c8ef7b4f1fcd938266b85a47fe95794a177a39f217203ca649e0e251d737
anchor-sha256: src/frontend/analyzer/xa_typed_program.c a0cdf15d4053af6bdcce2952cecd6b0e56dadb0ff88426272bf156b2160f6628
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_internal.h b68c60f6af4d1597021f2d06d3716007487242b3f578f94a5533f8808780a52f
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_decl.c 5332f3fc932141c92f2ae2e760d1803faa4c12e5a1695bc76113cd6c8ce0b8ef
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_stmt.c 703174dd47d5bab2613b281aa4a6cd9591826e93ae1578c7e4b4b99d3895ca31
anchor-sha256: src/runtime/value/xtype.h 8d160aa422b68e0b3c4ab080fe30385bd920d3106a36ca7445bfb0944d4504ff
anchor-sha256: src/ir/xi.h 3952b18f591a6a4155c25d040d33947a72b0e3d0e9f91d705853fa6becb3e98f
anchor-sha256: src/ir/xi_lower.c 85f777ba16d19ee4d8d84d8034104c2594bbcae30f421bc090d7cc503d87e49c
anchor-sha256: src/app/cli/xcmd_verify.c f890d8419073137ec0bdc74595c555d3ffaf7bbb866a1b1432337e5e7df66fd2
anchor-sha256: tests/cli/run_verify_contract_tests.sh fe3588ad68af235f00aef1d24cbf27576206c6ce5ae9c987d3d681ebe45b9dc3
anchor-sha256: tests/unit/analyzer/test_analyzer.c 854e1c33ee7f3ab2866a5709338d6f7a634af5342ff1f9551127cb0c63d02e78
anchor-sha256: tests/unit/analyzer/test_effect_db.c d444b5476930dcda7f57fc331faea237abecba18021ea28091c032cde3d4d865
anchor-sha256: tests/unit/ir/test_xi_lower.c 19b3408bc60b4c280d5ea2e6d10f5572320c176fdbdfde1262051092cfb62183
