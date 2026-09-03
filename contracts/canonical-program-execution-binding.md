# Canonical program execution binding

`XrTargetProfile` is the sole immutable owner of four independently identified partitions:
target-observable semantics, public BoundaryABI, runtime-kernel policy, and the exact provider
contract set. It is derived only from explicit target facts. Native host probing belongs in a
profile constructor outside this contract; foreign profiles cannot consult compiler-host width,
endianness, preprocessor OS macros, or provider defaults.

`XrValidatedProgram` is retained by `XrInstance`; an artifact or structurally decoded program is
not admissible. `ExecutionId` hashes ProgramId, TargetProfileId, BoundaryAbiId, and RuntimeKernelId.
Generation is deliberately excluded from that semantic identity and forms the second field of
`XrExecutionCacheKey`, so provider rebinding invalidates live caches without pretending that the
program/profile meaning changed.

Provider admission compares stable contract identity, exact provider-contract fingerprint,
ordered operation identities, non-null operation entries, and required thread/reentrancy/callback
behavior before the instance becomes ACTIVE. In particular, a provider compiled for a different
target call ABI is rejected even when it implements the same stable operation names.

The generation protocol is ACTIVE -> DRAINING -> RETIRED. DRAINING rejects new pins; RETIRED
requires zero pins; a successor is created only from a retired instance at generation + 1. The
execution authority owns retained program/profile references and copied provider bindings. The
pre-existing language object allocation type is named `XrObjectInstance`; `XrInstance` has one
meaning only.

BoundaryABI is active for copy-value scalar, aggregate, and variant boundaries. From one exact
`XrInstance`, the materializer deterministically derives boundary-kind-specific type layouts and
function call frames. Aggregate fields use declaration order with natural target alignment;
variants use a `u32` tag and naturally aligned union payload. Every materialized type identity binds
ProgramId, BoundaryAbiId, BoundaryKind, TypeId, and the complete derived layout. Every call identity
binds ExecutionId, BoundaryKind, FunctionId, and all argument/result slots. This contract is a
public-boundary description, not an executor-local representation.

The currently admitted types are copy values, so root tables are empty and cleanup is explicitly
trivial. Nontrivial ownership transfer, root/cleanup rows, coroutine state, and concrete AOT, FFI,
hybrid, or reloadable adapters remain inactive. Runtime-kernel policy remains a walking skeleton for
the active object/string identity, RC/weak/panic/OOM policies, and generation protocol. No executor
slot, native register, or common local physical plan is stored here.

anchor-sha256: src/plan/target/xr_target_profile.h cc34ac187a3bc33cbf326e81d233a195227adfceeee6f80b8473175c5947a067
anchor-sha256: src/plan/target/xr_target_profile.c 92c9a3f4a96329a25aa662a763e11bc5eb0fa0083d0bf0e69caac23442e82ea2
anchor-sha256: src/plan/target/xr_target_profile_verify.c 4092adc2ff88ab03eccf6e9795b4c32efd7173a8acf96c94dba7932cc5e34ab7
anchor-sha256: src/plan/target/xr_target_verify.c 60cf29ff49511e8a0e00aff7f43d8b227d8eb6e2da1db7c205bbea8f3abc9431
anchor-sha256: src/execution/xr_execution.h d9d777969c65f282ff70a7cc755f26d11b2ebf0a044e05d059fde0049c88d357
anchor-sha256: src/execution/xr_execution.c abb49afef5a5a9233c0e471ad9d0bc71a22cc65726938a6ce613e337f43d06c9
anchor-sha256: src/execution/xr_boundary_materialization.h bc2e27bdeaa0243a9fcab271c1b9f7735088bee46a751eade590b94bb846f45e
anchor-sha256: src/execution/xr_boundary_materialization.c e43762a796276f490220a5d4700b370587bc5f6b650987d210c6cd6060d50fb8
anchor-sha256: src/program/xr_program_verify.h 4deae77110a835e6bbdd4c83e76e875a706b68098bc78f87fd2f3f27d7bd97ea
anchor-sha256: src/program/xr_program_verify.c b3a92d85a983e6221be6595c5579003205d2bd4c7a26580ae05206bd5a2da4c4
anchor-sha256: src/program/xr_validated_program_internal.h 91bcf2a18f6a11e81866346277795e9ce739491c93815587133ce73c33ad47a6
anchor-sha256: src/runtime/abi/xr_runtime_contract.h b786851747d2808668f714e668a7ff7a2c325d8a704e9adfea342ed2770baf0c
anchor-sha256: src/runtime/abi/xr_runtime_contract.c abcdaf535396af094227cdaf2348d88760396d61ecdf8d2fa9f776d61b7edee7
anchor-sha256: src/runtime/class/xinstance.h 42cfcf363dcc88dfe35d1d2fa0ccf7e581e4982bb05a11c12db2b2d20ca71462
anchor-sha256: tests/unit/plan/test_target_profile.c ebcd1c0fef635f5e4997fd41523f47349b5c6ccaf868eaa491789993abaab8ac
anchor-sha256: tests/unit/execution/test_xr_execution.c 80d16e6d185dd0e1ca73f14005876e4301ad71010d1d382158d6ee2f5ec9df8a
anchor-sha256: tests/unit/execution/test_xr_boundary_materialization.c c2b6a99a7a38815d49953b3292b1cc1597aba54d88f6d8830f4a745605f9a2f2
anchor-sha256: tests/unit/runtime/test_runtime_abi_contract.c 6252f7fc4712596c39a4dbc42b4633dbb144f6b2638f67040ff7642cd8f7ee61
anchor-sha256: scripts/check_xr_execution_contracts.py 9dd3608922e8ae89fe4f655769eaf33aad29538ed8320c651a331a4f0de285aa
anchor-sha256: contracts/canonical-program/execution-binding-coverage.json 05e7bd91f8a5649e8fe46a246b663445c6211b480a569139bde3116be973753c
