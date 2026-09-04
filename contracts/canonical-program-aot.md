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

anchor-sha256: CMakeLists.txt 100df49512a0a67e30f3e2025ee89b40115afe70cfa2031fffb856ee68931648
anchor-sha256: tests/unit/CMakeLists.txt a4fc11daa1720d43c2e9e93b74848f726f45b8c6dcf8810214483cb1b6fb1156
anchor-sha256: xisa/core/registry.json 7fa5c92be0b89b9f1654d9bdeb7ffb23e6ea96c5a5c8619e09f37c63559f6e91
anchor-sha256: contracts/canonical-program/architecture-identity.toml 844f5e20d293d2b74efda9d1755c7da2d9da27b9acc985b346dcdc54e1917ccd
anchor-sha256: contracts/canonical-program/operation-capability-matrix.json 069b5565ff1cec2a7aebe77e4a80ddfbe08835d70ff2bcba1cdab70ad13e3472
anchor-sha256: contracts/canonical-program/xrprogram-aot-coverage.json 76de732d965b6eb58bfc3150df0f36aa5ce32ff2373bb2e67847e9ba53b45808
anchor-sha256: src/aot/program/xr_backend_ir.h d0cafeda3702e5a406b09573adab96223942321f9dab06f4ef0ac32ed6a30349
anchor-sha256: src/aot/program/xr_backend_ir_internal.h 54d76b775cbd71586132d15763651ffde9ff215b7bfa070445b5db3dedf5ebfb
anchor-sha256: src/aot/program/xr_backend_ir.c b5b9794c45ef3457248132afae75111f89063a2f64805919d4dacb19cad6528d
anchor-sha256: src/aot/program/xr_backend_ir_verify.c 12ecd1a560c8934b27677b2fc2af6bdb11fb9f5a89bc9df23a376ae5085697fe
anchor-sha256: src/aot/program/xr_backend_ir_emit_c.c d6748b477c5669766c3c9bf9168929aadadaba62c273e368febe68dcdd7645a4
anchor-sha256: src/aot/program/xr_native_artifact.c fc273aac15c76b9ffcd6300e7c77978a4f729bbbfcbf06c8018df85ba72d4f76
anchor-sha256: scripts/check_xr_program_aot_contracts.py 61657cd5870e1b965a20800fbaff4bbec7f4b3695bfad1830df0b2a8f7d06285
anchor-sha256: scripts/check_xr_program_aot_native.py a90027218e16c75351ca8de8dbb68f188d20255ba4ad23324a08416af0eda56d
anchor-sha256: scripts/check_xr_program_aot_providers.py 51b1bdc7b16aa8709ba082ad185dcd0ca59af2f3bc10428d8d4ef480c80acbc0
anchor-sha256: tests/unit/aot/test_xr_program_aot.c af4039413ce0b2a7ad7430bb111b9951f8b53841946e571508085759cf9d5b65
