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

anchor-sha256: CMakeLists.txt a2c23451b1bda53528dc233ac464c95c5434bb01fc165de4418c89645b7ca310
anchor-sha256: tests/unit/CMakeLists.txt bfec61c945ff48a5eeb8139fc73606ec050fc172fa569c568f352d3fc053ed1e
anchor-sha256: xisa/core/registry.json 5dbedbc83a541e456d67e15ec0ced2282f23aaf843f101d8d7eb33ae6a85c619
anchor-sha256: contracts/canonical-program/architecture-identity.toml 844f5e20d293d2b74efda9d1755c7da2d9da27b9acc985b346dcdc54e1917ccd
anchor-sha256: contracts/canonical-program/operation-capability-matrix.json 2f33e8745a0441caa8aaed08a28c60223bc73a4e2e4ff69bb2b5e231ca5932c5
anchor-sha256: contracts/canonical-program/xrprogram-aot-coverage.json 443fa248a0068185d069d717f6042deee2ae0c02013eaf960bd78615ff19a962
anchor-sha256: src/aot/program/xr_backend_ir.h d0cafeda3702e5a406b09573adab96223942321f9dab06f4ef0ac32ed6a30349
anchor-sha256: src/aot/program/xr_backend_ir_internal.h 3cc29a6374d0d87ff0fa6d819b3af26c126a6b02993fe114a11308f3a45d59d3
anchor-sha256: src/aot/program/xr_backend_ir.c 037cc918de808f1a86f3d73707c472d3df55999a695c9bb83dff61878573797b
anchor-sha256: src/aot/program/xr_backend_ir_verify.c af890cc3c911f1789844b88db55684ecc464979f3eb34086f2c3677b29c187eb
anchor-sha256: src/aot/program/xr_backend_ir_emit_c.c e133565773e5b64d0d27b82a8d48d6ea15835f4958d8b30b7b6d253b52ee42f7
anchor-sha256: src/aot/program/xr_native_artifact.c fc273aac15c76b9ffcd6300e7c77978a4f729bbbfcbf06c8018df85ba72d4f76
anchor-sha256: scripts/check_xr_program_aot_contracts.py 6a3bbbcb5d131b7eb213fe707545a4baded9bc7191bdb734bfe56382b1f07871
anchor-sha256: scripts/check_xr_program_aot_native.py a1aa1f340832282b480e856fac654330492cf065f99bbd2791b1942b06370a1c
anchor-sha256: scripts/check_xr_program_aot_providers.py 51b1bdc7b16aa8709ba082ad185dcd0ca59af2f3bc10428d8d4ef480c80acbc0
anchor-sha256: tests/unit/aot/test_xr_program_aot.c 04069851ba1d0effe5c07eb2d1cca309e6d34832b1e215142d9513504f08c8d7
