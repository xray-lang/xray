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
SemanticPlan schema 23 preserves the pointer-free `DIRECT_LOCAL` call-target
authority to lexical shared slots. Direct SSA callees still resolve only
through exact identity copies to a closure/function binding. A shared callee
must be a `GET_SHARED(slot)` whose first lexical owner, found by walking the
frozen function-parent chain, contains exactly one `SET_SHARED(slot, value)`;
that value must independently resolve through exact identity copies to a local
closure binding. Multiple writes are ambiguous even when they store the same
value, and a sibling's same-numbered slot is never evidence. In the caller's
own function the store block must dominate the load block, with strict store
before load order when both are in one block. A lexical-parent store is accepted
only from canonical function index zero with no parent, in that function's
entry block, and before every call, method call, builtin call, tail call, or
`XI_GO` activation boundary in the block. This is the exact module-initializer
prefix known to run before a nested function can execute. Conditional root
stores, late root stores, and stores in any other lexical parent remain
unavailable rather than assuming closure-creation order. The verifier rebuilds
this relation from frozen operation, operand, function-parent, CFG-dominance,
slot, and callable-function facts rather than trusting the builder's target row.
Schema 22 also freezes `core.string.bytes` as an exact non-allocating borrowed
`Slice<byte>` view. Its record binds the numeric intrinsic identity, String
source operand and semantic root, byte element type, read capability, and
caller lifetime. Independent verification reconstructs these facts; selector
text, aliases, analyzer-local IDs, and backend type guesses are not authority.
A method call on a compiler-owned class namespace carries its own numeric
intrinsic identity. `JSON.value` is frozen when the receiver type record is the
`JSON` class with no source-class index and no source-class identity, the
selector is the sole frozen metadata entry, exactly one argument carries the
call contract, and the result is an owned non-aliasing JSON value. A source
declaration can never produce that receiver record because every declared class
records its own class authority, so the namespace plus the selector names one
implementation. Every other namespace member stays without dispatch authority.
When the independently proven target reaches a function with a canonical
static suspend operation (or `XI_GO`), the same row also authorizes exactly one
coroutine-state entity for an ordinary call. A direct tail-call edge propagates
the target's suspendability to its caller but has no resume state of its own.
The record never copies caller-authored
effects, provider spellings, symbols, pointers, or raw digests. Schema 16 retains
one separate `NATIVE_YIELDABLE` authority for an ordinary `XI_CALL` whose
callee resolves through exact identity copies to a bare canonical
`XI_IMPORT_REF`. The module/member pair must name exactly one generated stdlib
definition with a complete signature, VM binding, yieldable contract, and
exact argument count. The full generated stdlib registry is fingerprinted into
SemanticPlan and XSM; the independent verifier repeats the name, shape,
argument, registry-fingerprint, and coroutine-state proof. Tail calls, relative
imports, unknown or duplicate definitions remain absent and fail closed.
Schema 17 additionally admits `SOURCE_EXPORT` only for a non-super method call
whose receiver independently resolves through identity copies and one
root-initializer `SET_SHARED` to a whole-module `XI_IMPORT_REF`. The import path,
selector, dependency module stable identity, exact dependency SemanticPlan
fingerprint, public export identity, and exported function identity are frozen.
The dependency plan must itself prove that the public export is the unique
root-entry store of that local closure before activation and that its function
is suspendable through the frozen direct call graph. Standalone XSM decoding of
such a plan fails closed; module-set decoding requires the exact canonical
ordered vector of verified dependency plans and repeats every relation. This
follows a public Xray wrapper such as
`net.writeBytes`; it never reinterprets it as private native `net.__writeBytes`.
Schema 17 also freezes one `INDIRECT_CALLABLE` row for an ordinary `XI_CALL`
whose callee operand has an exact frozen function type but whose runtime
function-value producer is open. The row records only that function-type stable
identity and the exact coroutine-state obligation; all target function,
dependency, source-export, and native identities remain empty. The independent
verifier rebuilds the callee operand role, function type, identity-copy chain,
and open producer from the frozen value definitions. Direct closures, canonical
native imports, source exports, lexical shared-slot resolution, method calls,
builtin calls, and tail calls retain their more specific authority or remain
unavailable. An indirect row neither enumerates a target set nor authorizes
dispatch or execution in TargetPlan.
Schema 17 replaces the operation wire's reserved byte with one exact import
resolution kind: unresolved, source module, or native stdlib fallback. A native
fallback is publishable only after the module-graph resolver has completed the
lookup, found no graph module, left every source-export binding empty, and the
bare module exists in the generated stdlib registry. `aux_int == -1`, an import
spelling, or registry membership alone is never resolution authority. The
builder may then publish `NATIVE_NAMESPACE_YIELDABLE` for a non-super
`XI_CALL_METHOD` only when its receiver independently follows exact identity
copies through one canonical root-initializer shared-slot store to that
whole-module import, and the selector and argument count match one complete
yieldable registry definition. The verifier reconstructs the shared-store,
root-prefix, import-resolution, registry, arity, and one-to-one coroutine-state
relation from frozen rows. Source-module shadowing, unresolved imports,
selective imports, private-wrapper substitution, builtin calls, tail calls,
ordinary methods, and non-yieldable definitions remain unavailable. This
family freezes coroutine obligation only; TargetPlan dispatch and execution
continue to fail closed until a separate target family consumes it.
Schema 19 additionally requires an exact allocation identity for the zero-
argument builtin `StringBuilder()` constructor. Publication requires the
canonical builtin instance type (never a user class with the same spelling),
the exact `CALL_BUILTIN` metadata, flags, arity, ownership, return provenance,
and an allocation key equal to the operation key plus `/allocation`; its stable
ID is recomputed from that key. The independent verifier rebuilds the same
relation from frozen type, metadata, operation, ownership, and identity facts.
Missing allocation coverage, a forged key or ID, a shadowed type, or any
partial constructor shape fails closed. The generic builtin-call effect row is
unchanged, and no TargetPlan or AOT execution authority follows from this
SemanticPlan prerequisite alone.
Schema 19 freezes a canonical builtin declaration identity in every semantic
type row and in the `type-v3` canonical key. A user class with the same display
name has builtin identity zero and cannot collide with a runtime builtin. The
builder may publish `BUILTIN_INSTANCE_YIELDABLE` only for an exact non-super
`XI_CALL_METHOD` on the frozen builtin receiver type when the selector and
arity match the bounded Task, WorkQueue, ResultGroup, CountdownLatch,
Semaphore, or EventCount yieldable-method table. The independent verifier
rebuilds the receiver operand, builtin identity, selector, arity, canonical
call identity, and one-to-one coroutine-state relation. It does not freeze an
execution target, provider spelling, frame layout, or result materialization;
TargetPlan therefore continues to reject the call until a separate target
family consumes it. Unknown builtins, user shadows, super calls, wrong arity,
ordinary methods, and missing states remain fail closed.
Schema 20 additionally freezes each source class with its stable module entity,
normalized module path, module-local source ordinal, name, method count, and
final/runtime/generic flags. Source functions freeze that class identity,
member ordinal, and method kind; source-instance types freeze the class table
link. None of these rows may publish analyzer-local class, method, function, or
module IDs. The builder may publish `SOURCE_INSTANCE_METHOD_LOCAL` only for an
exact non-super method call whose receiver is a final, runtime, non-generic
source class in the same module. An erased receiver is accepted only when it is
exactly parameter zero of a source instance method in that class. Selector and
arity must identify one unique instance method. The independent verifier
reconstructs the class and function stable keys, method membership, receiver or
exact-self proof, selector, arity, callee stable identity, and one-to-one
coroutine-state relation. Open classes, generic classes, super calls, ambiguous
methods, unrelated erased receivers, forged targets, and missing states remain
fail closed. This authority proves suspendability only; TargetPlan still has no
execution family for source instance dispatch.
Schema 21 additionally freezes every source instance-method declaration as a
stable source-class identity, module-local member ordinal, selector, function
identity, parameter count, and final/open-domain flag. Imported source nominal
types carry only that dependency class stable identity across XSM; analyzer
class IDs and pointers may locate a dependency row during construction but are
never serialized or hashed as authority. `SOURCE_INSTANCE_METHOD_OPEN` is
published only for an exact non-super call on a dependency's open, runtime,
non-generic source class when the declaration selector and arity are unique and
the dependency's verified SemanticPlan independently proves that declaration
suspendable. Module-set verification repeats the class, method, function,
selector, arity, flags, and suspendability proof against the exact ordered
dependency plans. It authorizes the conservative coroutine-state obligation
for the open dispatch domain, not a closed target set or execution target;
TargetPlan therefore remains fail closed. Inherited declarations, generic
classes, ambiguous dependency identities, synchronous declarations, super
calls, missing states, standalone dependency decoding, and forged class or
method IDs remain unavailable.
Schema 23 additionally freezes an exact source-enum declaration in each
eligible enum type row. Its stable preimage binds the canonical nominal owner,
enum name, ordered member names and payload counts; the row also freezes the
independently recomputable nominal layout ID, member count, and unit-enum flag.
Analyzer-local enum IDs, pointers, display names alone, and backend type guesses
are never authority. XSM has a hard schema cutover and the independent verifier
reconstructs the stable identity, layout hash, and canonical type-key link.
Function declarations publish their immutable binding capability and lexical
storage domain with the rest of analyzer ownership evidence. Closure lowering
may copy those facts into SemanticPlan capture records but may not infer them
from a function value or declaration name.
Backend contract verification now requires a canonical exact TargetProfile
before AOT preparation. The profile selects representation and ABI facts only;
it cannot add, remove, or complete a source-semantic effect dimension.

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
6. Analyzer database, node, and symbol IDs are analyzer-local. Cross-analyzer
   publication must re-intern semantic summaries into the destination
   databases; copying any numeric identity is invalid. When a declaration-owned
   expression is analyzed in another analyzer -- including an imported default
   argument -- its referenced declaration is represented by one cached,
   destination-owned semantic symbol view with a fresh ID and re-interned export
   metadata. The view is not inserted into lexical lookup, and borrows only
   immutable source semantic payloads whose owner outlives the destination.
   Stable effect and memory-effect fingerprints are the cache and verifier
   identity. TypedProgram exposes immutable effect and memory-effect sidecars
   and owns a node-id-keyed immutable numeric conversion snapshot; Xi consumes
   the published data rather than borrowing mutable analyzer node tables or
   re-inferring semantics from Xi op names.
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
    call graphs are solved to a fixed point. Reference capability, not movable
    roots, decides which functions publish: a returned `string` has no movable
    root yet is refcounted, so its ownership at the return boundary is exactly
    the fact a caller needs. Publication is unconditional and happens while the
    declaration's own scope is current, because the scan resolves returned
    names through that scope; a summary computed on demand from a later phase
    resolves them elsewhere or not at all. Cross-analyzer symbol copies
    preserve the semantic kind and parameter identity rather than a database
    address. Native reference returns must declare the same metadata in the
    standard-library definition, including explicit `UNKNOWN` when no stronger
    fact is valid. Xi consumes the published summary at each statically known
    call and does not re-infer it from a callee name or allocation effects.
    A call through an interface has no single declaration to read, so the
    whole-program evidence answers it by meeting the published ownership of
    every implementor. This is agreement, not devirtualization: the caller
    never needs to know which implementation runs, so a multi-implementor
    interface still yields a fact when the implementors agree. The meet is
    fail-closed on an unresolvable target, an implementor whose own ownership
    is unproven, any disagreement, and an empty implementor set. Making the
    meet require a single implementor, or letting it answer from a subset of
    implementors, is a contract change.
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

anchor-sha256: src/frontend/analyzer/xa_effect_db.h 3f8e0952e2b25291fa4aaeb96baa05197f789c4292db2ec2291da407d9724b01
anchor-sha256: src/frontend/analyzer/xa_effect_db.c bbf0a9f9fd78e8daf7301437bcaace000fd4d22ac91167d79ca7b71307814a7f
anchor-sha256: src/frontend/analyzer/xa_memory_effect_db.h 4a2527c4da62c7238c5df9f13b4fbcf9e210bb3555745425ace07b3704e674c3
anchor-sha256: src/frontend/analyzer/xa_memory_effect_db.c 1c3b0121cb1d9814189b615c7a5314a4dc873d1ef7ab87d86ed6deb7ba51a5e0
anchor-sha256: src/frontend/analyzer/xanalyzer_errorset.c c2ac036bb86b8e760961e571ae24a326173de68a35675a50fcd366b0ad5ef8de
anchor-sha256: src/frontend/analyzer/xanalyzer_allocation.c 9ee98106c86e153d1aee935862a2065fb24bd79036283bc26750018f120c3290
anchor-sha256: src/frontend/analyzer/xanalyzer_suspend.c b5447f9c3826852dea8fd79da5b09706b3c73a6e66d072af9901ef38a28b20d7
anchor-sha256: src/frontend/analyzer/xanalyzer_memory_effect.c 19585145d88b00d1c1e4fad9fe23ac841e75c941eeaf7c18be3779befc872367
anchor-sha256: src/frontend/analyzer/xa_typed_program.c dc666a71819aa81f3573754e55626d8bec56766e16eed6191cbcfa293914b723
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_internal.h 47c711827b9cb65225516cb2c772b0338f20f690b6ff92b0edf531505faabe82
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_decl.c 266c952fd177c41a2b288a44f3b9b699e3eb5821bb9cc3359a5638f1aeb99cd3
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_stmt.c 02940d35a66fa77ad3e67a9a7b11322f48c87a7af6a2829760ad514f65985b50
anchor-sha256: src/runtime/value/xtype.h e0f9c44c615d8a91f501d3952b1804c793abe68e3eba39744c9a000ae00f10cf
anchor-sha256: src/ir/xi.h c215b18a09e101d32ce662289ed2ac2b6b8d778c36e5c40cb1a6724d5a06f661
anchor-sha256: src/ir/xi_lower.c 66376000591f3126f5d0202134c502b3f2b048f31e29337ea4f09e6f433e79af
anchor-sha256: src/app/cli/xcmd_verify.c 5fd6d66c6bc2c4be29cb121963eea94682cb48ea20f42aacdeb52fb2a2285b9b
anchor-sha256: tests/cli/run_verify_contract_tests.py 5478ddddc8b0ad7ee001e901ceb2a1b4f44c57cee48032ac438f4f7f9187ce18
anchor-sha256: tests/unit/analyzer/test_analyzer.c 34bbb2512ed40c85db6fe91bf6da083abc457792e3e3b76ab56611dce7ccb1ca
anchor-sha256: tests/unit/analyzer/test_effect_db.c 15b62bd4e820af1d1798476afe61459372218e26b83db65d00a0f40cb2002bf1
anchor-sha256: tests/unit/ir/test_xi_lower.c fbcb2ea7d98487c81c049b936716158b2090ec5f0ef02e29a374867867e77fc7
anchor-sha256: src/frontend/analyzer/xanalyzer.c 4c74a794312a006324974d456bece12e1073d3ab3e3e044c49705c19eaf23f8a
anchor-sha256: src/frontend/analyzer/xanalyzer.h 286b7887eb943763de2e9494df62eef875074bfbe67aa4c0ffcb9c6cda031741
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_call.c db1c1bfdce0e098e294852bc67faf37ad8c3bc4ecbfd9410a8698a82132acc28
anchor-sha256: src/plan/format/xr_xsm_encode.c e56ac921cb9c4be957086f9b6b960c5d194a04b066986ddefafc9efc838e8876
anchor-sha256: src/plan/format/xr_xsm_schema.h 98fc9a9c8f4627de81075e25905a55189ce82f5b985b190a6bfaa6ce72810242
anchor-sha256: src/plan/semantic/xr_semantic_builder.c d6cff3d360ad33b7eccfcb46a456a931e73ded4e24c2c64be46b88a14c7ab8cb
anchor-sha256: src/plan/semantic/xr_semantic_ids.h 6f5048cf95e341617e68eba47ffbf51ae396b72f9771328ff2ba8f8f500bf459
anchor-sha256: src/plan/semantic/xr_semantic_plan.c ea4675808fc8b4cc1bd3539b4b437756b9b0b92afd5a2afd1dd17ad9b1e9e7b0
anchor-sha256: src/plan/semantic/xr_semantic_plan.h 83486fbaa7cdcfd43285326bf43bcc4e37236b4813d858c9bf35d20b8e3a71a9
anchor-sha256: src/plan/semantic/xr_semantic_plan_internal.h 63905a40cb54d913e4a9366c0ed29116b5f6d482ac90100da16add5ebf366966
anchor-sha256: src/plan/semantic/xr_semantic_verify.c 6e06bea77035872d2bd79ecddb36d6e3a519da4b2df9c53bd84dbda866c7817e
anchor-sha256: src/stdlib/xstdlib_metadata.h 4f0d9628ff18ec6522c48bf602a9ba738813cb1a11f2d059dc7d9c7daf179c14
anchor-sha256: tests/unit/plan/test_semantic_plan.c c78d240af78d32c8c39e129219cfe0427e778fb9428d46dbd337525c529b35e7
