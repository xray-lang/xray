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

BoundaryABI and runtime-kernel walking-skeleton rows currently cover the active scalar CoreSpec,
object/string identity, RC/weak/panic/OOM policies, and generation protocol. The typed VM consumes
those scalar rows. Aggregate ownership, cleanup/root, coroutine, AOT, FFI, and hybrid adapter rows
remain explicitly inactive until their operation families are activated. No VM slot, native
register, or common local physical plan is stored here.

anchor-sha256: src/plan/target/xr_target_profile.h ee595a78a40c2aafde42998009a077391c90f8a76ff617cbed0576b3066594e3
anchor-sha256: src/plan/target/xr_target_profile.c ed1e163d875568280438d48caf01bc18415f68a1ebeda4f3e0f21b0ebb9cbec6
anchor-sha256: src/plan/target/xr_target_profile_verify.c 4092adc2ff88ab03eccf6e9795b4c32efd7173a8acf96c94dba7932cc5e34ab7
anchor-sha256: src/plan/target/xr_target_verify.c 3fc3839e6332ab12e08cc2200d722138970be76b09c52dfce7422f05dba0e358
anchor-sha256: src/execution/xr_execution.h d9d777969c65f282ff70a7cc755f26d11b2ebf0a044e05d059fde0049c88d357
anchor-sha256: src/execution/xr_execution.c abb49afef5a5a9233c0e471ad9d0bc71a22cc65726938a6ce613e337f43d06c9
anchor-sha256: src/program/xr_program_verify.h 4deae77110a835e6bbdd4c83e76e875a706b68098bc78f87fd2f3f27d7bd97ea
anchor-sha256: src/program/xr_program_verify.c 1c983267259338aa537927abdcd169ad52b2b696676f648a21879930a41b5d23
anchor-sha256: src/program/xr_validated_program_internal.h ecffb3c6a094991a4fdf6a80004e92c230458a2300922a96b51277a098311d65
anchor-sha256: src/runtime/abi/xr_runtime_contract.h b786851747d2808668f714e668a7ff7a2c325d8a704e9adfea342ed2770baf0c
anchor-sha256: src/runtime/abi/xr_runtime_contract.c abcdaf535396af094227cdaf2348d88760396d61ecdf8d2fa9f776d61b7edee7
anchor-sha256: src/runtime/class/xinstance.h 42cfcf363dcc88dfe35d1d2fa0ccf7e581e4982bb05a11c12db2b2d20ca71462
anchor-sha256: tests/unit/plan/test_target_profile.c ebcd1c0fef635f5e4997fd41523f47349b5c6ccaf868eaa491789993abaab8ac
anchor-sha256: tests/unit/execution/test_xr_execution.c a2bd33b15d3c09fb82e0a9ed8b3763c2b2aee28db2466fbe9dd7f5cd4f3220ab
anchor-sha256: tests/unit/runtime/test_runtime_abi_contract.c 6252f7fc4712596c39a4dbc42b4633dbb144f6b2638f67040ff7642cd8f7ee61
anchor-sha256: scripts/check_xr_execution_contracts.py 5646a4a05348790110427baa1374597459d1a61c2a05d4db1cc2f8e3dc911318
anchor-sha256: contracts/canonical-program/execution-binding-coverage.json 185050ac3d5ecc19226619761a1e1c97940df85d85b8ac12f140b59dc702afc2
