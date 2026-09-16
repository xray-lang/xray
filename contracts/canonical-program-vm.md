# Canonical XrProgram VM contract

VM code construction consumes only `XrValidatedProgram` and a verified `XrTargetProfile`; execution requires one active `XrInstance`. It does not decode program
bytes, call the reference evaluator, reconstruct source types or effects, or consult TargetPlan,
legacy Proto bytecode, or AOT. Every active CoreSpec operation has one explicit typed handler.

`XrProgram` remains the only distributed executable program format. `XrVmCode` is private runtime
state and has no serializer, reader, compatibility promise, or public install header. The baseline
view walks immutable validated rows. The fixed-row view copies only dispatch-shaped instruction
rows and retains references to validated operands and successors. Both views produce the same
logical operation trace. Adaptive quickening remains disabled until a measured policy and its
traceability proof exist.

VM code is qualified by `ExecutionId`, VM build identity, decode policy, and quickening policy.
Construction owns a Program reference and does not require a live provider binding or generation.
Execution separately checks Program/Profile compatibility and pins the actual instance lease.
A compatible successor or independent instance can reuse code; a retired instance cannot execute.
Provider admission and binding-context lifetime remain instance-owned, and resource values or
leases from one instance do not gain authority in another by sharing an ExecutionId. A private
view can always be discarded and rebuilt from `XrValidatedProgram`; corruption or mismatch cannot
fall back to an older executor.

Coroutine execution preserves the same boolean constants, selected conditional edges, and
parallel block-argument assignment as ordinary execution. Each execution owns bounded temporary
edge storage: it snapshots the selected payload and any implicit result before overwriting target
arguments, including on a self-loop. That storage is private to the parent or child execution and
is released with it. Multi-block cancellation and provider-failure cleanup follow only validated
reason-private edges; no executor-local search for a convenient terminal is permitted.

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
cells and never expose a shared physical layout. A one-shot aggregate return or uncaught typed
error is recursively detached into executor-owned logical storage before that arena is released;
the caller may inspect it only through the typed aggregate view and must dispose the outcome.
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
the parent adopts the child's carrier storage before destroying its frame and
publishes the result on the verified normal edge. This is distinct from exporting
class carriers to the host, which still fails closed at the public step/cancel
boundary. The affine Pipe source proves normal return and four cancellation points,
including class finalization/reclamation before frame disposal and no implicit copy.
A separate invocation of the same child as a host entry verifies export rejection.

Canonical `string` (builtin type 12, an affine owned value with explicit copy) and `rune`
(builtin type 13, a trivial Unicode scalar value) share one header-only typed text kernel with the
reference evaluator and with generated C. The kernel owns strict UTF-8 admission, scalar admission,
canonical i64 and bool display, byte-lexicographic comparison, concatenation sizing, and atomic
output-group assembly; no executor carries a second copy of those rules, and an executor-independent
boundary test with mutation evidence governs the kernel itself. The VM keeps string payloads in
execution-private arena cells, never exposes a shared physical layout, detaches a string result by
deep copy, and drops a string owner without a lifecycle event. `core.output.group` accepts i64,
bool, string, and rune operands, renders one exact line through the kernel, and performs one
byte-sink provider call. A string constant that is not strict UTF-8 is rejected at decode and never
reaches a handler. A real-source fixture proves that returned, concatenated, interpolated, compared,
branch-assigned, and loop-accumulated strings plus rune output satisfy independent value and stdout
expectations in the existing local model, both VM decode policies, and native C. Borrowed explicit-copy
results acquire one owner at each return site, including two sites returning the same parameter. `string?` owners transferred
across unbalanced branches, aggregates or classes with string fields, and text methods remain
outside this slice.

## Verification

verification-test: test_xr_program_vm
verification-test: test_xr_program_vm_runtime
verification-test: xr_program_h2_backend_differential
verification-test: test_xr_program_source_build
verification-test: test_mono
verification-test: test_xglobal_summary
verification-test: test_xglobal_cache_payload
verification-test: test_text_kernel
