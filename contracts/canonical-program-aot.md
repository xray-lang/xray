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

The `core.provider.call` executor foundation lowers an exact program requirement/operation index
to a typed callback in the private `XrAotContext` and normalizes refusal to the canonical trap.
The standalone native harness supplies an arbitrary callback only to prove portable lowering and
native execution. It does not prove that an AOT artifact has captured an exact generation binding;
that closure remains inactive until the generated entry adapter owns a live generation lease.

`XrNativeArtifact` owns actual native bytes. `NativeArtifactId` hashes `ExecutionId`, `BackendId`,
`ToolchainId`, `OptimizationPolicyId`, and those bytes. Toolchain identity is reconstructible from
provider version, target triple, codegen options, sysroot, runtime objects, and target profile
fingerprints; every partition is checked exactly and mismatches fail closed.

The pure-AOT walking-skeleton executable contains no VM, compiler, program-loader, TargetPlan, or
AOT-toolchain symbol and executes without program bytes. All thirty-eight current CoreSpec operations
have private BackendIR/C lowering; Wave 2 aggregate and variant rows become portable C11 structs and
tagged unions without a VM carrier or shared local layout. The operation matrix separately records
which rows have a production source owner. Checked overflow, wrapping overflow, and division by zero
also compile and execute as independent native cases. Full language operation families, high-risk
optimizations, public loader ABI, and package publication remain inactive.

anchor-sha256: CMakeLists.txt 830633d6559aded38a0233a76f93d73abe770412e7e77cc283e594e260b4661c
anchor-sha256: tests/unit/CMakeLists.txt 680cd567fa2d0f2488ca9f3c2c8d5afaf33ef50bfbdbd2a2d33a02343192fd31
anchor-sha256: xisa/core/registry.json 5a3008c03058fe1d8c426848d1cc077d4a17156abf34c1393d654e10acb0a1e4
anchor-sha256: contracts/canonical-program/architecture-identity.toml 844f5e20d293d2b74efda9d1755c7da2d9da27b9acc985b346dcdc54e1917ccd
anchor-sha256: contracts/canonical-program/operation-capability-matrix.json eaca434672db959842bc5b2c2fb7d4517c8f0c19dfda4490d13440dce984b52a
anchor-sha256: contracts/canonical-program/xrprogram-aot-coverage.json bf262285d5cc57f952f88f9856ae47cc0bee11765ba687071e42c9ac2d7a9148
anchor-sha256: src/aot/program/xr_backend_ir.h d0cafeda3702e5a406b09573adab96223942321f9dab06f4ef0ac32ed6a30349
anchor-sha256: src/aot/program/xr_backend_ir_internal.h 5e5102b94208f46dfbd4b8f1feb59ad6dab42bd4a8e9ecfaea1bd97d6a92d83c
anchor-sha256: src/aot/program/xr_backend_ir.c 220d636a5d28b739141b5ef16ecb267e21648b241eb93f60281aa1ff42df89f0
anchor-sha256: src/aot/program/xr_backend_ir_verify.c f3faeec4cf0724be3eb9a4d5c9548732b94be3d1155825d5231597a3e0bb4ecb
anchor-sha256: src/aot/program/xr_backend_ir_emit_c.c dec1b959e2108d13bf9ba843ab253d0ed33ad752cddf5b6fe28787c01868bd95
anchor-sha256: src/aot/program/xr_native_artifact.c fc273aac15c76b9ffcd6300e7c77978a4f729bbbfcbf06c8018df85ba72d4f76
anchor-sha256: scripts/check_xr_program_aot_contracts.py 616086796cb35e40731aa80ae09af1f18d31c58bd88d47f8b5957dab6bd3c10f
anchor-sha256: scripts/check_xr_program_aot_native.py a90027218e16c75351ca8de8dbb68f188d20255ba4ad23324a08416af0eda56d
anchor-sha256: scripts/check_xr_program_aot_providers.py 51b1bdc7b16aa8709ba082ad185dcd0ca59af2f3bc10428d8d4ef480c80acbc0
anchor-sha256: tests/unit/aot/test_xr_program_aot.c 4cf454958be5684f5a2ec38c23a5cc11716dd0573626b4f279ea550bc41ecdd3
