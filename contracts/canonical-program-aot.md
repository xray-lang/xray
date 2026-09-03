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

anchor-sha256: CMakeLists.txt d047f474b10bcc45b5dfb0dc3ebbff8099a20617a8f2791e8f74936ae13d6525
anchor-sha256: tests/unit/CMakeLists.txt 7344640b396e241ed4623a41f23ef7c58de86507c04a231be2737d2440e29538
anchor-sha256: xisa/core/registry.json 8fb9c70b946a02d637c3c74ff2e1dcf93e34376b3eff9e9699df180f01e4819c
anchor-sha256: contracts/canonical-program/architecture-identity.toml 844f5e20d293d2b74efda9d1755c7da2d9da27b9acc985b346dcdc54e1917ccd
anchor-sha256: contracts/canonical-program/operation-capability-matrix.json f118d1d1fcd6a66a9ba99d76582619326082c142d78c2521a00f4ede611a1069
anchor-sha256: contracts/canonical-program/xrprogram-aot-coverage.json 90662f9cb5ce27592baa64e1a72d06816e2d48165edf579673067ee5fe08fa46
anchor-sha256: src/aot/program/xr_backend_ir.h d0cafeda3702e5a406b09573adab96223942321f9dab06f4ef0ac32ed6a30349
anchor-sha256: src/aot/program/xr_backend_ir_internal.h 605828cb796bb9334565a554735841cb8a7b26b94d2dc781fcad50fca0b2296f
anchor-sha256: src/aot/program/xr_backend_ir.c 5ee0627fe60ae51faad024a7f2dd93df7a06221a75226f97ef6ecc748bd35a9d
anchor-sha256: src/aot/program/xr_backend_ir_verify.c 836f4322f9ca7e1e0f504dd2c4fbf456b946b9847d9cddd8d2ba664366878b55
anchor-sha256: src/aot/program/xr_backend_ir_emit_c.c 4f9a29f3ee848fccf4f333ef07f77e2e41622d802f1530d6e4ab1fe38b2813d3
anchor-sha256: src/aot/program/xr_native_artifact.c fc273aac15c76b9ffcd6300e7c77978a4f729bbbfcbf06c8018df85ba72d4f76
anchor-sha256: scripts/check_xr_program_aot_contracts.py 05ded6ee59451a29e4a6fb6f898e89e5b9b0cc49fa10451149dd4e79ecf741c2
anchor-sha256: scripts/check_xr_program_aot_native.py a1aa1f340832282b480e856fac654330492cf065f99bbd2791b1942b06370a1c
anchor-sha256: scripts/check_xr_program_aot_providers.py 51b1bdc7b16aa8709ba082ad185dcd0ca59af2f3bc10428d8d4ef480c80acbc0
anchor-sha256: tests/unit/aot/test_xr_program_aot.c ec7882c4a30361dd4ec4e99512347509335d02a6e1b1293ac24db7c7ebd56224
