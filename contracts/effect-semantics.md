# Effect and assertion semantics contract

Status: re-frozen after suspension was split into two independent product
dimensions. Task 242 gave numeric conversions typed evidence and Task 245
separated semantic effects from native code-shape controls; the fail-closed
semantics remain unchanged.

1. Every function-like entity publishes one canonical `XaEffectSummary`
   product. Its source-semantic dimensions are typed errors, semantic
   allocation, scheduler suspend, generator suspend, may-block, thread-block,
   panic, abort, IO, foreign, synchronization, contained unsafe operations, and
   caller-unsafe requirements. Backend heap residue is not a source-semantic
   allocation.
2. Each product dimension has fail-closed completeness. Error-set completeness
   and error unknown reasons remain independently queryable so an unknown
   allocation or suspend result cannot falsify a proven no-throw result.
   `unknown_semantic_effects` identifies exactly which non-error dimensions are
   unproved; the aggregate completeness query rejects any unknown dimension.
3. Suspension is two dimensions, not one, because `Coro.yield()` and
   `yield expr` differ in every property a caller can act on. Scheduler suspend
   means control reaches the scheduler: the coroutine parks, may resume on
   another OS thread, and observes cancellation there. Generator suspend means
   the body contains `yield expr`, so its frame survives a symmetric transfer to
   the iterator driving it, with no scheduler involvement, no thread migration,
   and no cancellation point. Scheduler suspend is caller-visible and composes
   transitively across call edges; generator suspend is lexical, never composes
   across a call edge, and is never incomplete, because driving a generator
   resumes the generator's frame and returns normally to an untouched caller
   frame. `no_reschedule` forbids the first, `no_suspend` forbids both. Merging
   them back into one bit, or making generator suspend propagate, is a contract
   change.

4. Function throw effect is the tri-state internal dimension `NO_THROW`,
   `MAY_THROW`, or `POLY`. `NO_THROW` is assignable where `MAY_THROW` is
   accepted; the reverse is rejected. `POLY` is specialized at concrete
   callback call sites. Error sets remain outside structural function identity.
5. Address stability is not an effect bit. Every function also publishes one
   root-relative `XaMemoryEffectSummary` over Param, Receiver, Return, and
   ForeignHandle roots. Writes, descriptor rebinding, relocation, shortening,
   and invalidation compose transitively after call-site root substitution.
   Missing or dynamic evidence invalidates live-view permission fail-closed.
6. Analyzer database IDs are analyzer-local. Cross-analyzer publication must
   re-intern semantic summaries into the destination databases; copying a
   numeric ID is invalid. Stable effect and memory-effect fingerprints are the
   cache and verifier identity. TypedProgram exposes immutable effect and
   memory-effect sidecars and owns a node-id-keyed immutable numeric conversion
   snapshot; Xi consumes the published data rather than borrowing mutable
   analyzer node tables or re-inferring semantics from Xi op names.
7. `contains_unsafe_op` is an audit fact and normally does not propagate as a
   caller requirement. `requires_unsafe_at_call` is a capability boundary that
   must be discharged at each call site; a safe wrapper may contain unsafe work
   while remaining safe to call.
8. Dynamic/open/native unknown evidence propagates conservatively. Allocation,
   capacity, serialization, or other analysis resource failure is a compiler
   error, is never interned as an ordinary summary, and grants no optimization,
   move, sharing, borrowing, or boundary permission.
9. Source `@no_throw`, `@no_suspend`, `@no_alloc`, and `@zero_cost` spellings
   are removed. `xray verify --contract <file>` consumes the same immutable
   summaries after ordinary analysis and optimization; a contract can reject a
   build but cannot change inference, storage, optimization, ABI, or generated
   code. Semantic requirements are checked dimension by dimension: an unrelated
   unknown effect cannot erase a proven allocation/suspend/throw fact. Contract
   subjects resolve deterministically: qualified names use exact exports;
   unqualified names first identify a unique package declaration in its file
   scope, including private hot paths, and ambiguity fails closed. Each passing
   item is reported independently even when an earlier item failed.
10. Changing product membership, root substitution, completeness handling,
   resource-failure behavior, subtype direction, or assertion equivalence is a
   contract change.
11. Numeric conversion effects are witness-dependent rather than inferred from
    the generic `XI_CONVERT` opcode. Identity, contextual literals, lossless
    widening, integer-to-integer, integer-to-float, and float-to-float
    conversions are non-throwing. A runtime float-to-integer conversion is
    marked `XI_FLAG_MAY_THROW`; its only conversion failure is
    `XR_ERR_OVERFLOW` (`E0422`), and the Xi verifier rejects missing throw
    evidence. Optimization must not erase or speculate that failure edge.
12. `@inline`, `@noinline`, `codegen.opaque`, and
    `codegen.compilerFence` are semantic-neutral code-shape controls. They do
    not add or remove errors, allocation, suspension, blocking, IO, unsafe,
    synchronization, or caller requirements. `codegen.opaque` is a typed
    identity with no allocation; `codegen.compilerFence` participates only in
    compiler memory-motion legality and does not enter `XaEffectSummary` or
    `XaMemoryEffectSummary`. A verification contract may reject an unhonored
    request but cannot turn it into a semantic fact or optimization license.

## Digest anchors

anchor-sha256: src/frontend/analyzer/xa_effect_db.h e849adc15c07e973d7e5c9267afd62bd5c9b6fd912ae3a79b7b7273f798111aa
anchor-sha256: src/frontend/analyzer/xa_effect_db.c 00d83ddcc7aa11858fc3dc193208eeb67bf806915bce61835aa3b973bb97227d
anchor-sha256: src/frontend/analyzer/xa_memory_effect_db.h 4a2527c4da62c7238c5df9f13b4fbcf9e210bb3555745425ace07b3704e674c3
anchor-sha256: src/frontend/analyzer/xa_memory_effect_db.c 1c3b0121cb1d9814189b615c7a5314a4dc873d1ef7ab87d86ed6deb7ba51a5e0
anchor-sha256: src/frontend/analyzer/xanalyzer_errorset.c 65212eecec23ddb73ded9b54e869f9c51feb99126e9cc3bb9053f98be7d27ba8
anchor-sha256: src/frontend/analyzer/xanalyzer_allocation.c d4cd4b47a2e498d1602dd1e0d01751be3e88acbcc197edcc49ad99557f57d93f
anchor-sha256: src/frontend/analyzer/xanalyzer_suspend.c 37c8fef25cda39a540e1cefe7cba4f77380fd3949f49660df1a09cb7db737f64
anchor-sha256: src/frontend/analyzer/xanalyzer_memory_effect.c 19585145d88b00d1c1e4fad9fe23ac841e75c941eeaf7c18be3779befc872367
anchor-sha256: src/frontend/analyzer/xa_typed_program.c dc666a71819aa81f3573754e55626d8bec56766e16eed6191cbcfa293914b723
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_internal.h 6f9f31dfa653ad3e7b346b883acf4d58e331b1adb27afa59d9df980661c02740
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_decl.c cf282547f6086a8d9c9a756b573c28749f8999484729b547720dda17f6460224
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_stmt.c 146b18f6fad01a48d3d393549f72b57eee26548a67220adb3598b1100b12ef16
anchor-sha256: src/runtime/value/xtype.h 6f8356f93346c35577eb25432398ffa3b3f9a82904de5f0675a5a840f4cb1950
anchor-sha256: src/ir/xi.h 01dec600a8404726ae9156626681afdbdd77fa392b81a71748b603ead3c5de6e
anchor-sha256: src/ir/xi_lower.c 353819a2f766252019fec7b0e6f0b30ac210d77ad1609aa663b7f39b8d80f06c
anchor-sha256: src/app/cli/xcmd_verify.c a50c22c43cbbb4b6f9da9efbaca567600a7ded385b468b2ef8d9b71c8fa52564
anchor-sha256: tests/cli/run_verify_contract_tests.sh 80959d672b76374463854828b69ae989f44fbbe2c542253f2202737cfeeeb3f3
anchor-sha256: tests/unit/analyzer/test_analyzer.c 9bb7af4576bef36efc3fa97810b367169ed2b4c2809819719d4ea247b7972d72
anchor-sha256: tests/unit/analyzer/test_effect_db.c e2a4209e026574e6874cb6292abcaec7c47cc39a9888c64ad85f37b916ec551e
anchor-sha256: tests/unit/ir/test_xi_lower.c 473a09a299541a3ec233cee8162e09acbcf2c17b198d0bbeb7d198966225eff2
