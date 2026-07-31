# Effect and assertion semantics contract

Status: re-frozen after Task 242 gave numeric conversions typed evidence and
Task 245 separated semantic effects from native code-shape controls. The
ordinary effect product, product membership, and fail-closed semantics remain
unchanged.

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
   cache and verifier identity. TypedProgram exposes immutable effect and
   memory-effect sidecars and owns a node-id-keyed immutable numeric conversion
   snapshot; Xi consumes the published data rather than borrowing mutable
   analyzer node tables or re-inferring semantics from Xi op names.
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
10. Numeric conversion effects are witness-dependent rather than inferred from
    the generic `XI_CONVERT` opcode. Identity, contextual literals, lossless
    widening, integer-to-integer, integer-to-float, and float-to-float
    conversions are non-throwing. A runtime float-to-integer conversion is
    marked `XI_FLAG_MAY_THROW`; its only conversion failure is
    `XR_ERR_OVERFLOW` (`E0422`), and the Xi verifier rejects missing throw
    evidence. Optimization must not erase or speculate that failure edge.
11. `@inline`, `@noinline`, `codegen.opaque`, and
    `codegen.compilerFence` are semantic-neutral code-shape controls. They do
    not add or remove errors, allocation, suspension, blocking, IO, unsafe,
    synchronization, or caller requirements. `codegen.opaque` is a typed
    identity with no allocation; `codegen.compilerFence` participates only in
    compiler memory-motion legality and does not enter `XaEffectSummary` or
    `XaMemoryEffectSummary`. A verification contract may reject an unhonored
    request but cannot turn it into a semantic fact or optimization license.

## Digest anchors

anchor-sha256: src/frontend/analyzer/xa_effect_db.h 24f9175683e2d780e9a45f8f54e9349577845333d888e44a99815e953b6321e5
anchor-sha256: src/frontend/analyzer/xa_effect_db.c 4c26c5da9762ceb45390900880c8590fb2ca712ccae3174f4b3673541b204111
anchor-sha256: src/frontend/analyzer/xa_memory_effect_db.h 4a2527c4da62c7238c5df9f13b4fbcf9e210bb3555745425ace07b3704e674c3
anchor-sha256: src/frontend/analyzer/xa_memory_effect_db.c 1c3b0121cb1d9814189b615c7a5314a4dc873d1ef7ab87d86ed6deb7ba51a5e0
anchor-sha256: src/frontend/analyzer/xanalyzer_errorset.c 65212eecec23ddb73ded9b54e869f9c51feb99126e9cc3bb9053f98be7d27ba8
anchor-sha256: src/frontend/analyzer/xanalyzer_allocation.c bc3e201c0ac687cc4fde170d26bdab72e6b238ebb2d6d6b2395fdd1a439c9526
anchor-sha256: src/frontend/analyzer/xanalyzer_suspend.c 06e64d18fc91182ea07577202c8e42c189da7be0e4f9c62e7b2042056aa31e41
anchor-sha256: src/frontend/analyzer/xanalyzer_memory_effect.c 19585145d88b00d1c1e4fad9fe23ac841e75c941eeaf7c18be3779befc872367
anchor-sha256: src/frontend/analyzer/xa_typed_program.c dc666a71819aa81f3573754e55626d8bec56766e16eed6191cbcfa293914b723
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_internal.h 19aae957f5cf95f8827641a64eac347a5e943b1bd1525f779f8b3d562f6e83d5
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_decl.c c19ad0a9554c49e12c750343c7b826f390d504e1ba8b90048a2be764465ba431
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_stmt.c ca6ff19c044cb7f9b3a27ae15bd98c3a3c65836ac87f61d378e1caf7920fa8d9
anchor-sha256: src/runtime/value/xtype.h 6ee6583450d72ae437a49f663115338eaeaba5f0a57dbab738ca443bf9cf1e57
anchor-sha256: src/ir/xi.h 174c729c1c7dbe55a0bc2e6d866f8cd2cf8973d447e325ec97f14b3958218e7f
anchor-sha256: src/ir/xi_lower.c 353819a2f766252019fec7b0e6f0b30ac210d77ad1609aa663b7f39b8d80f06c
anchor-sha256: src/app/cli/xcmd_verify.c ca9b1e734649830c52da8060b0bba68afffb031257cd8aaf75208f277b5fc153
anchor-sha256: tests/cli/run_verify_contract_tests.sh fe3588ad68af235f00aef1d24cbf27576206c6ce5ae9c987d3d681ebe45b9dc3
anchor-sha256: tests/unit/analyzer/test_analyzer.c 0bc4d04e1a027edfc0bdff456542f105428110aeb7b6e5a7d450dc3ce2a7848f
anchor-sha256: tests/unit/analyzer/test_effect_db.c d444b5476930dcda7f57fc331faea237abecba18021ea28091c032cde3d4d865
anchor-sha256: tests/unit/ir/test_xi_lower.c 788a6d9c4281030537c5146af5eab412fdcfdafac0c0cea163550805b6303f33
