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

anchor-sha256: CMakeLists.txt 888ea2e2045ce386d4d828e8da360294ee72d3b73a7f2273a6d79ea7e3b02475
anchor-sha256: tests/unit/CMakeLists.txt d46f9a87b38db215387a13984f169af0933afcec34622591d65bccfb3decbc7a
anchor-sha256: xisa/core/registry.json ee2b159b334319266244a237e4f75464f968d5cc705a02060bdb24b7ef4e58b1
anchor-sha256: contracts/canonical-program/architecture-identity.toml 844f5e20d293d2b74efda9d1755c7da2d9da27b9acc985b346dcdc54e1917ccd
anchor-sha256: contracts/canonical-program/operation-capability-matrix.json 60e1fb008e434c57727d94c706fa3d96a67ef59cd78157e5b18cbf30b4f76932
anchor-sha256: contracts/canonical-program/xrprogram-aot-coverage.json 1707c0be4d39d1436ab18363688b8e0de4e585dc7955531f7bdf6090424666db
anchor-sha256: src/aot/program/xr_backend_ir.h d35248d0b4b0e46429216c5b8181474065164e30952f40de314e934ff8450208
anchor-sha256: src/aot/program/xr_backend_ir_internal.h 8a9ce58abae4ad01097ce0d948585c62d5e7a81b1b9ef1f55e4f3db47c989835
anchor-sha256: src/aot/program/xr_backend_ir.c 401b8f59cb43b2e0c984dc413f6d2de4ee4d374e4a9b4c94b612b4a854558ed6
anchor-sha256: src/aot/program/xr_backend_ir_verify.c 1bb61d2c13cdfb0125982658530841e97ea8c9a15621bc6563955770d3cca79f
anchor-sha256: src/aot/program/xr_backend_ir_emit_c.c f9dd735e968773d63605bad5f84c37c53bd8abc9fdcd153ca15b0455a972ba4f
anchor-sha256: src/aot/program/xr_native_artifact.c fc273aac15c76b9ffcd6300e7c77978a4f729bbbfcbf06c8018df85ba72d4f76
anchor-sha256: scripts/check_xr_program_aot_contracts.py 61657cd5870e1b965a20800fbaff4bbec7f4b3695bfad1830df0b2a8f7d06285
anchor-sha256: scripts/check_xr_program_aot_native.py a90027218e16c75351ca8de8dbb68f188d20255ba4ad23324a08416af0eda56d
anchor-sha256: scripts/check_xr_program_aot_providers.py 51b1bdc7b16aa8709ba082ad185dcd0ca59af2f3bc10428d8d4ef480c80acbc0
anchor-sha256: tests/unit/aot/test_xr_program_aot.c 9437db64dcedd352b87468a78efdba114f35a0bd3eeb8e078aec0f6d4d13be44
