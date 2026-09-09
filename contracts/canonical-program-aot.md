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
signed decimal line and performs one output-write call. The Pipe source slice emits target-native
anonymous-pipe open and endpoint-close helpers only for the exact stable operations named by its
profile, while retaining no live provider instance or lease in the compiler. Embedded AOT receives
typed callbacks from its host; hosted standalone AOT binds the generated entry directly to its
closed native helper set, while freestanding standalone emission without an embedder fails closed.
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
AOT-toolchain symbol and executes without program bytes. All fifty-six current CoreSpec operations
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
implementors: each generic function clone carries its own nominal READ signature and calls one
distinct non-generic method FunctionId. The physical Xi call-bound address and receiver load are
erased only after their frozen non-escaping READ plans and exact struct identities agree; Program
contains neither an interface witness table nor runtime constraint dispatch. Reference, both VM
views, and strict native C return 42 from the same four sealed calls. Parameterized/builtin
constraints, generic methods, generic ownership cleanup, temporary-expression READ places, and
recursive specialization remain unqualified. A branching
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

anchor-sha256: tests/unit/program/xr_program_cleanup_graph_fixture.h 44cab32da792042b788f5282f91a042f2c6deb76c65bf7be69c242cc36003c54
anchor-sha256: tests/unit/program/xr_program_coroutine_branch_fixture.h a69072ef2f14b4b1350dcdeb9a5facaf9ee36b4a04c5bf1b18f09727f5b3aa50
anchor-sha256: CMakeLists.txt 98de1343fd92cf4293bc518311f52c1fd98b4d94279a86640234c1fff37dc71f
anchor-sha256: tests/unit/CMakeLists.txt 20786799eea5478bc68d3c02a705f02c19effb635c151e97db463fe0b0d4419d
anchor-sha256: xisa/core/registry.json 3ada1fa0976aea3619134da0e559427465bf0f2b83910497d59647ce55d0b172
anchor-sha256: contracts/canonical-program/architecture-identity.toml 844f5e20d293d2b74efda9d1755c7da2d9da27b9acc985b346dcdc54e1917ccd
anchor-sha256: contracts/canonical-program/operation-capability-matrix.json c54673aaa35f65552e77f593a6aa167b933992cef66f9138325fa37b0f44d564
anchor-sha256: contracts/canonical-program/xrprogram-aot-coverage.json 9559194c9eef62233098c0054861da75c3cbdf6d191a3d26f68de01ff247f244
anchor-sha256: src/aot/program/xr_backend_ir.h 27bbdd970f1bf1ad039af977cd395aa8609d9c02794e9a566232ba174d86b8af
anchor-sha256: src/aot/program/xr_backend_ir_internal.h 5a33e1981425e9392eebbe80a058d3a9a8b80f221e327fd0d505a3f5ee737855
anchor-sha256: src/aot/program/xr_backend_ir.c 0f400b1c25fba3bdd5efac6976caaedc09cb79260dfbc91c37e37a637f361f28
anchor-sha256: src/aot/program/xr_backend_ir_verify.c 9c0819c55037cf4f8de7394802bb47fdd1281a96ef976b3d09d8d72a725ffa7f
anchor-sha256: src/aot/program/xr_backend_ir_emit_c.c 9a630f11d72d9b3b4e9938b68659455fd7b3e73739c3439ede920cd967c54d5f
anchor-sha256: src/aot/program/xr_backend_ir_emit_copy.inc.c 8c831fe2d6161dd1b9a4b86de86d9a40b4573e05985650d258772345658d78f8
anchor-sha256: src/aot/program/xr_native_artifact.c fc273aac15c76b9ffcd6300e7c77978a4f729bbbfcbf06c8018df85ba72d4f76
anchor-sha256: src/execution/xr_execution_identity.h 5783c870cd0d642c6d60983e24efcd183edbfbb63380ffae3254e5617af5fd51
anchor-sha256: src/execution/xr_execution_identity.c 857dc89de900a4eda6e71c9498aac3657779a93e68d9593cd4fefb374b84f0bf
anchor-sha256: scripts/check_xr_program_aot_contracts.py df427a19a20a6b6b42320b91bd3928d1f82b993b1955240e13323b572360497e
anchor-sha256: scripts/check_xr_program_aot_native.py 56822ee193168c2f76f549afbbaacaf3ed99f0236bf531b2500e52337881c2e2
anchor-sha256: scripts/check_xr_program_aot_providers.py 51b1bdc7b16aa8709ba082ad185dcd0ca59af2f3bc10428d8d4ef480c80acbc0
anchor-sha256: tests/unit/aot/test_xr_program_aot.c b8149f84c1f75ad8df83b26fe55ab44724867b86f7cfc377706e1cac50a9c6ab
anchor-sha256: tests/unit/program/test_xr_program_source_build.c 8f57b37e41b331ea0a650f6bdab7341575df0455ec3d8f314f24a513b001c6d1
