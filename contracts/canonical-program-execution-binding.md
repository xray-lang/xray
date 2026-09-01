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
object/string identity, RC/weak/panic/OOM policies, and generation protocol. Aggregate ownership,
cleanup/root, coroutine, VM, AOT, FFI, and hybrid adapter rows remain explicitly inactive until
their operation families are activated. No VM slot, native register, or common local physical plan
is stored here.

anchor-sha256: src/plan/target/xr_target_profile.h 178d7470dae47ed63a956bb1e8d9e602b510094918ec418d743b461adc79bd65
anchor-sha256: src/plan/target/xr_target_profile.c 9d7a0d371bdffb62e064a2d85b3375c177778736a37f8cc226d71283f3139d3e
anchor-sha256: src/plan/target/xr_target_verify.c 38be67a015fd6fee9e96161af440d26bd6cd14852bb920b150341c339c892b7f
anchor-sha256: src/execution/xr_execution.h d9d777969c65f282ff70a7cc755f26d11b2ebf0a044e05d059fde0049c88d357
anchor-sha256: src/execution/xr_execution.c abb49afef5a5a9233c0e471ad9d0bc71a22cc65726938a6ce613e337f43d06c9
anchor-sha256: src/program/xr_program_verify.h 40c0bc2fbdd5813df1e89755d25a1cf27efbf4846bd1dc9506f58f0b43708268
anchor-sha256: src/program/xr_program_verify.c 5093bd71bfb7e7a675d5f2366e1be6b39f9fc78cbd0a8d870ff26d59203f2b91
anchor-sha256: src/program/xr_validated_program_internal.h 90e438735ee3d0f80868f1498466ccee1bbf39b45d5bb310ee9058eb44851c5d
anchor-sha256: src/runtime/abi/xr_runtime_contract.h b786851747d2808668f714e668a7ff7a2c325d8a704e9adfea342ed2770baf0c
anchor-sha256: src/runtime/abi/xr_runtime_contract.c abcdaf535396af094227cdaf2348d88760396d61ecdf8d2fa9f776d61b7edee7
anchor-sha256: src/runtime/class/xinstance.h 42cfcf363dcc88dfe35d1d2fa0ccf7e581e4982bb05a11c12db2b2d20ca71462
anchor-sha256: tests/unit/plan/test_target_profile.c ebcd1c0fef635f5e4997fd41523f47349b5c6ccaf868eaa491789993abaab8ac
anchor-sha256: tests/unit/execution/test_xr_execution.c a2bd33b15d3c09fb82e0a9ed8b3763c2b2aee28db2466fbe9dd7f5cd4f3220ab
anchor-sha256: tests/unit/runtime/test_runtime_abi_contract.c 6252f7fc4712596c39a4dbc42b4633dbb144f6b2638f67040ff7642cd8f7ee61
anchor-sha256: scripts/check_xr_execution_contracts.py ff624983a82e5a77621da13888f9e66e3624ff9cab581364f5027b48f03ba63f
anchor-sha256: contracts/canonical-program/execution-binding-coverage.json ec8efe65cb3450e432b978546857759132147ac96d094662820c9946fa3ae2f7
