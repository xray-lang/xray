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
`core.output.group.i64` formats the exact signed decimal line and performs one output-write call.
Embedded AOT receives that typed callback from its host; hosted standalone AOT binds the generated
entry directly to a binary stdout sink, while freestanding standalone emission without an embedder
fails closed. The compiler neither receives nor captures a live provider instance or lease.

`XrNativeArtifact` owns actual native bytes. `NativeArtifactId` hashes `ExecutionId`, `BackendId`,
`ToolchainId`, `OptimizationPolicyId`, and those bytes. Toolchain identity is reconstructible from
provider version, target triple, codegen options, sysroot, runtime objects, and target profile
fingerprints; every partition is checked exactly and mismatches fail closed.

The pure-AOT walking-skeleton executable contains no VM, compiler, program-loader, TargetPlan, or
AOT-toolchain symbol and executes without program bytes. All forty-seven current CoreSpec operations
have private BackendIR/C lowering; Wave 2 aggregate and variant rows become portable C11 structs and
tagged unions without a VM carrier or shared local layout. A source-built higher-order function
fixture proves that a callable borrowed through an ordinary function parameter has the same
reference, VM, and native generated-C result, and that repeated C emission is byte-identical. The operation matrix separately records
which rows have a production source owner. Checked overflow, wrapping overflow, and division by zero
also compile and execute as independent native cases. Full language operation families, high-risk
optimizations, public loader ABI, and package publication remain inactive.

anchor-sha256: CMakeLists.txt 2a14f3d1a43bfc8b6f7050c4317b3b7254a70593550a3a47d9c30c9970da899a
anchor-sha256: tests/unit/CMakeLists.txt a4578a3cfb7afaa79389e57f6a7213d51fe054b21cf17d9c2e727ad94fa02a68
anchor-sha256: xisa/core/registry.json c5c2b2be5583b82ab439a631fe71ac47f346b92a62d5c169d587b9effaf3758b
anchor-sha256: contracts/canonical-program/architecture-identity.toml 844f5e20d293d2b74efda9d1755c7da2d9da27b9acc985b346dcdc54e1917ccd
anchor-sha256: contracts/canonical-program/operation-capability-matrix.json c9903e9ac14f69b319910c0ee5390499e261e5963a92ea03580042a6e50641dd
anchor-sha256: contracts/canonical-program/xrprogram-aot-coverage.json dffca121e3f25bfdff7805bcb297a017d690c34eecd45e5289a9a432c70da897
anchor-sha256: src/aot/program/xr_backend_ir.h 1d9dca590e0a7ace9034939925bfe69fca8d60eb33b48d3915822cbf4bda42cf
anchor-sha256: src/aot/program/xr_backend_ir_internal.h c1360e511db5f08bb28aea04b0b4048b958f4778da8eecd7adeee88fba9f9942
anchor-sha256: src/aot/program/xr_backend_ir.c 68278180675df0079b23ae4e57a785e77b2b6746bbe0b726ffbd46bd02ca17ba
anchor-sha256: src/aot/program/xr_backend_ir_verify.c 6ad6e6055ed2c56d1e5b0d0edbf6f00aa7743a7499bfecc2517be50778709054
anchor-sha256: src/aot/program/xr_backend_ir_emit_c.c 068b520a4425c2bc6476b93c06a884087107ef7d77999cb4d22efe03b3259f39
anchor-sha256: src/aot/program/xr_native_artifact.c fc273aac15c76b9ffcd6300e7c77978a4f729bbbfcbf06c8018df85ba72d4f76
anchor-sha256: src/execution/xr_execution_identity.h 5783c870cd0d642c6d60983e24efcd183edbfbb63380ffae3254e5617af5fd51
anchor-sha256: src/execution/xr_execution_identity.c 857dc89de900a4eda6e71c9498aac3657779a93e68d9593cd4fefb374b84f0bf
anchor-sha256: scripts/check_xr_program_aot_contracts.py dd313ca4ba67c8c24a285f376ebedb92e0efe7d838e5d95286509e64d2af2d96
anchor-sha256: scripts/check_xr_program_aot_native.py 56822ee193168c2f76f549afbbaacaf3ed99f0236bf531b2500e52337881c2e2
anchor-sha256: scripts/check_xr_program_aot_providers.py 51b1bdc7b16aa8709ba082ad185dcd0ca59af2f3bc10428d8d4ef480c80acbc0
anchor-sha256: tests/unit/aot/test_xr_program_aot.c a9cd6a835fb71bd0a2532be4b50c53bc60afbc43758c362a400ccf1143d7e11f
