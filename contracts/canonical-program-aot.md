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
AOT-toolchain symbol and executes without program bytes. All forty-six current CoreSpec operations
have private BackendIR/C lowering; Wave 2 aggregate and variant rows become portable C11 structs and
tagged unions without a VM carrier or shared local layout. The operation matrix separately records
which rows have a production source owner. Checked overflow, wrapping overflow, and division by zero
also compile and execute as independent native cases. Full language operation families, high-risk
optimizations, public loader ABI, and package publication remain inactive.

anchor-sha256: CMakeLists.txt ab85e7a0caeaeeb4d9a7b6e673a44a164d165e9c17f13621333f6cfccbc23d70
anchor-sha256: tests/unit/CMakeLists.txt 8d6bdd520031e9b0b945b37b7d9a9c7fe92b15a0562b9173ce8b40c9955037d6
anchor-sha256: xisa/core/registry.json 58a22dfeca6bfcbf53b43437d3f5b63334c30eaa27ff17ccf4cd4f1eed445e36
anchor-sha256: contracts/canonical-program/architecture-identity.toml 844f5e20d293d2b74efda9d1755c7da2d9da27b9acc985b346dcdc54e1917ccd
anchor-sha256: contracts/canonical-program/operation-capability-matrix.json 3f93c0e324baad4651c9473a65577a5f6bfa37caadc0677edf11a490301e0358
anchor-sha256: contracts/canonical-program/xrprogram-aot-coverage.json 4b9d7c81666993226325a061700eb92e3553f540478b8b38aa91b202a40e6436
anchor-sha256: src/aot/program/xr_backend_ir.h 1d9dca590e0a7ace9034939925bfe69fca8d60eb33b48d3915822cbf4bda42cf
anchor-sha256: src/aot/program/xr_backend_ir_internal.h 2049ab3ed50c6bf6a82d4eda2c2832d2569c0d4910f69a29ffbfb5cce479a422
anchor-sha256: src/aot/program/xr_backend_ir.c 7de678c64b390a0a123245a011ec1f65a6c68bb5712c6065d1febaa740f5f21c
anchor-sha256: src/aot/program/xr_backend_ir_verify.c e964d6fe7fda2082bfda2c3800e1fe7bee96e3111adea2131668eb10a357d285
anchor-sha256: src/aot/program/xr_backend_ir_emit_c.c 933a04a7bf072b7a231ca2264b6427c852a7db7ccc791a713140b899638ce980
anchor-sha256: src/aot/program/xr_native_artifact.c fc273aac15c76b9ffcd6300e7c77978a4f729bbbfcbf06c8018df85ba72d4f76
anchor-sha256: src/execution/xr_execution_identity.h 5783c870cd0d642c6d60983e24efcd183edbfbb63380ffae3254e5617af5fd51
anchor-sha256: src/execution/xr_execution_identity.c 857dc89de900a4eda6e71c9498aac3657779a93e68d9593cd4fefb374b84f0bf
anchor-sha256: scripts/check_xr_program_aot_contracts.py dd313ca4ba67c8c24a285f376ebedb92e0efe7d838e5d95286509e64d2af2d96
anchor-sha256: scripts/check_xr_program_aot_native.py 56822ee193168c2f76f549afbbaacaf3ed99f0236bf531b2500e52337881c2e2
anchor-sha256: scripts/check_xr_program_aot_providers.py 51b1bdc7b16aa8709ba082ad185dcd0ca59af2f3bc10428d8d4ef480c80acbc0
anchor-sha256: tests/unit/aot/test_xr_program_aot.c 594eeee9dfff9a9a21bfbdd1a1fb77f119c7dbf88b13e46083aae6112c655644
