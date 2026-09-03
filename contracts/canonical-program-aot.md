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
anchor-sha256: tests/unit/CMakeLists.txt 38c40402720e2eb8673b045d7db889ff22f20e9dc281af1071d222da91d55d55
anchor-sha256: xisa/core/registry.json fb8a16b3180835d0fb79da7f4f517cf14ef047f511fb2f10035c11c9b4477d33
anchor-sha256: contracts/canonical-program/architecture-identity.toml 844f5e20d293d2b74efda9d1755c7da2d9da27b9acc985b346dcdc54e1917ccd
anchor-sha256: contracts/canonical-program/operation-capability-matrix.json cfee9e77e68452002c1afc7290ae88124cefe8df0875bd38559fa0133c690dbd
anchor-sha256: contracts/canonical-program/xrprogram-aot-coverage.json 8926bf5b148afafb3079008faddb325b0b90bb221bcd9841bdc827612ae0d539
anchor-sha256: src/aot/program/xr_backend_ir.h d0cafeda3702e5a406b09573adab96223942321f9dab06f4ef0ac32ed6a30349
anchor-sha256: src/aot/program/xr_backend_ir_internal.h 2c7e9be889ebc91943eb0b8437033314a5ea8ac71b0ebb207171504dc6248a27
anchor-sha256: src/aot/program/xr_backend_ir.c f6e9697319ec7d749cd6f65783d02c202fd4e60226d016aec4c5e7d3fef9184b
anchor-sha256: src/aot/program/xr_backend_ir_verify.c 9d62924ad8d9b9f89b4570fb3cbc52e14e7190dcbdaee9f9bf458b4f60aac061
anchor-sha256: src/aot/program/xr_backend_ir_emit_c.c 13ab6c4c474bb063ed11055cb4f35e77d10ac10becf1d4868cc881513f087f76
anchor-sha256: src/aot/program/xr_native_artifact.c fc273aac15c76b9ffcd6300e7c77978a4f729bbbfcbf06c8018df85ba72d4f76
anchor-sha256: scripts/check_xr_program_aot_contracts.py 6a3bbbcb5d131b7eb213fe707545a4baded9bc7191bdb734bfe56382b1f07871
anchor-sha256: scripts/check_xr_program_aot_native.py a1aa1f340832282b480e856fac654330492cf065f99bbd2791b1942b06370a1c
anchor-sha256: scripts/check_xr_program_aot_providers.py 51b1bdc7b16aa8709ba082ad185dcd0ca59af2f3bc10428d8d4ef480c80acbc0
anchor-sha256: tests/unit/aot/test_xr_program_aot.c 4376e4aa8c20a65d3bd49133a6ea86dd762949340b533706b111f19e033e5ad3
