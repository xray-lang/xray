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
AOT-toolchain symbol and executes without program bytes. All forty-four current CoreSpec operations
have private BackendIR/C lowering; Wave 2 aggregate and variant rows become portable C11 structs and
tagged unions without a VM carrier or shared local layout. The operation matrix separately records
which rows have a production source owner. Checked overflow, wrapping overflow, and division by zero
also compile and execute as independent native cases. Full language operation families, high-risk
optimizations, public loader ABI, and package publication remain inactive.

anchor-sha256: CMakeLists.txt 100df49512a0a67e30f3e2025ee89b40115afe70cfa2031fffb856ee68931648
anchor-sha256: tests/unit/CMakeLists.txt c9e8d9d45a4ad7bee40b1e66dd12fca0ceaa087cd4106b54a5410cc6e00852c7
anchor-sha256: xisa/core/registry.json 86a6f14d8971d7217de112cef7147c78b9b2328f7e778530ab9317cd8525555c
anchor-sha256: contracts/canonical-program/architecture-identity.toml 844f5e20d293d2b74efda9d1755c7da2d9da27b9acc985b346dcdc54e1917ccd
anchor-sha256: contracts/canonical-program/operation-capability-matrix.json 75822087e806a6b30578316a151f9c1b6f598c113763146924fa702be4dc6cca
anchor-sha256: contracts/canonical-program/xrprogram-aot-coverage.json bc57cfc8c759990bd3eebf2f137b833e71e9988651b1185837c0cfe3c29036d4
anchor-sha256: src/aot/program/xr_backend_ir.h d0cafeda3702e5a406b09573adab96223942321f9dab06f4ef0ac32ed6a30349
anchor-sha256: src/aot/program/xr_backend_ir_internal.h 63f79292db4bf8586ec45e9436a53b26194afb6683adffaea070960b3817225d
anchor-sha256: src/aot/program/xr_backend_ir.c 598b4a0ff7c33f704d32c8cf408573c12f735891099a153eca79caba2bf83810
anchor-sha256: src/aot/program/xr_backend_ir_verify.c d1a907fc82739ba4cd081810e25662d3e6171185000c9ec5a61e4e35073b443d
anchor-sha256: src/aot/program/xr_backend_ir_emit_c.c 02e8947f746a950d7eae9c737bdf06d5c6f3ece2aafb2804f5615bd252983642
anchor-sha256: src/aot/program/xr_native_artifact.c fc273aac15c76b9ffcd6300e7c77978a4f729bbbfcbf06c8018df85ba72d4f76
anchor-sha256: scripts/check_xr_program_aot_contracts.py 61657cd5870e1b965a20800fbaff4bbec7f4b3695bfad1830df0b2a8f7d06285
anchor-sha256: scripts/check_xr_program_aot_native.py a90027218e16c75351ca8de8dbb68f188d20255ba4ad23324a08416af0eda56d
anchor-sha256: scripts/check_xr_program_aot_providers.py 51b1bdc7b16aa8709ba082ad185dcd0ca59af2f3bc10428d8d4ef480c80acbc0
anchor-sha256: tests/unit/aot/test_xr_program_aot.c 030fd0ad1150014f465c71951b2a8d0eb44aae40ef2eec59db8a37427973a536
