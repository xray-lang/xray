# Effect and assertion semantics contract

Status: re-frozen after suspension was split into two independent product
dimensions. Task 242 gave numeric conversions typed evidence and Task 245
separated semantic effects from native code-shape controls; the fail-closed
semantics remain unchanged.
Task 254 makes mutable-capture cell and weak-field memory effects explicit Xi
operations; it does not add a source-level effect or permit backend inference.
Task 251 makes source-parameter write provenance complete for scalar `ref`
parameters and permits an advisory unused-`ref` hint only from that canonical,
complete effect product.

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
13. A coroutine boundary that re-raises a child's failure is an error-set edge,
    not just a control-flow one. `await t` unions the error set of the awaited
    coroutine's body into the awaiting function, and each `go` inside a
    `linked scope` unions its body's errors into the function containing the
    scope, because that is where the scope re-raises them. A body that cannot
    be named at the boundary is fail-closed to may-throw. The forms that report
    an outcome as a value -- `awaitResult()`, `awaitTimeout()`, `await all`,
    `await any`, `await anySuccess` -- do not re-raise and contribute nothing.
    A detached `go` outside a linked scope likewise contributes nothing, and
    expanding a callee's body must not carry either fact across the call edge.
    Making a re-raising boundary contribute nothing, or a non-re-raising one
    contribute, is a contract change.
14. Return ownership is a typed-program sidecar fact, not an allocation-effect
    heuristic. Reference-capable source functions publish `OWNED`,
    `BORROWED_PARAM(n)`, `BORROWED_STATIC`, or fail-closed `UNKNOWN`; recursive
    call graphs are solved to a fixed point. Cross-analyzer symbol copies
    preserve the semantic kind and parameter identity rather than a database
    address. Native reference returns must declare the same metadata in the
    standard-library definition, including explicit `UNKNOWN` when no stronger
    fact is valid. Xi consumes the published summary at each statically known
    call and does not re-infer it from a callee name or allocation effects.
15. Parameter mutation is one canonical effect fact for scalar parameters,
    aggregate/view roots, and transitive calls. Direct assignment, compound
    assignment, increment/decrement, member/index writes, and a known mutating
    callee set the write fact. An explicit `ref` argument to an unresolved
    dynamic callable makes that parameter effect incomplete and therefore
    cannot prove read-only behavior. An unused-`ref` diagnostic is advisory
    only: it may be emitted for a source-explicit anonymous-function `ref`
    parameter only when the effect is complete and non-mutating, and is
    suppressed when an expected callable contract requires `ref` or the mode
    came from inference. The diagnostic never changes the function type,
    effect product, exit status, or runtime semantics.

## Digest anchors

anchor-sha256: src/frontend/analyzer/xa_effect_db.h e849adc15c07e973d7e5c9267afd62bd5c9b6fd912ae3a79b7b7273f798111aa
anchor-sha256: src/frontend/analyzer/xa_effect_db.c 00d83ddcc7aa11858fc3dc193208eeb67bf806915bce61835aa3b973bb97227d
anchor-sha256: src/frontend/analyzer/xa_memory_effect_db.h 4a2527c4da62c7238c5df9f13b4fbcf9e210bb3555745425ace07b3704e674c3
anchor-sha256: src/frontend/analyzer/xa_memory_effect_db.c 1c3b0121cb1d9814189b615c7a5314a4dc873d1ef7ab87d86ed6deb7ba51a5e0
anchor-sha256: src/frontend/analyzer/xanalyzer_errorset.c 16b9910b193cde99233b16ef8fdd0cf3d39f4cdae9216c2e89f8d804c9884417
anchor-sha256: src/frontend/analyzer/xanalyzer_allocation.c d4cd4b47a2e498d1602dd1e0d01751be3e88acbcc197edcc49ad99557f57d93f
anchor-sha256: src/frontend/analyzer/xanalyzer_suspend.c b71501e112a6aee3caa413c03e883fbd3f05e9d458dfdbeb8240025cd431ff92
anchor-sha256: src/frontend/analyzer/xanalyzer_memory_effect.c 19585145d88b00d1c1e4fad9fe23ac841e75c941eeaf7c18be3779befc872367
anchor-sha256: src/frontend/analyzer/xa_typed_program.c dc666a71819aa81f3573754e55626d8bec56766e16eed6191cbcfa293914b723
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_internal.h ea162b828bb6c8e3f563bd11945d1d5873258c5a832eb8f3e05a51a48830efaa
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_decl.c 2240e7ab541f0da201dfe82a3fac9f8005444a446e175815e88979b419820ee1
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_stmt.c 9a00415e02c13138871e893473eaefc1ccd1df00a8cc844774f58162f0a96761
anchor-sha256: src/runtime/value/xtype.h 1f0e1b8129b20d80d527cccc341bb22fffd34b1e1fd83eb252da7d23e2669317
anchor-sha256: src/ir/xi.h 286b998d786bed1678de12e25b19e08e03a95299f82499bebe483dc465cfff2d
anchor-sha256: src/ir/xi_lower.c 4518cf90540ed7a869ef07629d7305e9e6df31f4b44ad47359efcd339952ca4e
anchor-sha256: src/app/cli/xcmd_verify.c 621d117db22a9c3c101d183f3c5554616bdba614d6ebbaf942d50d6b3ccf6f29
anchor-sha256: tests/cli/run_verify_contract_tests.sh 0e6de65b1956cb7151e90630d72bc41e10d5561147da0ee012882660eee7ac65
anchor-sha256: tests/unit/analyzer/test_analyzer.c 86efb5f000793d5c0beb8e0a45979a42d328a1bd3c9e07165810e776de40aa5e
anchor-sha256: tests/unit/analyzer/test_effect_db.c 15b62bd4e820af1d1798476afe61459372218e26b83db65d00a0f40cb2002bf1
anchor-sha256: tests/unit/ir/test_xi_lower.c 11a2064441f6aa9b964d9d01655c58dd95530e79f2c34188d353fdc36b0a87a1
