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
AOT-toolchain symbol and executes without program bytes. All fifty-five current CoreSpec operations
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
retains the cancelled state identity. Resume operands and the exact live-owner cancel suffix are
verified separately. Generated C spills the canonical resume live row, reloads the original live
value identities for cancellation, and performs the cancel edge's parallel argument transfer before
the static cleanup drops its owners. A sealed child may accept an exact non-owner `REF` place. A
caller-local place is never spilled as an automatic C address: its unique storage owner occupies the
parent safepoint slot, the child place is rebound to that slot before polling, and the continuation
reconstructs its block-local place. A call-bound parameter place may remain live in the child frame
because its caller owns the backing storage. The BackendIR verifier rejects instruction-defined raw
places in live rows, missing frame owners, extra or undropped cancel values, and malformed
continuations; generated C does not treat frame disposal, place-address reconstruction, error,
panic, or trap as a cancellation alias.

For related aggregate-field `REF` arguments, BackendIR independently checks the typed projection
chain and proves that its unique value root occurs once in the caller safepoint. Generated C saves
and restores each distinct root once, and binds each child pointer to its own field inside that
parent-frame root. Scalar field snapshots remain separate live values rather than aliases. The
source fixture covers two fields sharing one affine aggregate, an independent scalar reference,
and a pre-suspension snapshot, with normal resume and cancellation. Deferred provider cleanup,
escaped views, and arbitrary source-level nested projections remain outside that fixture's proof.

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
the canonical preflight mechanically includes every manifest native target and test.

The source declaration census and both generated registration projections are checked before every
producer relink. A test-body edit alone does not reconfigure CMake. A new unregistered case fails
before linking and fails again on retry; unchanged builds need no new check. A real CMake/Ninja
self-test covers these dependencies independently of the lightweight preflight.

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

anchor-sha256: tests/unit/program/xr_program_cleanup_graph_fixture.h a964247d2aacfd087417bc19ad30e2c81c870cb5a28521bb953b2dc8d84d2f1f
anchor-sha256: tests/unit/program/xr_program_coroutine_branch_fixture.h c51ff9a5892b84710d42cd5eceab4fc7f2887ed37062dd28b3a2f0bc68fc00ca
anchor-sha256: CMakeLists.txt 24ef3583c9df1d17d0d44ce5ede860b2acf6b70544facc5f05d5dbe8c600788d
anchor-sha256: tests/unit/CMakeLists.txt 37c7d17d4cf391031ba0d3d71017d9c6b78bc8580e8bac1055a46a9910c132d7
anchor-sha256: xisa/core/registry.json 1066fb08ca4dae249c7fc59d26e1ce4f6c387fbbdd16d31480c72badb109483f
anchor-sha256: contracts/canonical-program/architecture-identity.toml 844f5e20d293d2b74efda9d1755c7da2d9da27b9acc985b346dcdc54e1917ccd
anchor-sha256: contracts/canonical-program/operation-capability-matrix.json 5554779cd3ab5d8a1ead2ab78d6ba135a1bdab269dfb9f998ab043fb149f7d48
anchor-sha256: contracts/canonical-program/xrprogram-aot-coverage.json 81bb6fb3456dbc45d6bd4d81b672a296e9c2990cf846a2c8ed72a2a2b45fa6da
anchor-sha256: src/aot/program/xr_backend_ir.h af75b1247f80f1b3132d71b65774b05f6c99d4bfbcab3e9fd097911f7694a799
anchor-sha256: src/aot/program/xr_backend_ir_internal.h c1360e511db5f08bb28aea04b0b4048b958f4778da8eecd7adeee88fba9f9942
anchor-sha256: src/aot/program/xr_backend_ir.c d2f80522ab5b2907551229d5efd615c6be04bb7ce6ba6e6a40eae40cdb6c0c3d
anchor-sha256: src/aot/program/xr_backend_ir_verify.c eeae99edc5c0fb85d74cc711fbe83fed34dfd96948e43ebe379d0f85c4b63a5c
anchor-sha256: src/aot/program/xr_backend_ir_emit_c.c 78e21051e18b6307bfc0baf600fcb33dbb3b0f870aa96b18389c0c4dd9634ac0
anchor-sha256: src/aot/program/xr_backend_ir_emit_copy.inc.c 8c831fe2d6161dd1b9a4b86de86d9a40b4573e05985650d258772345658d78f8
anchor-sha256: src/aot/program/xr_native_artifact.c fc273aac15c76b9ffcd6300e7c77978a4f729bbbfcbf06c8018df85ba72d4f76
anchor-sha256: src/execution/xr_execution_identity.h 5783c870cd0d642c6d60983e24efcd183edbfbb63380ffae3254e5617af5fd51
anchor-sha256: src/execution/xr_execution_identity.c 857dc89de900a4eda6e71c9498aac3657779a93e68d9593cd4fefb374b84f0bf
anchor-sha256: scripts/check_xr_program_aot_contracts.py df427a19a20a6b6b42320b91bd3928d1f82b993b1955240e13323b572360497e
anchor-sha256: scripts/check_xr_program_aot_native.py 56822ee193168c2f76f549afbbaacaf3ed99f0236bf531b2500e52337881c2e2
anchor-sha256: scripts/check_xr_program_aot_providers.py 51b1bdc7b16aa8709ba082ad185dcd0ca59af2f3bc10428d8d4ef480c80acbc0
anchor-sha256: tests/unit/aot/test_xr_program_aot.c 9a2340d3c3859e49b0d6358f9cccd47221c0a52a70c28a136efd0acb84e7a8ce
anchor-sha256: tests/unit/program/test_xr_program_source_build.c 938b04802603328b720dbf9bf64ee0d5ddfc4ace5b7f758115d90d82fbffbba2
