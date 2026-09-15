# Canonical XrProgram VM contract

The VM consumes only `XrValidatedProgram` plus one active `XrInstance`. It does not decode program
bytes, call the reference evaluator, reconstruct source types or effects, or consult TargetPlan,
legacy Proto bytecode, or AOT. Every active CoreSpec operation has one explicit typed handler.

`XrProgram` remains the only distributed executable program format. `XrVmCode` is private runtime
state and has no serializer, reader, compatibility promise, or public install header. The baseline
view walks immutable validated rows. The fixed-row view copies only dispatch-shaped instruction
rows and retains references to validated operands and successors. Both views produce the same
logical operation trace. Adaptive quickening remains disabled until a measured policy and its
traceability proof exist.

VM code is qualified by `ExecutionId`, generation, VM build identity, decode policy, and quickening
policy. Execution pins the active generation and refuses retired or successor instances. A private
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
profile, BoundaryABI/runtime contracts, execution instance, and typed VM. It excludes frontend,
CoreIR writer, reference evaluator, TargetPlan, legacy Proto VM, and AOT. The runtime-only test
links a source-committed canonical XrProgram artifact against this archive and verifies the
resulting symbol closure. The artifact is deliberately outside the executor build graph: schema or
CoreSpec drift fails closed during validation instead of rebuilding and executing a compiler-side
fixture writer whenever VM or verifier sources change. An excluded, opt-in writer remains available
to regenerate the committed artifact when its schema intentionally changes; it is never a runtime
test dependency.

The executor covers all fifty-six current CoreSpec operations. `core.logical.not`,
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
canonical preflight. The source owner retains pre-monomorphization generic roots, merges them into
post-monomorphization evidence, and closes Program reachability over concrete instances only. A
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

anchor-sha256: tests/unit/program/xr_program_cleanup_graph_fixture.h 44cab32da792042b788f5282f91a042f2c6deb76c65bf7be69c242cc36003c54
anchor-sha256: tests/unit/program/xr_program_coroutine_branch_fixture.h a69072ef2f14b4b1350dcdeb9a5facaf9ee36b4a04c5bf1b18f09727f5b3aa50
anchor-sha256: CMakeLists.txt 5cbe10e2f0599c8f4db286797f119e4fdea855f12adea2f89e4ccd420543511b
anchor-sha256: xisa/core/registry.json aa317e48543dd4d9c2d2808e9dbaa5745338df790ca7aa4a8e7604892755269d
anchor-sha256: src/vm/xr_program_vm.h 18a60ac662d9be6377df0aa103cf971420da95a5c016cb10e3b25249413f365e
anchor-sha256: src/vm/xr_program_vm.c ec658bfde0b99f5eb8df256b90892352a420e31422ba0ea7018a8dbf93a20baa
anchor-sha256: src/program/xr_program_verify.h 05a87dca25a389c21133915c9684fc2560f21d02c888e6117a6da3337e4f6d9e
anchor-sha256: src/program/xr_program_verify.c fbc0f4d9b167efd73d068f3a013833ff0d424de38d7edfd43a7a0411debaa9ad
anchor-sha256: src/execution/xr_execution.h 3a09783038967320ae3566e258de5ae7108b60d7ed5f4f01a4a020067b447867
anchor-sha256: src/execution/xr_execution.c 9edba6e59f290cda924a6a337aba73554f8bd7aa1ac5a0e6dfba958d4b998046
anchor-sha256: src/execution/xr_execution_identity.h 5783c870cd0d642c6d60983e24efcd183edbfbb63380ffae3254e5617af5fd51
anchor-sha256: src/execution/xr_execution_identity.c 857dc89de900a4eda6e71c9498aac3657779a93e68d9593cd4fefb374b84f0bf
anchor-sha256: scripts/check_xr_program_vm_contracts.py 85c765550944c88b1c4b88dd720bc43372205efa6881706fc8d28fbf0bc1b0ce
anchor-sha256: contracts/canonical-program/xrprogram-vm-coverage.json d54078ce28815be03f9e980de2304c902afa02d855d9917bc8f6803a96c5ceb7
anchor-sha256: tests/unit/vm/test_xr_program_vm.c ac8e075604762f23d4743809c0769644dffcae544c0b95e525eb33cf810637bb
anchor-sha256: tests/unit/vm/test_xr_program_vm_runtime.c 75e731e0d36264735ad2f9625206b2ec323e14de9101f91d32827fdffbcce570
anchor-sha256: tests/unit/vm/xr_program_vm_embedded_fixture.h 50d0314588ccc1e02c71fe8cf06f656ac559be97be1cc60c688da9e26bfcd0e4
anchor-sha256: tests/unit/program/xr_program_vm_fixture_writer.c 9e8418945a6b029c67e4d85d76d6603a68f0c77f72cab8ff5a3b6f37691865ab
anchor-sha256: tests/unit/program/test_xr_program_source_build.c 3c8916f1e8ccbeffdbe712f46e2ddd5941cbd9161fff4a4c460fd4a1a3e5b690
