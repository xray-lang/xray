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

The `core.provider.call` executor foundation lowers an exact program requirement/operation index
to a typed callback slot in the private runtime `XrAotContext` and normalizes refusal to the
canonical trap. The compiler neither receives nor captures that runtime provider instance. The
standalone native harness supplies a callback only at generated-code execution time to prove
portable lowering and native execution; product provider qualification remains separately gated.

`XrNativeArtifact` owns actual native bytes. `NativeArtifactId` hashes `ExecutionId`, `BackendId`,
`ToolchainId`, `OptimizationPolicyId`, and those bytes. Toolchain identity is reconstructible from
provider version, target triple, codegen options, sysroot, runtime objects, and target profile
fingerprints; every partition is checked exactly and mismatches fail closed.

The pure-AOT walking-skeleton executable contains no VM, compiler, program-loader, TargetPlan, or
AOT-toolchain symbol and executes without program bytes. All forty-four current CoreSpec operations
have private BackendIR/C lowering; Wave 2 aggregate and variant rows become portable C11 structs and
tagged unions without a VM carrier or shared local layout. The operation matrix separately records
which rows have a production source owner. Checked overflow, wrapping overflow, and division by zero
also compile and execute as independent native cases. Full language operation families, high-risk
optimizations, public loader ABI, and package publication remain inactive.

anchor-sha256: CMakeLists.txt 7f5171ebf0ab6b0ecc2ef7494e44193014600951061b8476bbeff2e6533e75fa
anchor-sha256: tests/unit/CMakeLists.txt cf1f8a302fdcfc07ddbdca70b80d158e42022f0d08d6386efc178b74953e9d5e
anchor-sha256: xisa/core/registry.json cfc0bb16d59982012a09c178ec68311658a75c27223774087e879f5445616258
anchor-sha256: contracts/canonical-program/architecture-identity.toml 844f5e20d293d2b74efda9d1755c7da2d9da27b9acc985b346dcdc54e1917ccd
anchor-sha256: contracts/canonical-program/operation-capability-matrix.json 159d57ccd13f0e900ee2fc7dc1c8322f77d764341979f150ed9fad15d09e596d
anchor-sha256: contracts/canonical-program/xrprogram-aot-coverage.json a08964220eb5ef63ef947f872aa948629989ead5a3c367cbf28a23db63859b02
anchor-sha256: src/aot/program/xr_backend_ir.h 1d9dca590e0a7ace9034939925bfe69fca8d60eb33b48d3915822cbf4bda42cf
anchor-sha256: src/aot/program/xr_backend_ir_internal.h 2049ab3ed50c6bf6a82d4eda2c2832d2569c0d4910f69a29ffbfb5cce479a422
anchor-sha256: src/aot/program/xr_backend_ir.c ffbd0f46305b6212cd93c1251763b9df8de75a96330bd1f2c15c7229ffb4923d
anchor-sha256: src/aot/program/xr_backend_ir_verify.c dc7298cfab8e95440cbf14a63cbf864013fef4412e341f703f966ec4f672f64e
anchor-sha256: src/aot/program/xr_backend_ir_emit_c.c 357d4715bb203ae50af9fe6c5e6390c77882a9445836f0473b56efd81af59a3c
anchor-sha256: src/aot/program/xr_native_artifact.c fc273aac15c76b9ffcd6300e7c77978a4f729bbbfcbf06c8018df85ba72d4f76
anchor-sha256: src/execution/xr_execution_identity.h 5783c870cd0d642c6d60983e24efcd183edbfbb63380ffae3254e5617af5fd51
anchor-sha256: src/execution/xr_execution_identity.c 857dc89de900a4eda6e71c9498aac3657779a93e68d9593cd4fefb374b84f0bf
anchor-sha256: scripts/check_xr_program_aot_contracts.py dd313ca4ba67c8c24a285f376ebedb92e0efe7d838e5d95286509e64d2af2d96
anchor-sha256: scripts/check_xr_program_aot_native.py a90027218e16c75351ca8de8dbb68f188d20255ba4ad23324a08416af0eda56d
anchor-sha256: scripts/check_xr_program_aot_providers.py 51b1bdc7b16aa8709ba082ad185dcd0ca59af2f3bc10428d8d4ef480c80acbc0
anchor-sha256: tests/unit/aot/test_xr_program_aot.c 5df6decda86dfafa8c3cd5297d2244d12b3c949fe1c916aabcd21a3259c09bab
