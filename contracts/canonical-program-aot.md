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
anchor-sha256: tests/unit/CMakeLists.txt 4089786b4d2cc13bbdd6d5e7b96cbc73504daacca3feb49400b10fa23b3f81dc
anchor-sha256: xisa/core/registry.json 00044c7993db9758a2357ee0805159bd083818945ee5833b8d19c39d256caf88
anchor-sha256: contracts/canonical-program/architecture-identity.toml 844f5e20d293d2b74efda9d1755c7da2d9da27b9acc985b346dcdc54e1917ccd
anchor-sha256: contracts/canonical-program/operation-capability-matrix.json 1117f9345385ee738e11f2348a1ce91096de99741c64b204cb69af2dc781e31e
anchor-sha256: contracts/canonical-program/xrprogram-aot-coverage.json 7e3bdbb8c51d1986ea4c1d5970b0886524bbf1d8bea8dc4e4241cc2a2919335e
anchor-sha256: src/aot/program/xr_backend_ir.h d0cafeda3702e5a406b09573adab96223942321f9dab06f4ef0ac32ed6a30349
anchor-sha256: src/aot/program/xr_backend_ir_internal.h d055b218f106e91175b9226631f293ca0b4d4a5f1e8762ca8a49d87e039c69bc
anchor-sha256: src/aot/program/xr_backend_ir.c 801502462927b063e2b92347d102e0cc367a3e7ebc42ad5350f9fd01cf825886
anchor-sha256: src/aot/program/xr_backend_ir_verify.c c9c453397617e557c71b19a80b39f630ca8a62815e43f9c59c6000248482774e
anchor-sha256: src/aot/program/xr_backend_ir_emit_c.c 10223bca2710e71e370ae30330a670f36351bd6760496c1cbcd1671c777bb3d5
anchor-sha256: src/aot/program/xr_native_artifact.c fc273aac15c76b9ffcd6300e7c77978a4f729bbbfcbf06c8018df85ba72d4f76
anchor-sha256: scripts/check_xr_program_aot_contracts.py 6a3bbbcb5d131b7eb213fe707545a4baded9bc7191bdb734bfe56382b1f07871
anchor-sha256: scripts/check_xr_program_aot_native.py a1aa1f340832282b480e856fac654330492cf065f99bbd2791b1942b06370a1c
anchor-sha256: scripts/check_xr_program_aot_providers.py 51b1bdc7b16aa8709ba082ad185dcd0ca59af2f3bc10428d8d4ef480c80acbc0
anchor-sha256: tests/unit/aot/test_xr_program_aot.c ca5e89378d8776d1c3ad5c76de363bb20768374cf528e8c96d4f7434a2c8f9a5
