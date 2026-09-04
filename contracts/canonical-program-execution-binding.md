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

anchor-sha256: src/plan/target/xr_target_profile.h 2b3843da897b8c1ff4d5954354619d62dc9cab820bd87e2c137dc6b3823a29cf
anchor-sha256: src/plan/target/xr_target_profile.c 88bef72104a8394c107a90a8373a3c78057a161eb4b253d6addacf0a061c2468
anchor-sha256: src/plan/target/xr_target_profile_verify.c 4092adc2ff88ab03eccf6e9795b4c32efd7173a8acf96c94dba7932cc5e34ab7
anchor-sha256: src/plan/target/xr_target_verify.c a9ccb2c7e2fe3010a36c73a0ea76a1310f3fa216f25d8c5087ec7b8f296fad42
anchor-sha256: src/execution/xr_execution.h 03bf038c0a855f2244e98dff0ac126f982a4c76662501c7cb3cc5a03c0c75148
anchor-sha256: src/execution/xr_execution.c 63bf9ed5c42f5ec6522eadd65c388ffca0783b78b3cadc0c61aa5e3d5e2b7945
anchor-sha256: src/execution/xr_boundary_materialization.h 337225749c98d6b0ae0ddce921c1e27b71765dc023ddfef2deb273fef45c8482
anchor-sha256: src/execution/xr_boundary_materialization.c cc88382d31ff8fd0ef5c7ecb32b9184f63c7fab0964c358c5547c695173ff06a
anchor-sha256: src/program/xr_program_verify.h 74916d1083b6a680e29ce698eb89321270f21c679ef56e7f6d73c542e5dfad61
anchor-sha256: src/program/xr_program_verify.c 015a93ba589808deef762389cd4421a10a7cfa2549a2fa5db6c41451343cbe1d
anchor-sha256: src/program/xr_validated_program_internal.h 30fe60a345331ba8a3f6a372be9a31813405bb9f9da494b78178d221011c8d15
anchor-sha256: src/runtime/abi/xr_runtime_contract.h b786851747d2808668f714e668a7ff7a2c325d8a704e9adfea342ed2770baf0c
anchor-sha256: src/runtime/abi/xr_runtime_contract.c 85f91cb590821995e321fec074ac7a820b91ffbc0d9009d03996075b78daee26
anchor-sha256: src/runtime/class/xinstance.h 42cfcf363dcc88dfe35d1d2fa0ccf7e581e4982bb05a11c12db2b2d20ca71462
anchor-sha256: tests/unit/plan/test_target_profile.c ebcd1c0fef635f5e4997fd41523f47349b5c6ccaf868eaa491789993abaab8ac
anchor-sha256: tests/unit/execution/test_xr_execution.c e58485d6ed90a24d77ce47fefe414346dca6a5f25d33e69777f87ccda95632b4
anchor-sha256: tests/unit/execution/test_xr_boundary_materialization.c 1c19fb2ae0306cfd84de656ef868289433848497df35a7af04047ff6ffad2748
anchor-sha256: tests/unit/runtime/test_runtime_abi_contract.c 950a30305d929637d52c8b5c10672ed1f03fc33c8c22b1c2c6e5c703fe96f72f
anchor-sha256: scripts/check_xr_execution_contracts.py b191275389d71b7fc148c1354e47287b74b47f7a9f58b1f86062b6432be07683
anchor-sha256: contracts/canonical-program/execution-binding-coverage.json 37deb555a259fd7ff8aea6b6d6a8f3cff1819206be62ad41be8a5da2024a4c53
