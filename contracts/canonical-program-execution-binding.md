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

Coroutine execution keeps the generation lease pinned while suspended. Resume and cancellation
are distinct operations on that same execution: cancellation is valid only at a verified
safepoint, follows the Program-owned cancel successor, recursively cancels an active sealed child,
and releases the lease only after canonical cancellation publication. Safepoint metadata is the
canonical unordered live-value set, while each suspension instruction carries the ordered tuple
used to transfer and restore those values. The separate cancel segment is an exact subset needed by
the selected reason-private cleanup graph: every live affine owner appears exactly once, and a
needed non-owner may appear at most once. Missing owners, duplicate values, values outside the live
set, and cleanup graphs that fail to consume an owner are invalid. Executors materialize only the
selected edge, so resume never pre-consumes cancellation resources. Exact caller-local `REF` places
and scoped `READ` existential reborrows are admitted only when the Program safepoint carries their unique
owner exactly once; ambiguous projections, missing owners, and duplicate owner paths remain
rejected. Disposing a frame is not an implicit cancellation operation, and no error, panic, or
trap outcome aliases cancellation.

Callable dispatch is program-owned rather than provider-owned. An indirect call performs a
non-consuming `READ` of its affine callable operand, so an owned pack and a borrowed non-owner
function parameter are both valid; target identity and capture layout remain private to each
executor and never enter the execution binding.

Provider invocation is available only through ABI-matched typed lease adapters. Nullary and unary
i64 calls, unary `bool(i64)` calls, nullary optional-i64-pair calls, and byte-output writes occupy
distinct trampoline kinds; an operation cannot enter through the wrong union member. Each adapter
resolves the dense program requirement and operation index under the instance lock, pins the lease
ticket for the duration of the call, and invokes user code without holding that lock. A copied
lease cannot be released while any nested or concurrent provider call is in flight.
Program/profile accessors return retained references so a racing release cannot invalidate a
borrowed pointer.

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

The public BoundaryABI remains active only for copy-value boundaries, so its root tables are empty
and its cleanup is explicitly trivial. Program-internal affine aggregate/variant values, taking
variant projection, and exact `MOVE` calls are admitted without becoming public boundary carriers.
The active Pipe lifecycle slice binds a consumed signed-i64 resource token to an exact
`bool(i64)` provider operation and relies on the enclosing `MOVE` method call to invalidate the
Pipe owner. It does not grant an implicit destructor, public owned boundary, general root/cleanup
rows, or arbitrary resource-token inference. Coroutine boundary state and concrete AOT, FFI,
hybrid, or reloadable adapters remain inactive. Runtime-kernel policy remains a walking skeleton
for the active object/string identity, RC/weak/panic/OOM policies, and generation protocol. No
executor slot, native register, or common local physical plan is stored here.

anchor-sha256: src/plan/target/xr_target_profile.h 0673f198b623c2b6e3f1895b2c2fdeed19b7fae5ed78ff36e04936b60e4596cb
anchor-sha256: src/plan/target/xr_target_profile.c a132e3f382f6293969649e57445e2543aaf0c133584d5345374680d653f8f9e1
anchor-sha256: src/plan/target/xr_target_profile_verify.c 4092adc2ff88ab03eccf6e9795b4c32efd7173a8acf96c94dba7932cc5e34ab7
anchor-sha256: src/plan/target/xr_target_verify.c a7aade83086711c19cdfc03b0130fd186c5a5a71bd549273476260ae21304433
anchor-sha256: src/execution/xr_execution.h 3a09783038967320ae3566e258de5ae7108b60d7ed5f4f01a4a020067b447867
anchor-sha256: src/execution/xr_execution.c 9edba6e59f290cda924a6a337aba73554f8bd7aa1ac5a0e6dfba958d4b998046
anchor-sha256: src/execution/xr_execution_identity.h 5783c870cd0d642c6d60983e24efcd183edbfbb63380ffae3254e5617af5fd51
anchor-sha256: src/execution/xr_execution_identity.c 857dc89de900a4eda6e71c9498aac3657779a93e68d9593cd4fefb374b84f0bf
anchor-sha256: src/execution/xr_boundary_materialization.h 337225749c98d6b0ae0ddce921c1e27b71765dc023ddfef2deb273fef45c8482
anchor-sha256: src/execution/xr_boundary_materialization.c d822157b7d686cd315434b08a730d04a61fba6018a3decbed566f45dfee45f22
anchor-sha256: src/program/xr_program_verify.h 05a87dca25a389c21133915c9684fc2560f21d02c888e6117a6da3337e4f6d9e
anchor-sha256: src/program/xr_program_verify.c e582bf2d99821a9473a0621914168cccb730f904c93c6180603f2d36f2035eb7
anchor-sha256: src/program/xr_validated_program_internal.h 1cd61fb3b536b3414091d39919ff909e6b12710387438a89303cf1e5c66e78c8
anchor-sha256: src/runtime/abi/xr_runtime_contract.h b786851747d2808668f714e668a7ff7a2c325d8a704e9adfea342ed2770baf0c
anchor-sha256: src/runtime/abi/xr_runtime_contract.c 7a05cee07b815c943fa3745c701b5871649929f13d8daf82479b5cff6f064dfa
anchor-sha256: src/runtime/class/xinstance.h 5a19d7f36bf25723bf9f9c4cb47f60ed0d1abf3d4a7903f281af8d3132b62a97
anchor-sha256: tests/unit/plan/test_target_profile.c ebcd1c0fef635f5e4997fd41523f47349b5c6ccaf868eaa491789993abaab8ac
anchor-sha256: tests/unit/execution/test_xr_execution.c 04ba917bf55ef830923fd8a9c70baca6ed17b14a526bd3935cecc1745455edbc
anchor-sha256: tests/unit/execution/test_xr_boundary_materialization.c 177583c0f785168d4693a33d035ce52c04c6bfd37eeb4de7dacf0843aeffb601
anchor-sha256: tests/unit/runtime/test_runtime_abi_contract.c 1bf549df5f42fabbbe537b578709e032b72b8d0b4848627186945a12b2ee215f
anchor-sha256: scripts/check_xr_execution_contracts.py f478d5f8032c7a17c79a49bb79e87a49687ebf13b6037533fa6a35fe16310e5b
anchor-sha256: contracts/canonical-program/execution-binding-coverage.json efee9456407f35a994f73bed05dd3116790be6f3667d400691cb08648a729227
