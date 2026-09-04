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

anchor-sha256: CMakeLists.txt 830633d6559aded38a0233a76f93d73abe770412e7e77cc283e594e260b4661c
anchor-sha256: tests/unit/CMakeLists.txt f37fc602774aa2d939332fcf671aeffd8e4ac12a4ee7696cc5da07fac2967d07
anchor-sha256: xisa/core/registry.json 8b1f4a8cf46d316c4818809466283fb8b1e633e5e022b7b1d66ad120b9487300
anchor-sha256: contracts/canonical-program/architecture-identity.toml 844f5e20d293d2b74efda9d1755c7da2d9da27b9acc985b346dcdc54e1917ccd
anchor-sha256: contracts/canonical-program/operation-capability-matrix.json 5a0212b3867ced1e630f58f5a2320982c6eed819d6e4211a3fbe49ec2c47c1fe
anchor-sha256: contracts/canonical-program/xrprogram-aot-coverage.json 90662f9cb5ce27592baa64e1a72d06816e2d48165edf579673067ee5fe08fa46
anchor-sha256: src/aot/program/xr_backend_ir.h d0cafeda3702e5a406b09573adab96223942321f9dab06f4ef0ac32ed6a30349
anchor-sha256: src/aot/program/xr_backend_ir_internal.h d43ecaa7b28028afe45f0d63c8de8e7e001d94d3d0958d2f640c552719a76b00
anchor-sha256: src/aot/program/xr_backend_ir.c 93e4defcda5da2a70cf3517d217992da6a7e22b826f35f0123cecd97847b2544
anchor-sha256: src/aot/program/xr_backend_ir_verify.c 5a497a7ceda188f27de4ab85fdf9fee1c34c83b0160ae68cdd7a7d936bdffadf
anchor-sha256: src/aot/program/xr_backend_ir_emit_c.c cf9fcda0120aaf69e95eb12ecc526aa3e524b35d74d562940837b24d803b42c3
anchor-sha256: src/aot/program/xr_native_artifact.c fc273aac15c76b9ffcd6300e7c77978a4f729bbbfcbf06c8018df85ba72d4f76
anchor-sha256: scripts/check_xr_program_aot_contracts.py 616086796cb35e40731aa80ae09af1f18d31c58bd88d47f8b5957dab6bd3c10f
anchor-sha256: scripts/check_xr_program_aot_native.py a90027218e16c75351ca8de8dbb68f188d20255ba4ad23324a08416af0eda56d
anchor-sha256: scripts/check_xr_program_aot_providers.py 51b1bdc7b16aa8709ba082ad185dcd0ca59af2f3bc10428d8d4ef480c80acbc0
anchor-sha256: tests/unit/aot/test_xr_program_aot.c d4f69e1ddb7cacc39b0d02ccc95d14608b76d359f6fd89fadf5cd370c72a3a7d
