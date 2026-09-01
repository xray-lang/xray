# Canonical XrProgram AOT contract

The AOT compiler accepts only `XrValidatedProgram` bound through one `XrInstance`. It pins that
execution authority while building a private `XrBackendIR`, copies the verified operation graph,
and adds only physical value representations and target literals. Lowering and emission do not
query source, AST, Xi, TargetPlan, VM-private code, or the reference evaluator.

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

`XrNativeArtifact` owns actual native bytes. `NativeArtifactId` hashes `ExecutionId`, `BackendId`,
`ToolchainId`, `OptimizationPolicyId`, and those bytes. Toolchain identity is reconstructible from
provider version, target triple, codegen options, sysroot, runtime objects, and target profile
fingerprints; every partition is checked exactly and mismatches fail closed.

The pure-AOT walking-skeleton executable contains no VM, compiler, program-loader, TargetPlan, or
AOT-toolchain symbol and executes without program bytes. All fifteen active operations occur in
the generated-C behavioral fixture; checked overflow, wrapping overflow, and division by zero also
compile and execute as independent native cases. Full language operation families, high-risk
optimizations, public loader ABI, and package publication remain inactive.

anchor-sha256: CMakeLists.txt 24f1f6dfd0299ca706737b11ded219b96c3b644880321a96f7395e224c1413d1
anchor-sha256: tests/unit/CMakeLists.txt 220175a3ed455c54b598fdf4373d4bd22f5590fb86bf17f404da7578aeae23f0
anchor-sha256: xisa/core/registry.json c77e3511ccbec192559b75e2d3e1ca45f482a85801e4657900f51ec8924a68bb
anchor-sha256: contracts/canonical-program/architecture-identity.toml e5a3d23489bd9f2669d9d65b6a5ff963584a9bf8a7929d341019abedb0e00d7f
anchor-sha256: contracts/canonical-program/operation-capability-matrix.json eb261937b6d75ad5ab27d1e1c107d44bbd1c762fa4f4193bd6bdb1bf22ab4c42
anchor-sha256: contracts/canonical-program/xrprogram-aot-coverage.json 37b931117ae6b4d2e7878f187e9e0220b6c79a06e9f0fe8e119fd52928a48a47
anchor-sha256: src/aot/program/xr_backend_ir.h d0cafeda3702e5a406b09573adab96223942321f9dab06f4ef0ac32ed6a30349
anchor-sha256: src/aot/program/xr_backend_ir_internal.h 37c02e04f8d23bce343ecdd6768486716e7d5169083329ecf46df08178f90580
anchor-sha256: src/aot/program/xr_backend_ir.c acc11c7d22c45492282a66fdb30351808f26d6f8c1243b003838c71e0f69c876
anchor-sha256: src/aot/program/xr_backend_ir_verify.c a96e0e78f6c852abf48c7433d986df0bc84bab4e37571ea3dc73498c2d8d4d3e
anchor-sha256: src/aot/program/xr_backend_ir_emit_c.c 1e5a084c5450b60751e396091faff56cd9189dfc815a2e3c07f845f770acce3b
anchor-sha256: src/aot/program/xr_native_artifact.c fc273aac15c76b9ffcd6300e7c77978a4f729bbbfcbf06c8018df85ba72d4f76
anchor-sha256: scripts/check_xr_program_aot_contracts.py 88b316422f8e2b104eae4cb95aecc4698cf9c11d5a1cf2a5bedaeb9b7fc51653
anchor-sha256: scripts/check_xr_program_aot_native.py a1aa1f340832282b480e856fac654330492cf065f99bbd2791b1942b06370a1c
anchor-sha256: scripts/check_xr_program_aot_providers.py 51b1bdc7b16aa8709ba082ad185dcd0ca59af2f3bc10428d8d4ef480c80acbc0
anchor-sha256: tests/unit/aot/test_xr_program_aot.c 1fa67d58c8ff2669275e33a85e7348ed293f3e408ce8f2c16167f95e3a9ff7c7
