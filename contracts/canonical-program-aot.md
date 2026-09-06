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
AOT-toolchain symbol and executes without program bytes. All fifty-three current CoreSpec operations
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

anchor-sha256: CMakeLists.txt a91db5257863b84ca8e55cd78ba0cd2623db866f1a3def15d2a4af972cec4c66
anchor-sha256: tests/unit/CMakeLists.txt e9e6c32399545885b5937a97c99e9b3c698d24e5d9ccc9971d32593cf31a4e5b
anchor-sha256: xisa/core/registry.json 818a78af6a234a62f9c66e828ff07bd13c0af4fa8df0c6d2306fcacf9046cea1
anchor-sha256: contracts/canonical-program/architecture-identity.toml 844f5e20d293d2b74efda9d1755c7da2d9da27b9acc985b346dcdc54e1917ccd
anchor-sha256: contracts/canonical-program/operation-capability-matrix.json 2ab8c2466dbfedd0f15267897d22a4a141e901638b4825ca66fc269acf74aedf
anchor-sha256: contracts/canonical-program/xrprogram-aot-coverage.json e9751acfa3e2e99115ff17afc48547ebe364a7ffe76ce498d861f721219f7f81
anchor-sha256: src/aot/program/xr_backend_ir.h 1d9dca590e0a7ace9034939925bfe69fca8d60eb33b48d3915822cbf4bda42cf
anchor-sha256: src/aot/program/xr_backend_ir_internal.h c1360e511db5f08bb28aea04b0b4048b958f4778da8eecd7adeee88fba9f9942
anchor-sha256: src/aot/program/xr_backend_ir.c 5ad81581785cd541a9e5946cd098ddf17befc3c86338230713bbe8d4dd9afed4
anchor-sha256: src/aot/program/xr_backend_ir_verify.c d8d6c966fe9b59f79bf45d3366489fb79655bbe73f940234b6ab1544469e385e
anchor-sha256: src/aot/program/xr_backend_ir_emit_c.c d0183086df19c3be0a9c20b4904de0d5c864e0eaa89d25b6f872a5b0186ac0f0
anchor-sha256: src/aot/program/xr_native_artifact.c fc273aac15c76b9ffcd6300e7c77978a4f729bbbfcbf06c8018df85ba72d4f76
anchor-sha256: src/execution/xr_execution_identity.h 5783c870cd0d642c6d60983e24efcd183edbfbb63380ffae3254e5617af5fd51
anchor-sha256: src/execution/xr_execution_identity.c 857dc89de900a4eda6e71c9498aac3657779a93e68d9593cd4fefb374b84f0bf
anchor-sha256: scripts/check_xr_program_aot_contracts.py dd313ca4ba67c8c24a285f376ebedb92e0efe7d838e5d95286509e64d2af2d96
anchor-sha256: scripts/check_xr_program_aot_native.py 56822ee193168c2f76f549afbbaacaf3ed99f0236bf531b2500e52337881c2e2
anchor-sha256: scripts/check_xr_program_aot_providers.py 51b1bdc7b16aa8709ba082ad185dcd0ca59af2f3bc10428d8d4ef480c80acbc0
anchor-sha256: tests/unit/aot/test_xr_program_aot.c 63608f680e3f8a475ab8780843c03d03ec5357d032753f041209bae7aa387b36
anchor-sha256: tests/unit/program/test_xr_program_source_build.c 703c0492e6463f5cb293da9a317e225dda42f835ea2ad3165baaa64008d41841
