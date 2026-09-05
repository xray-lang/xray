# Canonical program execution binding

`XrTargetProfile` is the sole immutable owner of four independently identified partitions:
target-observable semantics, public BoundaryABI, runtime-kernel policy, and the exact provider
contract set. It is derived only from explicit target facts. Native host probing belongs in a
profile constructor outside this contract; foreign profiles cannot consult compiler-host width,
endianness, preprocessor OS macros, or provider defaults.

`XrValidatedProgram` is the admitted program authority; an artifact or structurally decoded program
is not admissible. The pure `xr_execution_id_compute` owner hashes ProgramId, TargetProfileId,
BoundaryAbiId, and RuntimeKernelId without a live runtime instance. VM binding retains the program
and profile in `XrInstance`; AOT compilation consumes them directly. Generation is deliberately
excluded from that semantic identity and forms the second field of `XrExecutionCacheKey`, so
provider rebinding invalidates live caches without pretending that program/profile meaning changed.

Provider admission compares stable contract identity, exact provider-contract fingerprint,
ordered operation identities, non-null operation entries, and required thread/reentrancy/callback
behavior before the instance becomes ACTIVE. In particular, a provider compiled for a different
target call ABI is rejected even when it implements the same stable operation names.

Provider invocation is available only through a typed lease adapter. The adapter resolves the
dense program requirement and operation index under the instance lock, pins the lease ticket for
the duration of the call, and invokes user code without holding that lock. A copied lease cannot
be released while any nested or concurrent provider call is in flight. Program/profile accessors
return retained references so a racing release cannot invalidate a borrowed pointer.

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

anchor-sha256: src/plan/target/xr_target_profile.h c657834d5a0c233707e531c8712521d9781a2ec2fe4598703cfdcdcc30ff5399
anchor-sha256: src/plan/target/xr_target_profile.c 88bef72104a8394c107a90a8373a3c78057a161eb4b253d6addacf0a061c2468
anchor-sha256: src/plan/target/xr_target_profile_verify.c 4092adc2ff88ab03eccf6e9795b4c32efd7173a8acf96c94dba7932cc5e34ab7
anchor-sha256: src/plan/target/xr_target_verify.c a7aade83086711c19cdfc03b0130fd186c5a5a71bd549273476260ae21304433
anchor-sha256: src/execution/xr_execution.h 16532eaceedd51aeab25cc48e4cd7eb2ec90a9989ef3cf6828e717c3c5414bfd
anchor-sha256: src/execution/xr_execution.c 8ac885ec709e8f472610515c9347104be2eb31425f90f7d7d0f90208bf071d0a
anchor-sha256: src/execution/xr_execution_identity.h 5783c870cd0d642c6d60983e24efcd183edbfbb63380ffae3254e5617af5fd51
anchor-sha256: src/execution/xr_execution_identity.c 857dc89de900a4eda6e71c9498aac3657779a93e68d9593cd4fefb374b84f0bf
anchor-sha256: src/execution/xr_boundary_materialization.h 337225749c98d6b0ae0ddce921c1e27b71765dc023ddfef2deb273fef45c8482
anchor-sha256: src/execution/xr_boundary_materialization.c d822157b7d686cd315434b08a730d04a61fba6018a3decbed566f45dfee45f22
anchor-sha256: src/program/xr_program_verify.h 74916d1083b6a680e29ce698eb89321270f21c679ef56e7f6d73c542e5dfad61
anchor-sha256: src/program/xr_program_verify.c 0f75d31063f6900c42ef205280301642a56d3a99738bcb40b1f3e86b2fa2d875
anchor-sha256: src/program/xr_validated_program_internal.h f6eedb3790396acb8dc90680344139c8bd021877f81a6e0da2f97619cd9c0e5b
anchor-sha256: src/runtime/abi/xr_runtime_contract.h b786851747d2808668f714e668a7ff7a2c325d8a704e9adfea342ed2770baf0c
anchor-sha256: src/runtime/abi/xr_runtime_contract.c 85f91cb590821995e321fec074ac7a820b91ffbc0d9009d03996075b78daee26
anchor-sha256: src/runtime/class/xinstance.h 5a19d7f36bf25723bf9f9c4cb47f60ed0d1abf3d4a7903f281af8d3132b62a97
anchor-sha256: tests/unit/plan/test_target_profile.c ebcd1c0fef635f5e4997fd41523f47349b5c6ccaf868eaa491789993abaab8ac
anchor-sha256: tests/unit/execution/test_xr_execution.c a5136344e38c34c3a0e297f43f594631d5fc22a0e084ba06bcaaea47949dbb04
anchor-sha256: tests/unit/execution/test_xr_boundary_materialization.c 1c19fb2ae0306cfd84de656ef868289433848497df35a7af04047ff6ffad2748
anchor-sha256: tests/unit/runtime/test_runtime_abi_contract.c 950a30305d929637d52c8b5c10672ed1f03fc33c8c22b1c2c6e5c703fe96f72f
anchor-sha256: scripts/check_xr_execution_contracts.py 82b7b37fe10203ab7868b62e8700f2cbd03dacfb0768afc625abffe585ca27fb
anchor-sha256: contracts/canonical-program/execution-binding-coverage.json 89879f48aa9612cfc2f09b09885826828e43e48707f3a17d700c155b142c67ba
