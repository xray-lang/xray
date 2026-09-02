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

anchor-sha256: CMakeLists.txt af8549125f9746d3915b0fff88e62c23eea9e818afc253822f4e3ba46e831083
anchor-sha256: tests/unit/CMakeLists.txt 4089786b4d2cc13bbdd6d5e7b96cbc73504daacca3feb49400b10fa23b3f81dc
anchor-sha256: xisa/core/registry.json 18fbbba56262869fecbba8e262aed7d2bceb9d80e04c04ef99b5e77a6675403d
anchor-sha256: contracts/canonical-program/architecture-identity.toml 844f5e20d293d2b74efda9d1755c7da2d9da27b9acc985b346dcdc54e1917ccd
anchor-sha256: contracts/canonical-program/operation-capability-matrix.json b910da115513d59fa0ae903470d895ec3772eed82aad94039796229f59cef8fe
anchor-sha256: contracts/canonical-program/xrprogram-aot-coverage.json b557140133afc6bfcb44f7806692840d802bb3435f7eca8482df2a3d70146572
anchor-sha256: src/aot/program/xr_backend_ir.h d0cafeda3702e5a406b09573adab96223942321f9dab06f4ef0ac32ed6a30349
anchor-sha256: src/aot/program/xr_backend_ir_internal.h 22ffa78bcb4074d54c29cdfaf0848506ef5f9147e2f88ae1c6e4f5e44df23b58
anchor-sha256: src/aot/program/xr_backend_ir.c cca8bb64c0476b31dbf705a611d8322bcc37c35ded8a4fc78f532a8c9c44c1fc
anchor-sha256: src/aot/program/xr_backend_ir_verify.c f118275c31bd8043a0d67fdf8b7d8cccbe594d94b77a926136ec3bc9fc51a847
anchor-sha256: src/aot/program/xr_backend_ir_emit_c.c f6c4469c29cf72e32a8a81f1a78d68c6901ff48bdfc420b69708a2adf4de4c45
anchor-sha256: src/aot/program/xr_native_artifact.c fc273aac15c76b9ffcd6300e7c77978a4f729bbbfcbf06c8018df85ba72d4f76
anchor-sha256: scripts/check_xr_program_aot_contracts.py fc625ea2c9b5c45ec643d291b12ce8973ded3eae199ef58f4e14d8f4b33cf22c
anchor-sha256: scripts/check_xr_program_aot_native.py a1aa1f340832282b480e856fac654330492cf065f99bbd2791b1942b06370a1c
anchor-sha256: scripts/check_xr_program_aot_providers.py 51b1bdc7b16aa8709ba082ad185dcd0ca59af2f3bc10428d8d4ef480c80acbc0
anchor-sha256: tests/unit/aot/test_xr_program_aot.c 84f1323c5f8612c4c41e00f5238a21618ef293445a2831e59dd4e1f50151687f
