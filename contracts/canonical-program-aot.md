# Canonical XrProgram AOT contract

The AOT compiler accepts only an `XrValidatedProgram`, the exact immutable `XrTargetProfile`, and
`XrBackendOptions`. It builds a private `XrBackendIR`, copies the verified operation graph, and adds
only physical value representations and target literals. Compilation cannot name or acquire a live
`XrInstance`, provider instance, generation, or lease. Lowering and emission do not query source,
AST, Xi, TargetPlan, VM-private code, or the reference evaluator.

Every BackendIR build runs an independent invariant verifier and an exact structural translation
validator. The portable optimization policy currently introduces no high-risk semantic rewrite;
therefore no proof-shaped metadata is fabricated. A future BCE, RC/escape, devirtualization,
coroutine compression, or ABI-changing pass must add a directed witness and checker before it can
become active.

The invariant verifier independently reconstructs block-local definitions, value availability,
owner consumption, and exact outgoing payloads from BackendIR. Ordinary and cleanup edges must
match target types, categories, ownership, and the complete live-owner balance. Refusal edges are
checked after parameter consumption and before a normal result exists; typed implicit outcomes
take ownership from their logical type. A bounded graph walk keeps normal, cancellation, and trap
7 domains distinct, validates every explicit exit, and rejects suspension during cleanup. Cycles
within one reason domain are legal; admission does not claim they terminate. Exact translation
validation remains an additional check, not a substitute for these invariant proofs.

Generated C uses portable C11 typed locals. Checked integer operations avoid signed-overflow
undefined behavior, wrapping operations use unsigned arithmetic plus explicit two's-complement
reconstruction, sealed calls are direct C calls, block arguments use parallel edge copies, and
target queries become literals from `XrTargetProfile`. The existing W1-W4 generated-output
verifier runs before bytes leave the emitter. Strict Clang, Zig CC, and clang-cl frontend tests
compile the real generated translation unit.

Provider-backed operations lower an exact program requirement/operation index to a matching typed
callback slot in the private runtime `XrAotContext` and normalize refusal to the canonical trap.
The admitted logical shapes are nullary/unary i64, `bool(i64)`, nullary optional-i64-pair, and the
dedicated byte sink; no erased provider-call ABI exists. `core.output.group.i64` formats the exact
signed decimal line and performs one output-write call. Hosted standalone provider bindings
come from the same explicit declarations and typed adapter-body generator as VM bindings.
Selection matches complete contract/operation IDs and canonical logical facts. The emitter supplies
only dense Program indexes and private boolean-carrier conversion; no clock, UTC, pipe, process,
or other host service is reimplemented inside generated-C spelling. Unsupported standalone
requirements fail before C publication; embedded AOT retains its explicit typed host callbacks.

Generated native adapters include their declared host headers, check exact C function prototypes,
and call the shared typed host implementation. The standalone build links the narrow
`xray_native_provider_runtime` archive when needed; this archive contains only OS time/pipe
implementation objects, with no compiler, VM, Program loader, or tagged-value runtime. Header-only
host helpers share the same implementation with VM adapters. These host dependencies belong to
the native toolchain/runtime-object identity, never to target-neutral Program semantics.
Freestanding standalone emission without an embedder remains unsupported.
Source-built clock, Pipe and process fixtures execute generated native bindings
through Reference and both VM decode policies, then execute strict standalone C.
The process fixture checks that repeated getpid queries return the same positive
process identity. Existing controlled provider fixtures retain their independent
failure and resource-event checks; real host execution does not replace them.
A strict native fixture built from real-source `time.now()` proves that one callback refusal becomes
trap 7 and follows explicit Program continuations through two deferred Pipe closes in the callee and
then two deferred Pipe closes in the caller. The same validated edge contract covers direct,
sealed-direct, indirect-direct, indirect-invoke, witness-direct, witness-invoke, and sealed-invoke
calls; generated C does not infer an unwind policy or catch a different trap.

`XrNativeArtifact` owns actual native bytes. `NativeArtifactId` hashes `ExecutionId`, `BackendId`,
`ToolchainId`, `OptimizationPolicyId`, and those bytes. Toolchain identity is reconstructible from
provider version, target triple, codegen options, sysroot, runtime objects, and target profile
fingerprints; every partition is checked exactly and mismatches fail closed.

The pure-AOT walking-skeleton executable contains no VM, compiler, program-loader, TargetPlan, or
AOT-toolchain symbol and executes without program bytes. All current CoreSpec operations
have private BackendIR/C lowering. `core.logical.not`, `core.logical.and`, and `core.logical.or`
accept only canonical `bool` values and emit portable C logical expressions over already evaluated
SSA operands. Source expressions with a trapping or effectful right-hand side are projected into
explicit Program control flow, so short-circuit behavior does not depend on C expression evaluation
or an AOT-only convention. Wave 2 aggregate and variant rows become portable C11 structs and
tagged unions without a VM carrier or shared local layout. A source-built higher-order function
fixture proves that a callable borrowed through an ordinary function parameter has the same
reference, VM, and native generated-C result, and that repeated C emission is byte-identical. The
Pipe source fixture also executes guarded division-by-zero right-hand sides through generated C to
prove source short-circuit parity. A second Pipe fixture enters a fallible function directly and
proves that reference execution, VM execution, and strict native generated C preserve the same
typed error tag and payload after running both deferred endpoint closes. The operation matrix separately records
which rows have a production source owner. Checked overflow, wrapping overflow, and division by zero
also compile and execute as independent native cases. Full language operation families, high-risk
optimizations, public loader ABI, and package publication remain inactive.

Generated coroutine descriptors have distinct step and cancel callbacks. Their private frames
dispatch cancellation from the exact suspended state, recursively close an active sealed child,
follow the verified Program cancel successor, and publish a normalized cancelled outcome that
retains the cancelled state identity. Canonical safepoint metadata is verified as an unordered exact
set, independently from the suspension instruction's positional resume tuple and cancel subset.
An owned captured-callable carrier used by an indirect suspended call must appear exactly once in
that set and in the cancel transfer; a `READ` parameter in a sealed child remains non-owning while
the caller retains the carrier. BackendIR re-proves this relation before emission, including normal
owner forwarding into a later suspension and exact drop on both final normal and cancel paths.
Generated C assigns frame slots from the positional tuple, reloads the original live identities for
cancellation, and performs the cancel edge's parallel argument transfer before the reason-private
cleanup graph drops its owners. The cancel subset contains every required owner exactly once and may
also contain a required non-owner at most once. A sealed child may accept an exact non-owner `REF`
place. A caller-local place is never spilled as an automatic C address: its unique storage owner occupies the
parent safepoint slot, the child place is rebound to that slot before polling, and the continuation
reconstructs its block-local place. A call-bound parameter place may remain live in the child frame
because its caller owns the backing storage. The BackendIR verifier rejects instruction-defined raw
places in live rows, missing frame owners, extra or undropped cancel values, and malformed
continuations; generated C does not treat frame disposal, place-address reconstruction, error,
panic, or trap as a cancellation alias.

`core.coroutine.suspend` remains a logical request in private BackendIR and in
the generated step ABI. Its timer form has exactly one normalized i64 payload;
embedded hosts observe that request and decide readiness, while the hosted
standalone main drives cooperative yield immediately and timer readiness with a
monotonic OS wait before polling the same frame again. A sealed child forwards
its request but not its private state identity. Unknown kinds, wrong arity, and
non-normalized timer payloads become a terminal generated-C failure rather than
an eager native call, VM dependency, or compatibility fallback.

For related aggregate-field `REF` arguments, BackendIR independently checks the typed projection
chain and proves that its unique value root occurs once in the caller safepoint. Generated C saves
and restores each distinct root once, and binds each child pointer to its own field inside that
parent-frame root. Scalar field snapshots remain separate live values rather than aliases. The
source fixture covers two fields sharing one affine aggregate, an independent scalar reference,
and a pre-suspension snapshot while the aggregate is captured by static deferred provider cleanup.
The sealed call has exact normal, cancel, and child-trap successors. Generated C closes post-resume
field values on normal completion and child provider refusal, closes pre-resume values on parent
cancellation, and matches Reference and both VM decode policies without an AOT-only unwind rule.
Escaped views and arbitrary source-level nested projections remain outside that fixture's proof.

An exact `READ` existential crossing a sealed child suspension is represented by two distinct
values: the non-owner reborrow passed to the child and the unique affine existential owner stored
once in the caller safepoint. BackendIR independently re-proves the scoped reborrow/owner relation,
interface identity, ownership categories, and live-row multiplicity before emission. Generated C
keeps the owner carrier alive across child polling, executes witness-direct dispatch through the
borrowed carrier, and releases only the owner on cancellation. A missing, duplicate, ambiguous, or
interface-mismatched owner is not lowered.

Source-native test registration has one manifest owner. Each fixture has one source-case producer,
one generated-C output, and one independent strict native execution and symbol check. Free-function
and exported-static-method coroutine calls have distinct artifacts; one cannot overwrite the other.
The producer accepts only an exact fixture identity and matching manifest digest, runs that complete
source case, and stages its C output. Publication follows successful assertions and preserves the
previous file's bytes, identity, and timestamp when the new bytes match. Missing, duplicate, unknown,
or stale registration fails closed. The default source suite still executes every registered case;
the canonical preflight mechanically includes every manifest native target and test. One real-source
generic fixture is closed before BackendIR: two i64 calls target one concrete Program FunctionId,
the bool call targets a distinct FunctionId, and the open template is absent. Repeated C emission is
byte-identical and strict native execution returns the same value as Reference and both VM views;
generated C receives no erased generic body or runtime specialization policy. A generic value
struct fixture additionally proves that concrete field evidence closes `Box<i64>` and `Box<bool>`
to distinct nominal Program TypeIds, collapses the exact Xi allocation-and-initialization sequence
to one logical aggregate construction, and emits byte-identical C whose strict native result is 42.
The open aggregate skeleton is absent and there is no erased aggregate representation or AOT-only
specialization policy. A bounded constraint fixture additionally proves two concrete value-struct
implementors owned by a dependency module. Namespace-qualified inference publishes its complete
tuple to the source declaration owner. Selective-import aliases bind two explicit concrete calls
through independent compiler-private imports to that same dependency even when the entry declares a
same-named generic; the source declaration and ordered concrete arguments jointly identify each
specialization. An exact non-implementor through the selective alias is rejected before Program
construction. Each generic function clone carries its own nominal READ signature and calls one
distinct non-generic method FunctionId. One dependency-owned and one caller-local trivial
value-struct implementation remain distinct even when the defining module contains a same-named
private decoy; explicit and inferred calls with that caller type merge into one specialization.
Named locals and fresh direct trivial value-struct arguments carry distinct verified provenance but
share an exact call-bound, non-escaping READ lifetime. Their physical Xi addresses and receiver
loads are erased only after the frozen plans and exact struct identities agree; Program contains
neither an interface witness table nor runtime constraint dispatch. Before rewriting a generic call
to its concrete non-generic callee, the analyzer publishes the exact template declaration and ordered
concrete type tuple as one fact. Final Xglobal evidence consumes that fact rather than mangled or
display names, retains finite nested roots such as `nested<Array<T>>` after substitution, and preserves
the full 64-bit identity for function and class/struct specializations. Reference, both VM views, and
strict native C return 42. That native fixture covers both a selective re-export alias and a namespace
reached through the same facade. Their compiler-private bindings retain the defining module's exact
graph and declaration authority, repeated namespace calls reuse one binding, and the facade never
publishes a mangled specialization or the hidden original generic name. The same source-backed
identity now admits generic instance methods on a concrete, nongeneric value-struct receiver. Explicit
and inferred calls through a selective re-export and namespace produce one concrete method body per
exact nominal constraint implementor and reuse identical tuples; the open method template is absent
from executable reachability. Xglobal binds each clone to its origin class, method, declaration,
specialized body, exact callsite, body-use, and code-size evidence. Reference, both VM views, and strict
native C return 42. Xi-only call-bound READ places for the trivial receiver and explicit argument are
erased only after exact receiver-plan, callee-ABI, nominal-TypeId, direct-source-call, nonescape, and
ownership agreement. A constraint violation and an uninferable method type tuple fail during analysis.
The same exact fact path admits static generic methods on concrete, nongeneric class and struct owners
while keeping their owner and method identities; only the executable ABI omits the receiver. Direct
selective imports and namespace access through a uniquely resolved re-export produce the same
defining-module specialization. Two distinct owners with the same method spelling and concrete tuple
produce distinct FunctionIds, so generic call-site resolution has no unique-name fallback. Generic template bodies
record exact declaration facts without becoming executable roots, and clones carry the substituted
tuple into the nested-instantiation fixpoint. Repeated C emission remains byte-identical, strict native
C and both VM views return the reference value 42, and no AOT-only lookup or specialization policy is
introduced. A bounded generic value-struct receiver family keeps the receiver type tuple independent
from the method declaration tuple. Its real module graph constructs the defining module's
`Box<i64>` through a direct namespace and a renamed facade re-export; both paths share one nominal
aggregate TypeId and one specialized method FunctionId. A facade-owned same-name, same-layout
`Box<i64>`, a caller-local same-name decoy, and the defining declaration's `Box<bool>` prove that
source declaration identity and ordered type arguments, rather than terminal spelling or layout,
select the concrete aggregate and method body. `readAs<U>` and a nested `forwardAs<U>` call
materialize on the exact monomorphized receiver, while the open `this` receiver remains a
non-executable template edge until cloning substitutes it. Xglobal schema 59, Program validation,
and the AOT verifier independently rederive the source nominal origin, call-site owner module, and
monomorphized receiver identity; coordinated corruption of the instantiation, body-use, and derived
AOT plans is rejected. Repeated C remains byte-identical and strict native execution returns 42. A
second source-backed fixture proves the same exact identity for non-escaping generic nominal classes
with scalar fields and fieldwise constructors. Namespace access, inferred construction, renamed
re-export, same-name facade/caller decoys, and distinct `i64`/`bool` tuples produce four exact affine
class TypeIds and four exact getter targets. Repeated C is byte-identical and strict native execution
returns 42. Shared class aliases, mutation visibility, owned or nontrivial fields, physical heap
lifetime and reclamation, interface generic dispatch, parameterized/builtin constraints, generic
ownership cleanup, arbitrary returned/projected READ places, and valid finite recursive
specialization remain unqualified. Non-generic qualified aggregate syntax is a separate parser
ambiguity and is not claimed by this generic aggregate slice.
Before AOT receives a validated Program,
the shared source-build request enforces one fail-closed module/depth/whole-graph-instance/final-byte
budget. Requests may tighten but not expand the production defaults; E0388/E0389 exhaustion remains
an exact structured source diagnostic rather than an AOT-only limit or fallback. Small injected
budgets test this production boundary without generating a giant source fixture. A branching
deferred-Pipe fixture executes normal, cancellation-at-each-safepoint, and provider-refusal paths;
its native output proves that the complete private cancellation graph preserves branch inputs and
cleanup order without an AOT-only unwind rule.

The source declaration census and both generated registration projections are checked before every
producer relink. A test-body edit alone does not reconfigure CMake. A new unregistered case fails
before linking and fails again on retry; unchanged builds need no new check. A real CMake/Ninja
self-test covers these dependencies independently of the lightweight preflight. The registered
nested deferred-Pipe fixture compiles the same Program to native C and checks normal completion,
cancellation at both suspension points, and provider refusal in outer and inner cleanup regions.
Its exact provider trace matches Reference and VM, including consumed endpoints and retry after a
refused close; AOT has no private unwind or cleanup-stack reconstruction.

Explicit owner copy materializes independent owning aggregate, active variant payload, and
callable capture values through type-specific private C helpers. Their dependency closure starts
at actual copy operations and includes each type once. All helper declarations precede their
definitions because a finite callable value may have a cyclic possible-target type graph; this
is not a recursive-by-value type and must not be rejected or expanded recursively by the emitter.
Each helper writes a local candidate and publishes its output only after every child succeeds.
Read existentials remain non-owning views, and forbidden copy contracts remain rejected by Program
validation. Source-level nested owning captures and unbounded recursive-copy depth are not qualified
by the internal materialization fixture.

The private allocation header is aligned for the emitted scalar and pointer leaf types. Exact
TargetProfile size/alignment and each allocated payload alignment are checked in generated C11.
Overflow and allocation failure do not publish a new allocation-chain node; successfully allocated
partial copies stay owned by the execution context and are freed once when that context is destroyed.
The real nonempty allocation fixture validates one Program, compares Reference and both VM views,
then emits C without a live instance. Windows native checks use MSVC and installed clang-cl, inspect
the actual generated allocator, and cover nested value isolation, each allocation failure, empty and
invalid private dispatch, unchanged failed-copy outputs, destruction, and forbidden symbol absence.
Those private-state mutations are materialization checks, not source-language mutation examples.

The cleanup source fixtures emit dedicated native drivers for provider refusal,
child cleanup composition, branching and nested cleanup, typed uncaught errors,
place-backed cancellation, REF/READ child suspension, and affine child results.
Each driver executes its declared scenarios and checks exact outcomes and provider
traces; an ordinary standalone main does not qualify those scenarios. The REF
fixture's exit byte is the dedicated driver's success oracle, not the ordinary
program result modulo 256. Both generated C emissions must agree byte-for-byte.
The reusable checks remain test-private and are part of the governed fixture source.

Canonical `string` values lower to a private owning `XrAotString` heap handle and `rune` values to
a `uint32_t` scalar. Generated C embeds the shared typed text kernel verbatim from
`src/runtime/core/xr_text_kernel.h` through the build-time embedding step, so string constant
admission, display, comparison, concatenation, and output-group assembly are the same text that the
reference evaluator and the VM compile; the emitter contributes only allocation, constant tables,
and the single byte-sink call per group. String constants are emitted as byte tables and
materialized per use into fresh owners, a string drop frees the handle without a lifecycle event,
owner copy deep-copies the bytes, and aggregate copy helpers over string fields fail closed.
`core.output.group` measures the exact line through the kernel, renders it into one allocation, and
performs one output-write callback. The registered real-source text fixture compiles to native C
and checks both the exit status and the exact stdout bytes against independent literal expectations.
The shared text kernel has its own executor-independent boundary and overflow tests.

## Verification

verification-test: test_xr_program_aot
verification-test: test_xr_program_aot_real_artifact
verification-test: test_xr_program_aot_native
verification-test: test_xr_program_source_aot_native
verification-test: test_xr_program_child_coroutine_trap_cleanup_aot_native
verification-test: test_xr_program_branching_cleanup_aot_native
verification-test: test_xr_program_nested_cleanup_aot_native
verification-test: test_text_kernel
verification-test: test_xr_program_aot_text_output
verification-test: test_xr_program_text_program_aot_native
