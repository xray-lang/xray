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
AOT-toolchain symbol and executes without program bytes. All twenty-one current CoreSpec operations
have private BackendIR/C lowering; Wave 2 aggregate and variant rows become portable C11 structs and
tagged unions without a VM carrier or shared local layout. The operation matrix separately records
which rows have a production source owner. Checked overflow, wrapping overflow, and division by zero
also compile and execute as independent native cases. Full language operation families, high-risk
optimizations, public loader ABI, and package publication remain inactive.

anchor-sha256: CMakeLists.txt 8e2fe64fc2ec8cbb81dc48deab1ae0f38a7820f7fe610c463eb53d0a2e094d1c
anchor-sha256: tests/unit/CMakeLists.txt 5550bab4bebd17f794995600de93c03bbd6558e030cfb59542b8eda4bf6e052f
anchor-sha256: xisa/core/registry.json c68d7bb1c78179795e889e879abcef82388fd153b5cb1ea26a52afe407ce5536
anchor-sha256: contracts/canonical-program/architecture-identity.toml 844f5e20d293d2b74efda9d1755c7da2d9da27b9acc985b346dcdc54e1917ccd
anchor-sha256: contracts/canonical-program/operation-capability-matrix.json 1e55f5561eba92f443c5a9a7bb533064d170cc6964e6a9d9dac9febb5f4e2539
anchor-sha256: contracts/canonical-program/xrprogram-aot-coverage.json ea5c42c6adb2b8cf838e8fe6febde7e9275534af966547c21397578390a6dedb
anchor-sha256: src/aot/program/xr_backend_ir.h d35248d0b4b0e46429216c5b8181474065164e30952f40de314e934ff8450208
anchor-sha256: src/aot/program/xr_backend_ir_internal.h d5317d9b1d67c9181e1d6098e19d7c34f88fc31ec34c80b75d6a216e19131f8d
anchor-sha256: src/aot/program/xr_backend_ir.c 328031ef73380c625254837043b135dbb78a825b1796f6a86ef1591f1a226650
anchor-sha256: src/aot/program/xr_backend_ir_verify.c 93976d008ded759e9d925453a89eed737d4d4e85b82b80a2603ac07218f97a26
anchor-sha256: src/aot/program/xr_backend_ir_emit_c.c 3ef006d9aece2346d96bdf46e90b65ceffeaf5a07d9bbb95cc9927ee30f866f9
anchor-sha256: src/aot/program/xr_native_artifact.c fc273aac15c76b9ffcd6300e7c77978a4f729bbbfcbf06c8018df85ba72d4f76
anchor-sha256: scripts/check_xr_program_aot_contracts.py 616086796cb35e40731aa80ae09af1f18d31c58bd88d47f8b5957dab6bd3c10f
anchor-sha256: scripts/check_xr_program_aot_native.py a90027218e16c75351ca8de8dbb68f188d20255ba4ad23324a08416af0eda56d
anchor-sha256: scripts/check_xr_program_aot_providers.py 51b1bdc7b16aa8709ba082ad185dcd0ca59af2f3bc10428d8d4ef480c80acbc0
anchor-sha256: tests/unit/aot/test_xr_program_aot.c 3179c24b8bc0b02422d7c5e409359fef30d3f3627272042b3760af41d3c2aede
