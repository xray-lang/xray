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

anchor-sha256: CMakeLists.txt 5f1fa0ef1eefff52eaa31a5873a03cc59bd160805ee09c8f35091d6de501ce0d
anchor-sha256: tests/unit/CMakeLists.txt 1037b79ccbbe0e0e600231fee7cb7938d8fb9909845ce67d0f171c649495c412
anchor-sha256: xisa/core/registry.json 86a6f14d8971d7217de112cef7147c78b9b2328f7e778530ab9317cd8525555c
anchor-sha256: contracts/canonical-program/architecture-identity.toml 844f5e20d293d2b74efda9d1755c7da2d9da27b9acc985b346dcdc54e1917ccd
anchor-sha256: contracts/canonical-program/operation-capability-matrix.json c1b203f8a59f82075ca33e6916b18ce2d0d4bdd874a45993f5173d944dd952b9
anchor-sha256: contracts/canonical-program/xrprogram-aot-coverage.json 5e445629b30bebb2b16ed69659d091b7c586bab1bb26966d17dea22a04fc3184
anchor-sha256: src/aot/program/xr_backend_ir.h 72d7f3e9368b29dd39d2858cf0c2b8b3ab866f6bf68e239ae9747217fbde5d37
anchor-sha256: src/aot/program/xr_backend_ir_internal.h 63f79292db4bf8586ec45e9436a53b26194afb6683adffaea070960b3817225d
anchor-sha256: src/aot/program/xr_backend_ir.c 03f10e7d88edbde1e632626882df6643d2d777b3097916721deab9deae01b1db
anchor-sha256: src/aot/program/xr_backend_ir_verify.c d1a907fc82739ba4cd081810e25662d3e6171185000c9ec5a61e4e35073b443d
anchor-sha256: src/aot/program/xr_backend_ir_emit_c.c 02e8947f746a950d7eae9c737bdf06d5c6f3ece2aafb2804f5615bd252983642
anchor-sha256: src/aot/program/xr_native_artifact.c fc273aac15c76b9ffcd6300e7c77978a4f729bbbfcbf06c8018df85ba72d4f76
anchor-sha256: src/execution/xr_execution_identity.h 5783c870cd0d642c6d60983e24efcd183edbfbb63380ffae3254e5617af5fd51
anchor-sha256: src/execution/xr_execution_identity.c 857dc89de900a4eda6e71c9498aac3657779a93e68d9593cd4fefb374b84f0bf
anchor-sha256: scripts/check_xr_program_aot_contracts.py dd313ca4ba67c8c24a285f376ebedb92e0efe7d838e5d95286509e64d2af2d96
anchor-sha256: scripts/check_xr_program_aot_native.py a90027218e16c75351ca8de8dbb68f188d20255ba4ad23324a08416af0eda56d
anchor-sha256: scripts/check_xr_program_aot_providers.py 51b1bdc7b16aa8709ba082ad185dcd0ca59af2f3bc10428d8d4ef480c80acbc0
anchor-sha256: tests/unit/aot/test_xr_program_aot.c 96d1b3a31d25c28a11da6b7b58e77bed72c9b559b25bc5d4052922c8c2266b2e
