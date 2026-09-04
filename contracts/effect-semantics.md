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
SemanticPlan schema 39 preserves exact `String.runes()` builtin-member identity
and the sealed `Iterator<rune>.next()` result reached directly from it as frozen
intrinsics. The latter requires one unique same-function `String.runes()` result;
an Iterator type or selector alone is not authority. The schema also retains the
exact `rune.toUInt32()` intrinsic for an exact canonical Rune SSA receiver,
including a parameter, phi, literal, or iterator result. The exact method symbol,
non-null native `u32` result, borrowed Rune operand, and call ownership are the
authority; the operation that produced the Rune is not part of the conversion
semantics. The independent verifier reconstructs those facts without using the
builder's complete-answer predicate, and later phases cannot reconstruct the
identity from selector, live type, or argument count. The schema also retains the
exact `rune.isWhitespace()` intrinsic only for that same uniquely proven Rune;
its exact bool result and call contract cannot be reconstructed from selector,
live type, or arity. The schema separately freezes `string.fromUtf8` and
`string.fromUtf8Lossy` only as exact static calls on the compiler-owned String
namespace. The numeric method symbol and selector must agree, the input must
be exactly `Array<u8>` or `Slice<u8>`, and the result must be one owned String.
This family does not authorize a same-spelled source member, a dynamic
receiver, another element type, or generic method dispatch. Target and AOT
consumers rebuild the shared judgement from frozen rows.
For ordinary parameter admission, a top-level `const` difference is erased
only at a `READ` boundary and only when every other canonical type component
matches. `REF`, `OUT`, and consuming boundaries retain exact constness, and
nested element constness is never erased. Target call ABI verification admits
the corresponding read boundary only when caller and callee machine rows have
the same kind, size, alignment, ownership, and layout detail after removing
that one top-level const bit. The schema also retains the
exact three-operand `String.slice(start, end)` identity only when its receiver
is a unique frozen required String parameter or exact String literal and its
two ordered bounds are exact native i64 values. Its owned return provenance,
tail flag, operand ownership, and fixed selector identity are all frozen; later
phases cannot reconstruct this authority from selector, live type, or arity.
The schema additionally freezes only the exact two-operand scalar
`Array.fill(value)` method form: its array receiver, scalar element storage,
fill argument, receiver-alias result, ownership, effects, and selector metadata
are canonical facts. This scalar family still excludes range overloads and
non-scalar elements. Separately, the sealed array-member family admits exactly
the four-operand `Array<source-class>.fill(value, start, end)` shape: the frozen
selector and numeric method identity, exact source-class element, consumed
tagged fill value, and two ordered signed-i64 bounds are all required. Live Xi
types and selector spelling alone remain unavailable. The schema additionally
freezes the first complete
coroutine-owned value lifecycle. Only an exact fresh owned String concat whose
producer dominates one frozen suspension state and whose unique exact release
post-dominates that state qualifies. Three canonical entities record LIVE,
ROOT, and DROP with the same state, producer, owner, and release identities.
The producer and independent verifier each rebuild CFG dominance/
post-dominance, String concat and release ownership, exact entity keys,
uniqueness, and complete triple coverage from their own dense release, state,
owner, and lifecycle projections. Every operation, operand, ownership event,
entity, state-by-release pair, sort bound, and indexed lookup is charged
through checked arithmetic against one 100,000,000-work ceiling before the
corresponding projection allocation or sort. A plan with no state and many
release-shaped operations stays linear, while the exact maximum-plus-one
preflight fails before touching its intentionally absent operation table. The
registered mutation gate refuses restored per-row release/owner scans in the
Semantic producer, Semantic verifier, or either Target consumer. Types, opcode
spelling, or backend liveness are not fallback authority. The schema also retains the
pointer-free `DIRECT_LOCAL` call-target
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
Schema 23 also freezes `core.string.bytes` as an exact non-allocating borrowed
`Slice<u8>` view. Its record binds the numeric intrinsic identity, String
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
A member call on a compiler-owned container receiver carries its own numeric
intrinsic identity on the same terms. The array member authority is frozen when
the receiver type record is an array with no source-class index and no
source-class identity, the selector is the sole frozen metadata entry and names
one of the frozen shapes below, every call-contract argument is consumed, and
the result matches the shape the selector states. No declaration can produce an
array receiver record, so the container kind plus the selector names one
implementation, and a declared member that happens to share the selector never
reaches this authority. Each frozen shape states the operand count range it
admits, which operand carries the element, and what the result is: `push` and
`unshift` consume one element operand and return the unit type with no alias and
no returned ownership; `indexOf` and `contains` consume one element operand and
return an exact signed 64-bit integer or an exact boolean on the same terms;
`fill`, `reverse` and `sort` return the receiver itself, recorded as an alias of
operand 0. The element operand is proven against the receiver's own element
entry and every remaining argument is an exact signed 64-bit bound. Each shape
now freezes element access, reference
action, and drop lifecycle instead of publishing a permission bit. `reverse`,
`sort`, and `join` preserve references already in the container. The reference-
capable storing forms are exact `Array<T>.push(value)` for the closed exact
String, frozen source-class, and compiler-owned Array roster, plus exact
`Array<source-class>.fill(value, start, end)`. Push moves one ownership root
into one tagged slot; range fill remains narrower because it duplicates one
input across several slots. The canonical Array lifecycle releases every
stored root when erased or when the container is destroyed. TargetPlan binds
two ordered rows for `push` and four for range `fill`: the dynamic receiver is
borrowed, the exact owned managed element is consumed with `TAGGED` call
storage, and the two fill bounds are consumed trivial i64 values with no array
storage.
Its independent verifier reconstructs selector plus numeric method identity,
lifecycle, source-class identity, every ordered semantic operand, stable
argument identity, ownership, storage, and caller representation. Shorter
source-class `fill` forms, every Array-element `fill`, `unshift`, `indexOf`,
and `contains` remain
unavailable for reference-capable elements, as do every unknown shape and every
operand count outside the frozen range.
`Array.reserve` is the closed stable-identity member of this container family:
the analyzer records `core.array.reserve`, SemanticPlan freezes the exact array
receiver, signed capacity operand, receiver-alias result, write/may-throw
effects, and empty selector metadata, and independent verification reconstructs
the same shape. A selector, mutable Xi type, or legacy builtin auxiliary string
cannot authorize it.
A member call on an imported native stdlib module namespace carries its own
numeric intrinsic identity, stated once for every module rather than per module
name. The receiver is proven from the rows: a module shared-slot read whose
slot is published by exactly one module-init store, whose stored value is the
module-init import reference. That import record resolves against the native
definition registry rather than against a compiled module and names the module
path with an empty member, so a source-module namespace and a selected member
import both stay outside. The frozen definition registry must then name exactly
one entry for the complete module-path, selector, and callsite-arity identity, a
single returned value, an ordinary non-suspending binding, no conditional
compilation, no result enum, no runtime capability, and every argument crossing
as one plain tagged value. The result and every argument must be a plain
machine scalar with no reference, so the row leaves no ownership obligation
behind the call. The `builtin` AOT form of a member is refused: the native
backend rewrites those callsites into a different operation after this plan is
frozen, so no frozen row can describe the shape the backend emits. A local
function sharing the selector is an ordinary call through a callee operand and
never reaches this authority. Distinct arities are independent overloads;
duplicate registry rows with the same complete identity remain ambiguous and
fail closed.
When the independently proven target reaches a function with a canonical
static suspend operation (or `XI_GO`), the same row also authorizes exactly one
coroutine-state entity for an ordinary call. A direct tail-call edge propagates
the target's suspendability to its caller but has no resume state of its own.
The record never copies caller-authored
effects, provider spellings, symbols, pointers, or raw digests. Schema 17 retains
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
Caller and callee rows come from two plans, so an argument is admitted by the
canonical type key rather than by one shared stable id. Beyond an exact key
match and membership of a union parameter, the caller may hand a definite value
to a parameter that only widens it to the nullable form of the same type, which
is the language's own rule. An explicit null argument is likewise admitted only
when the frozen parameter is nullable, reference-capable, neither a value type
nor unknown. The widening is offered only where null is already one of the
values the representation encodes, so a reference-capable type is admitted and
a nullable scalar, which carries a separate discriminant, is not: admitting one
here would drop the adapter its call needs. Every remaining field of the two
keys must match, so a difference in constness, value semantics, element, name
or declaring class stays inadmissible. Semantic module-set verification and
target planning consume this one canonical admission rule independently of any
builder cache or planner result.
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
yieldable registry definition. A resolver-proven source-module import is also
admissible when that same namespace owns native registry leaves: source-export
resolution retains first refusal and owns receiver storage, while the exact
registry member owns only the native yieldable call target. The verifier
reconstructs the shared-store, root-prefix, import-resolution, registry, arity,
and one-to-one coroutine-state relation from frozen rows. Source-member
shadowing, unresolved imports, selective imports, private-wrapper substitution,
builtin calls, tail calls, ordinary methods, and non-yieldable definitions
remain unavailable. This family freezes coroutine obligation only; TargetPlan
dispatch and execution continue to fail closed until a separate target family
consumes it.
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
The Xi coroutine plan retains source exceptional-continuation authority at
every suspension point. An ordinary error catch is identified by its
arena-owned lexical error-region record and exact `XI_ERR_CATCH` value; a panic
catch is identified by the ordered, properly nested `XI_TRY` registrations and
their unique `XI_CATCH` blocks. The plan records the innermost error
continuation, the complete outer-to-inner panic-handler stack, and the
precomputed root/drop sets. Normal resume preserves the state continuation,
error and panic route only to their recorded non-terminal continuations, and
cancel/drop remain terminal cleanup edges. An independent verifier rebuilds
the regions from CFG and registration identity and rejects stale, missing,
extra, reordered, non-nested, or mutated edge/action rows. Cleanup-local panic
handlers remain unavailable across suspension. Opcode names, selector text,
types, and backend rediscovery grant no fallback authority.
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
enum name, ordered member names, payload counts, and each payload member's
ordered nonempty unique field names and instantiated field types; the row also
freezes the independently recomputable nominal layout ID, member count, and
unit-enum flag. Analyzer-local enum IDs, pointers, display names alone, empty
field-name sentinels, and backend type guesses are never authority. XSM has a
hard schema cutover and the independent verifier reconstructs the stable
identity, layout hash, field-name/type sequence, and canonical type-key link.
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
    `XR_CONVERSION_ENUM_ORDINAL` is a distinct non-numeric-source witness for
    an explicit unsafe enum-to-integer conversion. It is non-throwing and never
    carries `XI_FLAG_MAY_THROW`; its source is an exact enum/member ordinal and
    its destination is an exact native integer representation. Compact versus
    tagged source storage is owned by the frozen representation plan. Missing,
    dynamic, mixed, or forged authority fails closed.
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

Exact scalar parsing has one typed failure product. `i64.parse` and
`f64.parse` may produce only `NumberParseError.InvalidSyntax` or
`NumberParseError.OutOfRange`, and lowering must place a real `XI_ERR_CHECK`
after the parse operation. Publishing that typed error aggregate is a
`MAY_ALLOC`/`MAY_HEAP` path. `i64.tryParse` and `f64.tryParse` return an
optional value and have no error effect, allocation effect, or pending-error
write. The shared parser owns failure classification; VM and AOT may publish
the frozen member but may not reclassify syntax and range failures
independently.

Constructing a declared class that declares no instance constructor runs no
user body: it allocates the instance and applies the declared field defaults.
The whole-program callsite evidence names that construction in its own right
instead of leaving it on the open closure kind, so its composed effect set is
empty and it marks no reachable body, exactly as a sealed native call does. The
row names the declared class and no callee at all; a method identity on such a
row is refused as stale, and a class that does declare an instance constructor
keeps the ordinary method callsite and composes that body's effects.

SemanticPlan schema 45 names local and imported construction without erasing
their module boundary. A local construction target names only the declaration:
the instance result and the class object loaded from its unique local shared
slot must name the same frozen source class. An imported construction target
instead binds the ordered dependency, an explicitly tagged source-class export,
the exported declaration stable identity, and the dependency constructor stable
identity when one exists. The dependency source-export row is a tagged union;
function and source-class indexes are mutually exclusive, and the exported
entity identity is serialized rather than rediscovered from the initializer by
Target or AOT. Semantic construction proves the named `XI_IMPORT_REF` through
the caller root store, while module-set verification independently repeats the
import/member/dependency/export/class/constructor and argument proof. A
duplicated store or definition, mismatched export kind or entity, missing exact
constructor parameter, or call whose effects differ from the generated call
effects names nothing. There is no local-shared-slot or function-export
fallback for an imported class. The target grounds no coroutine-state
expectation of its own.

An unresolved function-typed callee loaded through a shared slot is published
as `INDIRECT_CALLABLE` only when coroutine lowering has frozen that exact call
as a suspend point. The builder reads the live coroutine plan; the independent
verifier reads the serialized coroutine-state entity and the callable operand
type. Synchronous shared callable loads therefore do not acquire invented
suspension, while a missing target, removed state, or mismatched callable type
fails closed.

The admitted scalar, leaf-value, and bounded two-module scalar graph families
consume frozen PSC and Xi authority as typed external construction/verification
inputs. The scalar family also requires its sealed CallDecision and exact
TargetProfile; the leaf-value and graph families require both to be absent.
SemanticPlan schema 45 and program-provenance schema 5 project the graph into
one exact plan per Xi partition. The zero-dependency producer carries its pure
unary function/export authority; the entry carries its pure nullary function,
ordered dependency, resolver binding, program call, and `SOURCE_EXPORT` target.
Xi and SemanticPlan module-set verifiers independently rejoin both live
partitions before XSM/cache publication. Semantic effect rows do not themselves
own target layout, ABI, dispatch, or execution answers. Their typed authority is
instead joined once into the unique Program TargetPlan. Same-plan VM lowering
and the bounded source-AOT `PROGRAM_DIRECT`/`CALL_DIRECT_I64` path consume that
target authority. For the exact two-module scalar graph, the verified program
C-emission binding mechanically supplies caller/callee/initializer value and
ABI views, the canonical direct symbol and reachability edge, and the exact
elided shared carrier; its generated-C/native cold, warm, edit, and revert
oracle is admitted as `execution=cgen-ready`. A separate public `XrProgram`
facade admits only this exact two-partition/two-function/one-call/one-argument
direct-`i64` graph through the same verified Program TargetPlan and live
manifest. This does not admit other graph shapes, dynamic reload, concurrent
unload, or `PRODUCT_ACTIVE`.
These schemas retain no compiler pointer or target-specific bytes. They
serialize and fingerprint the PSC schema/family/fingerprint,
GenerationClosureId, exact type/type-field/function/dependency/call row bindings, preserved
function flags, and their semantic type/function/operation joins. Single-module
families bind an exact direct-local target; the graph entry binds its external
target through the program dependency, resolver, `SOURCE_EXPORT`, and callee
stable identities. The generic verifier checks the complete pointer-free layout after
construction and XSM round-trip; the external verifier independently joins it
back to PSC and Xi. For each covered `XI_CALL`, neither verifier invokes the
generic direct, native, namespace, indirect, class, method, name, or body
resolver. The operation retains generated conservative CALL effects. Missing,
mixed bound/unbound, reordered, or mismatched authority fails closed rather than
falling back to another call-target or effect interpretation. The semantic
projection supplies no aggregate layout, slot, ABI, VM, or AOT answer by
itself; those answers exist only after the independent Program TargetPlan and
downstream execution bindings have joined and verified the typed rows for an
admitted family.

## Digest anchors

anchor-sha256: src/frontend/analyzer/xa_effect_db.h 3f8e0952e2b25291fa4aaeb96baa05197f789c4292db2ec2291da407d9724b01
anchor-sha256: src/frontend/analyzer/xa_effect_db.c bbf0a9f9fd78e8daf7301437bcaace000fd4d22ac91167d79ca7b71307814a7f
anchor-sha256: src/frontend/analyzer/xa_memory_effect_db.h 4a2527c4da62c7238c5df9f13b4fbcf9e210bb3555745425ace07b3704e674c3
anchor-sha256: src/frontend/analyzer/xa_memory_effect_db.c 1c3b0121cb1d9814189b615c7a5314a4dc873d1ef7ab87d86ed6deb7ba51a5e0
anchor-sha256: src/frontend/analyzer/xanalyzer_errorset.c 46bb2c02ec15f6bcbf0eb41a97333f93b96380958748fff26e48e363f70a38c7
anchor-sha256: src/frontend/analyzer/xanalyzer_allocation.c ceeb7a45b38b0d3632100f6716bb9cc39359b8942008c8fbcea15fb6db1375dc
anchor-sha256: src/frontend/analyzer/xanalyzer_suspend.c b5447f9c3826852dea8fd79da5b09706b3c73a6e66d072af9901ef38a28b20d7
anchor-sha256: src/frontend/analyzer/xanalyzer_memory_effect.c 37ded58432af0c583c64271fd3600dcecfabd6f552e7513e0d8db164e57d96f0
anchor-sha256: src/frontend/analyzer/xa_typed_program.c 4ded6b8892f1f6dd254d528109492caba805380e7c3a96bb3c10e7a01f0e80c3
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_internal.h a8921aeb7fb69e437cc935f9a493a9768a24e42fe2dca194e456308f5db81d4f
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_decl.c 5a81a52ce33aaeb99a088cf4076e44ab45e01171c442c6cc157db5149e66ad16
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_stmt.c a86799d1b44add6a95f07c2838ccee625c4eeb08d27a8f4f1e009354f5f02869
anchor-sha256: src/runtime/value/xtype.h e8379d7f493b364c1a1cb1e57d3c39a15ec142a53341dc9334ede38534be10ec
anchor-sha256: src/ir/xi.h 3160abfd948d12cbe076ed8d1c24815ec3e56eb32ec38f4989e8fea7deb4e3dd
anchor-sha256: src/ir/xi_lower.c 689908399810b5fa026a91c71bcd57479f86f17afe4f215d5ae1086add2e6630
anchor-sha256: src/app/cli/xcmd_verify.c 4d806bacb7a94efeba2d3d05e1ef657596fb7cbac2f315aff0d40f0e4de49629
anchor-sha256: tests/cli/run_verify_contract_tests.py 5478ddddc8b0ad7ee001e901ceb2a1b4f44c57cee48032ac438f4f7f9187ce18
anchor-sha256: tests/unit/analyzer/test_analyzer.c dadc4674624aeccaf46fa8e15eed8c51fafd72d695360211fe005732d9e9c415
anchor-sha256: tests/unit/analyzer/test_effect_db.c 15b62bd4e820af1d1798476afe61459372218e26b83db65d00a0f40cb2002bf1
anchor-sha256: tests/unit/ir/test_xi_lower.c cd4e8a7917f5434c1456d1bfdda5970767939c6745eb677ad8a455bd0010853e
anchor-sha256: src/frontend/analyzer/xanalyzer.c b69acdd9c2bebb9f00a705988e9634f72378c8c6081320647e02c11b1bd1e539
anchor-sha256: src/frontend/analyzer/xanalyzer.h 6995fef695bc1ee724ec1404ee5b6691f8ba703178a318ba2e973710f943d674
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_call.c 62812f59be934d3a0b660efff8dd177e439934145ee7cc2fb93233766302ad06
anchor-sha256: src/plan/format/xr_xsm_decode.c b31bf1696bacd3b435ea1383da4f92df51bb6692c45f28e7d22ab829154db8f4
anchor-sha256: src/plan/format/xr_xsm_encode.c 35840e929f9e86086cd57790af43eb4df6b84060704eba9045bdc9b40f579f2c
anchor-sha256: src/plan/format/xr_xsm_schema.h f5e6d875255f73803545a9cf99450e6b140e6282ee19233048afd4e0ce41362b
anchor-sha256: src/plan/semantic/xr_semantic_builder.c c24fc84d3f3e3112862af9a1e35fc22da343044b1998afcbb7e0f9052819ff01
anchor-sha256: src/plan/semantic/xr_semantic_cleanup_shape.h 9a2baf1ef059831b54641bd832b85a5279555dedd244f23b631fac349f45638d
anchor-sha256: src/plan/semantic/xr_semantic_coroutine_lifecycle_shape.h 82e14aa7ee4ae5ad18dcd9101016aee6869e8abe24110d979ae470fac715df45
anchor-sha256: src/plan/semantic/xr_semantic_enum_shape.h 16dd118c2a3c7fe472dd6dbd0f09723d4c00c2125c4d037ecb5a9eae650f33be
anchor-sha256: src/plan/semantic/xr_semantic_ids.h cbf2be1b8af3a9a96d91f8908bfd91a1dd05dc026bd61b076ec3ad979e8229a3
anchor-sha256: src/plan/semantic/xr_semantic_plan.c 0f78c911fd05636a4717ec9d4d0b8b5db3d8a669a5a680b367960cc8d7923d66
anchor-sha256: src/plan/semantic/xr_semantic_plan.h 23e070e3dfa6bd4c1d68151429ddfd27e50190f177fdb65072866632fffbc70f
anchor-sha256: src/plan/semantic/xr_semantic_plan_internal.h fbe1eb29e08425a629dda4c281f7a681ab48512c599cae9b63b379f4db338d2e
anchor-sha256: src/plan/semantic/xr_semantic_type_admission_shape.h b69c09a68349ea8bc126bdd5e72d23b06fd1848b43b1dddff77346c6c2cc2801
anchor-sha256: src/plan/semantic/xr_semantic_string_runes_shape.h f5725458cdd6af16c555c1a8145aea90fb7f1b50cd599420590f2cfbb96980f2
anchor-sha256: src/plan/semantic/xr_semantic_string_slice_shape.h 2b0db2abc1652ec45f6a8090ad973cd60bafe039eb4f64c7d0e38674fd388dce
anchor-sha256: src/plan/semantic/xr_semantic_string_utf8_shape.h aa8a342b9578e749c5e812dc9d193220ac63d849e15085130a4279ead5c24056
anchor-sha256: src/plan/semantic/xr_semantic_iterator_rune_has_next_shape.h 520152cb6e93b1cdd6639e094772a652905206e97bf15677ba753eecb4d075f6
anchor-sha256: src/plan/semantic/xr_semantic_iterator_rune_next_shape.h 4e4ac253f3837afde84345a2ea24a548f6c18378024ca9ac131ab3ad482433fd
anchor-sha256: src/plan/semantic/xr_semantic_rune_to_uint32_shape.h a781d061082d479ea0483a8a77237bd77dd0f2c0aadc866de482012d6dda7cae
anchor-sha256: src/plan/semantic/xr_semantic_rune_is_whitespace_shape.h 5ec6db5acd0d2c15ad5e6c292531b8dcfc9fdbde7addcb28c69a790586b57f5c
anchor-sha256: src/plan/semantic/xr_semantic_verify.c 09e922a5fe68c03852b6f4d94b4d6b79ea5b2b2c1a9f770ee9520832801780df
anchor-sha256: scripts/check_coroutine_lifecycle_projection.py 532959558cb72938709198f481ac42d53ec074ca0602b5d4c4512568db908f1a
anchor-sha256: src/stdlib/xstdlib_metadata.h 8e9e7c5f25d194dfd16bc05e92b895704fd7578c17c6970af0dd341f38d9efe4
anchor-sha256: tests/unit/plan/test_semantic_plan.c 5878fe725976652a270ed4364bdb78f56d93502af0b6d7ca7c385b74ca0feca9
anchor-sha256: src/frontend/analyzer/xa_native_member_contract.def f2fec1dbe429556d947a2548cdf657698b712b75cd90a2cb2f4a3eb2ac175b79
anchor-sha256: src/plan/semantic/xr_semantic_number_parse_error_shape.h 1a31a79d9b4e705850d225c76f0fe9d8b4698d0a06a6c5d0223e6323b9a7dcfb
anchor-sha256: src/shared/xr_string_parse_core.h e96e12444c85ef8d64e2b6ab0baa8b8e761c7f3636049f9f10420fe6184ad5a1
