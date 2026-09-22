# Canonical XrProgram VM contract

VM code construction consumes only `XrValidatedProgram` and a verified `XrTargetProfile`; execution requires one active `XrInstance`. It does not decode program
bytes, call the reference evaluator, reconstruct source types or effects, or consult TargetPlan,
legacy Proto bytecode, or AOT. Every active CoreSpec operation has one explicit typed handler.

`XrProgram` remains the only distributed executable program format. `XrVmCode` is private runtime
state and has no serializer, reader, compatibility promise, or public install header. The baseline
view walks immutable validated rows. No second instruction graph, decode selection API,
copy/free path or fixed-view coverage projection remains. Adaptive quickening remains disabled until a measured policy and its
traceability proof exist.

VM code is qualified by `ExecutionId`, VM build identity and quickening policy.
Construction owns a Program reference and does not require a live provider binding or generation.
Execution separately checks Program/Profile compatibility and pins the actual instance lease.
A compatible successor or independent instance can reuse code; a retired instance cannot execute.
Provider admission and binding-context lifetime remain instance-owned, and resource values or
leases from one instance do not gain authority in another by sharing an ExecutionId. A private
view can always be discarded and rebuilt from `XrValidatedProgram`; corruption or mismatch cannot
fall back to an older executor.

The source builder retains explicit named roots from its authoritative import graph
alongside the single default entry and every module initializer. Each root binds
canonical module identity, exact source bytes and its resolved declaration/body;
Xi selection uses that body identity rather than repeating a name lookup. The
writer closes all selected call graphs, validates one Program, then publishes
Program-local FunctionIds using the wire writer's canonical key order. Request
order changes only the returned mapping. The product owns the mapping independently
of source buffers and compiler lifetime; stale identities, duplicate selections or
publication allocation failure leave no partial product. This is shared compiler
support for test/hooks and export entry retention.

The source owner discovers entry-module test and hook declarations before the
single closed-world build. Product-owned names, roles, timeouts and exact
FunctionIds survive compiler teardown; imported tests are not implicitly run.
The test CLI now consumes that product through the same provider binding,
explicit VM frame and instance owner as source run. Proto discovery, closure
lookup, a separate test compiler graph and the old scheduler watchdog are removed.
Filtering, skipped cases, hook order, failure continuation, fail-fast teardown,
file isolation and rejection of zero executed tests have independent CLI checks.
After-hook failures are reported instead of silently ignored.

An execution-local interrupt callback can be installed before the first step.
The shared operation loop polls it across ordinary calls, resumable calls and
initializers; Code and Instance do not retain callback state. Interruption uses
the existing resource-limit terminal cleanup, including physical owner release.
The CLI bounds timer waits with the same monotonic deadline. This is not a claim
of preemption inside a blocking host-provider call or of verified cancellation
edge execution on resource interruption. Complete product error payloads and
diagnostics, blocking-provider interruption and full qualification remain open.

Coroutine execution preserves the same boolean constants, selected conditional edges, and
parallel block-argument assignment as ordinary execution. Each execution owns bounded temporary
edge storage: it snapshots the selected payload and any implicit result before overwriting target
arguments, including on a self-loop. That storage is private to the parent or child execution and
is released with it. Multi-block cancellation and provider-failure cleanup follow only validated
reason-private edges; no executor-local search for a convenient terminal is permitted.

Ordinary calls and resumable frames now dispatch through one operation loop.
The separate coroutine operation whitelist and its duplicate arithmetic,
ownership, place, provider and control-flow handlers are removed. An ordinary
call uses a temporary frame with the caller's execution context; a suspended
frame keeps its values and verified continuation. Both use the same bounded
value/edge allocation and terminal lease/trace publication. The explicit frame
API also accepts ordinary entries, whose returned views remain frame-owned.
One-shot execution still rejects a resumable entry because it cannot retain a
suspended session. Cancellation still follows only a verified suspension edge.

The shared operation fixtures execute through both one-shot and explicit-frame
entries using the validated graph view. Independent outcomes, step counts and nonzero
traces remain required. A dedicated identity test checks deterministic repeated-entry
traces, unchanged identity under execution budgets, and distinct target-profile identity.
Provider/text expectations keep exact byte streams and refusal effects with one VM route.
Fixed-row storage and duplicate view matrices are removed; independent VM, backend IR
and generated-native H2 expectations remain. Yield followed by signed division
returns 20 for 40/2 and publishes the existing division-by-zero trap for 40/0;
both terminal paths release their lease. These assertions replace the old
coroutine-only subset, without adding an operation or changing Program identity.

The `xray_program_vm_runtime` archive is the embeddable product boundary for this stage. Its exact
source closure contains the CoreSpec projection, XrProgram decoder/verifier, immutable target
profile, BoundaryABI/runtime contracts, execution instance, and typed VM. The target-neutral
provider logical decoder is part of that runtime closure. It excludes declaration generation,
native host-binding construction, frontend, CoreIR writer, reference evaluator, TargetPlan,
legacy Proto VM, and AOT. The runtime-only test
links a source-committed canonical XrProgram artifact against this archive and verifies the
resulting symbol closure. The artifact is deliberately outside the executor build graph: schema or
CoreSpec drift fails closed during validation instead of rebuilding and executing a compiler-side
fixture writer whenever VM or verifier sources change. An excluded, opt-in writer remains available
to regenerate the committed artifact when its schema intentionally changes; it is never a runtime
test dependency.

The executor covers all current CoreSpec operations. `core.logical.not`,
`core.logical.and`, and `core.logical.or` operate only on canonical `bool` SSA values. The binary
operations are eager at the Program level because their operands are already evaluated; source
`&&` and `||` expressions whose right-hand side can trap or perform effects are instead projected
as explicit control flow, preserving source short-circuit behavior without executor-local policy.
Indirect calls borrow an affine
callable without consuming it, whether the current SSA value owns the callable or is a non-owner
`READ` function parameter; the callable SignatureId remains the only visible dispatch contract.
For a suspending indirect call, an owned carrier is admitted only when the same owner occurs exactly
once in the safepoint and cancel segment. Captured closures keep their environment in that private
owned carrier, while a child receiving the callable as `READ` sees only a non-owner frame-stable
value. Normal resume can forward the owner into another suspension; the final normal or cancel path
drops it exactly once.
Provider calls use shape-specific lease-pinned trampolines for nullary/unary i64,
`bool(i64)`, nullary optional-i64-pair, and byte-sink operations. The byte sink is checked
byte-for-byte against the reference evaluator under both VM decode policies. The Pipe source
slice additionally verifies exact open/close operation identities, endpoint projection, taking
optional projection, a `MOVE` close receiver, and two close calls under one pinned execution
generation. A real-source `time.now()` refusal fixture proves that the independent reference and
VM executors each issue exactly one provider event and publish the same provider-call-failed trap.
Explicit Program trap continuations carry every live owner into the same static Pipe cleanup when
provider failure crosses direct, sealed-direct, indirect-direct, indirect-invoke, witness-direct,
witness-invoke, or sealed-invoke calls. A two-frame source fixture gives the callee and caller
distinct owned Pipes and proves LIFO composition: the inner two closes precede the outer two closes
before the final trap. The executor transfers only trap 7 through each verified edge; it does not
synthesize an unwind policy or catch other traps. The Pipe
slice uses natural `!`, `&&`, and `||` syntax and guarded division-by-zero right-hand
sides to verify the same result and short-circuit behavior in the reference evaluator and the
source-path VM. A dedicated logical-operation fixture covers both VM decode policies. Wave 1 source activates scalar,
control, block arguments, and sealed calls; Wave 2 adds logical aggregate construction/projection
and variant construction/test/projection. Aggregate and variant values use VM-private typed arena
cells and never expose a shared physical layout. A one-shot string, class or aggregate return or ordinary typed error transfers its reachable
value graph into one outcome owner retaining Code and the instance lease. It preserves
identity without recursive copying and releases the graph through the normal typed drop
path. The caller may inspect typed views after releasing its Code reference and must dispose
the outcome before instance retirement. Independent return/error allocation-failure probes
and class finalization/reclamation observations cover the shared owner.
Explicit execution frames retain the terminal dynamic owner and instance lease until frame
free; step/cancel results borrow that frame. The former class-result rejection is removed,
and normal typed drop runs exactly once before releasing the frame's lease. Borrowed views
must not escape their owning outcome or frame.
Captured callable results use that same graph owner, including their capture carrier.
A returned callable can be passed as a READ argument to a subsequent invocation; independent
fixtures require the captured value 42 through both one-shot and explicit-frame returns.
Per-allocation failures, Code release and retirement blocked until the final owner is disposed
are covered. Existentials with initialized owned storage also transfer their full payload graph
through this owner. An independent returned interface contains a string-bearing value struct;
a later witness invocation reads its string length as 42 after the original execution ended.
The frame and outcome retain separate leases, and failure injection covers the complete graph.
Borrowed READ/REF and MOVE-view existential host results remain outside this qualified return
boundary; owned-storage support does not replace their required origin and transfer proofs.
An initializer failure instead transfers its reachable owned graph into the instance's immutable
failure owner before reverse publication cleanup. Repeated entry observes the original error or
panic channel, not a synthetic trap. Borrowed error views keep their entry lease until frame free;
one-shot errors retain an opaque instance lease until outcome disposal. This preserves affine
payload identity without copying it and prevents retirement while a failure is observed. Failure
to allocate a host keepalive does not replace an already published error. Failure to publish the
owned graph yields sticky resource exhaustion and still cleans all acquired owners. Separate
allocation-failure and lifecycle checks retain exact finalization, reclamation and reverse module
cleanup responsibilities. The CLI's shared invocation owner now formats errors before freeing
the observing execution. Run streams the borrowed value; test captures its original diagnostic
within the existing bounded failure record and continues through teardown. A borrowed VM
layout reader supplies scalar/string/rune and enum fields to the runtime-neutral formatter.
Native retention has independent lifecycle checks; complete class/struct display, task reports
and PanicInfo diagnostics remain separate unfinished work.
Aggregate update and the walking-skeleton
trap/error/profile rows remain source-gated by the operation matrix even though their typed handlers
exist. Native LP64 and explicit foreign ILP32 profiles, resource limits, and generation invalidation
remain covered. Full-language operation families, public embedding ABI, adaptive quickening, and any
persistent private-code cache remain inactive for later tasks.

Coroutine suspension has separate normal and cancel successors. A private VM session remembers the
cancel target only while it is suspended, rejects cancellation at every other lifecycle point,
recursively cancels an active sealed child, charges the child's cleanup steps to the parent budget,
and terminates through `core.cancel.publish`. Canonical safepoint metadata is an unordered exact set;
the suspension instruction's live operands are the ordered resume tuple, and its cancel operands are
the exact cleanup subset. The VM materializes only the chosen segment. Cancellation transfers every
owner once and any explicitly required non-owner at most once through the verified reason-private
cleanup graph, while a normal resume leaves the cancel segment untouched. A sealed child `REF`
parameter receives the exact place owned by its parent execution. VM value/place storage is execution-owned and remains stable while
the child is suspended; caller-local places are represented in the Program safepoint by their unique
storage owner and reconstructed in the selected continuation. Cancellation is a distinct outcome,
retains the cancelled safepoint state for cross-executor comparison, and releases the exact
generation lease; there is no frame-dispose fallback, raw stack-address spill, or
cancellation-as-error compatibility path.

`core.coroutine.suspend` publishes a typed request without performing readiness
work inside the VM. The timer form accepts exactly one i64 request value,
normalizes it to 0 through 86400000, and exposes the same normalized payload as
the reference evaluator. A sealed child propagates that request while the
observable state and safepoint remain those of the parent. The product VM
driver resumes cooperative yield immediately, waits for timer readiness using
monotonic time, and rejects an unknown kind, wrong arity, or non-normalized
payload instead of selecting a legacy native-call path.

Related field `REF` arguments may share one typed aggregate storage root. Each field place retains
its own checked ordinal, but the parent safepoint carries the aggregate owner exactly once and
reconstructs the field addresses from that root. A separately observed scalar field snapshot stays
a distinct live value: modifying the referenced field after suspension cannot change the snapshot.
The source fixture proves two fields of one affine aggregate together with an independent scalar
reference while the aggregate is captured by static deferred provider cleanup. Normal resume
closes the post-resume field values, child provider refusal reaches the same parent cleanup through
the sealed call's exact trap successor, and parent cancellation closes the pre-resume values.
Reference and both VM decode policies agree on the outcome and exact provider trace. Escaped views
and arbitrary nested projections remain outside this proof.

A sealed child `READ` existential parameter receives only an explicit read-reborrow. The caller
safepoint must carry the unique affine existential owner exactly once, and the child frame keeps
the borrowed carrier as a non-owner across suspension. Witness-direct calls inside that child use
the same conformance row as synchronous execution. Normal resume leaves the caller owner live;
cancellation consumes only the verified owner suffix. Missing, duplicate, ambiguous, or
interface-mismatched owners are rejected before VM code construction.

Source-case registration and native fixture ownership share one manifest. Selecting one fixture
executes its complete source, reference, VM, and C-emission assertions before publishing that one
artifact; default execution still runs the complete source suite. Free-function and static-method
coroutine cases have independent native outputs and checks. Registry identity, declaration census,
and projection checks reject missing or mismatched evidence rather than silently narrowing the
canonical preflight. Declaration analysis publishes stable module/declaration identity before
monomorphization. Rewritten roots and clones retain the analyzer-owned source declaration,
receiver/type tuple and effect facts; invalidated type and selection facts are rebuilt before
lowering. Both source and native build owners publish one final Xglobal graph, closing its generic
root/body/storage relations without a complete pre-monomorphization snapshot or root merge.
Standalone analysis without module authority cannot publish exact nominal specialization keys.
Program reachability closes over concrete instances only. A
generic fixture proves two i64 callsites share one exact specialization, a bool callsite receives a
distinct typed specialization, both VM decode policies return the reference value, and selecting
the open generic template as entry fails instead of executing an erased fallback. Generic value
struct literals are admitted only when the concrete Xglobal instance, nominal identity, detached
layout, field evidence, and the complete Xi allocation-and-initialization sequence agree. Distinct
`Box<i64>` and `Box<bool>` instances receive distinct Program TypeIds; the open `Box<T>` skeleton
never becomes a runtime value. Exact value-struct declaration tokens published by a module
initializer are phase-only and are erased, while heap classes and ordinary shared state remain
outside that rule. Reference and both VM decode policies return 42 for field projection and update.
One bounded dependency-owned user-interface constraint family is also closed before Program.
Namespace-qualified inference publishes its complete tuple to the source declaration owner.
Selective-import aliases bind two explicit concrete calls through independent compiler-private
imports to that same dependency even when the entry declares a same-named generic; the source
declaration and ordered concrete arguments jointly identify each specialization. An exact
non-implementor through the selective alias is rejected with the generic-constraint error before
Program construction. One dependency-owned and one caller-local trivial value-struct implementation
produce distinct generic function clones, nominal READ signatures, and non-generic method
FunctionIds. The caller-local declaration remains exact even when the defining module contains a
same-named private decoy; explicit and inferred calls with that caller type merge into one
specialization. Named locals and fresh direct trivial value-struct arguments carry distinct verified
provenance but share an exact call-bound, non-escaping READ lifetime. Their physical addresses and
receiver loads are erased to value operands; no interface table, conformance table, witness call, or
runtime constraint lookup remains. Before a generic call is rewritten to its concrete non-generic
callee, the analyzer publishes the exact template declaration and ordered concrete type tuple as one
fact. Final Xglobal evidence consumes that fact rather than mangled or display names, retains finite
nested roots such as `nested<Array<T>>` after substitution, and preserves the full 64-bit identity for
function and class/struct specializations. Selective re-export aliases and namespaces reached through
the same facade carry compiler-private exact graph and declaration bindings to the defining-module
specialization; repeated namespace calls reuse that binding. The facade publishes only its declared
source names, and importing the hidden original generic name is rejected. Reference and both VM decode
policies return 42. The same exact declaration-plus-ordered-tuple authority now covers generic
instance methods on a concrete, nongeneric value-struct receiver. Explicit and inferred calls reached
through a selective re-export and its namespace produce separate concrete method bodies for two
nominal constraint implementors while reusing each identical tuple. The open method template is not
executable, each specialized body calls the exact sealed implementor method, and Xglobal records the
origin class, method, declaration, specialized body, callsite, body-use, and code-size evidence.
Reference and both VM decode policies return 42. Xi-only call-bound READ places for the trivial struct
receiver and explicit argument are erased to Program values only after the receiver plan, callee ABI,
nominal TypeIds, direct source call, nonescape, and ownership facts agree. A constraint violation and
an uninferable method type tuple fail during analysis instead of reaching Program. The same exact
fact path admits static generic methods on concrete, nongeneric class and struct owners while keeping
their owner and method identities; only the executable ABI omits the receiver. Direct selective
imports and namespace access through a uniquely resolved re-export produce the same defining-module
specialization. Two distinct owners with the same method spelling and concrete tuple produce distinct
FunctionIds, so generic call-site resolution has no unique-name fallback. Generic template bodies record exact
declaration facts without becoming executable roots, and clones carry the substituted tuple into the
nested-instantiation fixpoint. A bounded generic value-struct receiver family now preserves the
receiver type tuple independently from the method declaration tuple. Its real module graph constructs
the defining module's `Box<i64>` through both a direct namespace and a renamed facade re-export.
Those paths share one nominal aggregate TypeId and one specialized method FunctionId. A facade-owned
same-name, same-layout `Box<i64>`, a caller-local same-name decoy, and the defining declaration's
`Box<bool>` prove that source declaration identity and ordered type arguments, rather than terminal
spelling or layout, select the concrete aggregate and method body. `readAs<U>` and a nested
`forwardAs<U>` call materialize on the exact monomorphized receiver rather than on the source
skeleton. The open `this` receiver fact remains a non-executable template edge until aggregate
cloning substitutes it. Xglobal schema 59 and Program validation independently rederive each
concrete body, construction, and body-use from the source nominal origin and matching monomorphized
receiver class identity; incomplete, inconsistent, or coordinated-forged receiver facts are rejected
before execution. Reference and both VM decode policies return 42. A second source-backed fixture
proves the same exact origin-and-tuple identity for non-escaping generic nominal classes with scalar
fields and fieldwise constructors. Direct namespace access, inferred construction, renamed facade
re-export, same-name facade/caller decoys, and distinct `i64`/`bool` tuples produce four exact affine
class TypeIds and four exact getter targets; Reference and both VM policies return 42. Shared class
aliases, mutation visibility, owned or nontrivial fields, physical heap lifetime and reclamation,
interface generic dispatch, parameterized and builtin constraints, generic ownership cleanup,
arbitrary returned/projected READ places, and valid finite recursive specialization remain outside
this bounded slice. Non-generic qualified aggregate syntax is a separate parser ambiguity and is not
claimed by this generic aggregate slice.
The source-build request carries one fail-closed budget for module count, monomorphization depth,
whole-graph specialization count, and final Program bytes. A caller may tighten those limits but
cannot raise them above the production defaults. Exhaustion stops before VM construction with the
exact E0388 or E0389 analyzer diagnostic and a structured resource-limit stage; tests use small
limits to exercise the same boundary without manufacturing thousands of specializations. A branching
deferred-Pipe fixture proves that provider failure can use the
shared cleanup graph while cancellation receives a private copy of the complete multi-block graph,
including the non-owner branch condition. A nested deferred-Pipe fixture additionally proves that
an inner registration discovered from a cold outer handler keeps its outer-handler failure edge,
closed owner payload, and cleanup-local branch place across Reference and both VM decode policies.
This test-build decomposition changes no VM semantics or supported capability.

Source fixture execution includes both VM decode policies, exact provider traces,
typed return/error payloads, and cancellation at every declared suspension point.
Construction of a private code view alone does not satisfy an execute expectation.
A suspended child may transfer a class result inside the private execution tree:
ordinary and suspended frames borrow one execution-owned value store and publish
the result on the verified normal edge. Destroying a child frame cannot invalidate
returned values or REF writes. This is distinct from exporting
class carriers to the host, which still fails closed at the public step/cancel
boundary. The affine Pipe source proves normal return and four cancellation points,
including class finalization/reclamation before frame disposal and no implicit copy.
A separate invocation of the same child as a host entry verifies export rejection.

Canonical `string` (builtin type 12, an affine owned value with explicit copy) and `rune`
(builtin type 13, a trivial Unicode scalar value) share one header-only typed text kernel with the
reference evaluator and with generated C. The kernel owns strict UTF-8 admission, scalar admission,
canonical fixed-width integer and bool display, byte-lexicographic comparison, concatenation sizing, and atomic
output-group assembly; no executor carries a second copy of those rules, and an executor-independent
boundary test with mutation evidence governs the kernel itself. The VM keeps string payloads in
execution-private arena cells, never exposes a shared physical layout, detaches a string result by
deep copy, and drops a string owner without a lifecycle event. `core.output.group` accepts all eight fixed-width integers,
bool, string, and rune operands, renders one exact line through the kernel, and performs one
byte-sink provider call. A string constant that is not strict UTF-8 is rejected at decode and never
reaches a handler. A real-source fixture proves that returned, concatenated, interpolated, compared,
branch-assigned, and loop-accumulated strings plus rune output satisfy independent value and stdout
expectations in the existing local model, both VM decode policies, and native C. Borrowed explicit-copy
results acquire one owner at each return site, including two sites returning the same parameter. `string?` owners transferred
across unbalanced branches, aggregates or classes with string fields, and text methods remain
outside this slice.

## Verification

`core.sequence.length` admits immutable strings and arrays, returning Unicode scalar
count or element count as a non-owning i64. Each private string carrier caches that count on construction,
copy and concatenation; length reads are O(1), allocate nothing and preserve the source owner.
The six-scalar fixture distinguishes byte length and grapheme count, copies a string, drops
its source, and reads the surviving copy through both decode policies and explicit frames.
Admission rejects wrong types, an owning result, an immediate and use after owner release.
Real source checks literals, returned/copy/concatenated strings, class fields, module-owned
receivers and timer suspension against literal expected stdout in VM and native execution.
Arrays reuse the execution tree's existing owned value cells. Construction transfers exact
typed elements only after allocation succeeds; copy recursively clones their values, and
drop follows the same recursive physical release path. Empty arrays remain affine owners.
Independent fixtures cover empty, integer, string and nested arrays, rejected ownership
mutations, both decode policies, explicit frames and every runtime allocation failure.
Source arrays, including nested arrays, survive replacement and timer suspension.
`core.owner.alias` acquires a distinct owner of the same class or array identity; array
cells release children only when their final owner drops. Independent fixtures compare
alias identity and explicit-copy isolation through both decode policies. One-shot array
results transfer their reachable cells to a private outcome owner retaining Code and the
instance lease; they remain readable after the caller releases Code, and block retirement
until disposal. Failure injection covers this transfer as well as construction and copy.
View carriers, dynamic array construction, indexing and mutation remain unimplemented here.

The common value store owns every aggregate, class, existential, callable and
string carrier for one execution tree. Carrier-specific child adoption and its
reallocation/failure paths are removed. Nested calls charge the same cell budget
when allocating, and class identities come from that same owner. Independent
entries never share this store, even when they use the same Code and Instance.
Frame completion retains existing semantic drops; storage teardown stays at the
root execution boundary, separate from instance-owned module state.

The suspended string fixture returns the independently expected bytes `21`,
and covers resume, cancellation and disposal while suspended. Two overlapping
entries prove that destroying one cannot invalidate the other. A parent and
child allocating three cells each succeed at a six-cell budget and refuse at
five; cancellation and resource refusal leave no lease. Existing class/REF and
provider cleanup fixtures continue checking identity and physical reclamation.

verification-test: test_xr_program_vm
verification-test: test_xr_program_vm_runtime
verification-test: xr_program_h2_backend_differential
verification-test: test_xr_program_source_build
verification-test: canonical_program_test_cli
verification-test: test_mono
verification-test: test_xglobal_summary
verification-test: test_xglobal_cache_payload
verification-test: test_text_kernel

Provider calls project Program types into the common bounded typed host pack,
then adopt results into private VM cells. Resource cells carry an affine owner
and use the same value graph transfer and teardown as aggregates and class
fields. Optional resource results survive frame completion through their result
owner; cross-instance borrowing fails before entering host code. This replaces
the VM scalar-signature dispatch and blanket resource-type refusal. It does not
establish native resource execution or complete source-library coverage.

verification-test: test_xr_typed_provider
verification-test: test_xr_typed_provider_allocations

## Assertion messages and panic observation lifetime

Core assert control 1 optionally includes message-present bit 256. A message is a borrowed
immutable string preceding the panic-edge live values. False clones it into the existing
affine PanicInfo, then follows ordinary panic cleanup; true preserves input ownership and
left-to-right evaluation. Type, control bits and complete owner transfer are verified.
No message-specific opcode or library provider is added. Copy OOM remains resource-limit.

VM build v16 reuses the immutable string cell and owner transfer. Synchronous outcomes own
their detached cause; stepped outcomes borrow through their observing execution. Sticky
initializer failures own the cause in the instance, and both forms retain a lease until
disposal. Cleanup of the original strings, frames and module slots cannot free this cause.
The historical Reference subset rejects message controls; old scalar assertion tests stay.
Independent Program mutation, VM, native and physical allocation tests own the new responsibility.

verification-test: test_xr_program_verify
verification-test: test_xr_program_vm_allocations
verification-test: panic_report_diagnostics
