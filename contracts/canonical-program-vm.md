# Canonical XrProgram VM contract

The VM consumes only `XrValidatedProgram` plus one active `XrInstance`. It does not decode program
bytes, call the reference evaluator, reconstruct source types or effects, or consult TargetPlan,
legacy Proto bytecode, or AOT. Every active CoreSpec operation has one explicit typed handler.

`XrProgram` remains the only distributed executable program format. `XrVmCode` is private runtime
state and has no serializer, reader, compatibility promise, or public install header. The baseline
view walks immutable validated rows. The fixed-row view copies only dispatch-shaped instruction
rows and retains references to validated operands and successors. Both views produce the same
logical operation trace. Adaptive quickening remains disabled until a measured policy and its
traceability proof exist.

VM code is qualified by `ExecutionId`, generation, VM build identity, decode policy, and quickening
policy. Execution pins the active generation and refuses retired or successor instances. A private
view can always be discarded and rebuilt from `XrValidatedProgram`; corruption or mismatch cannot
fall back to an older executor.

The `xray_program_vm_runtime` archive is the embeddable product boundary for this stage. Its exact
source closure contains the CoreSpec projection, XrProgram decoder/verifier, immutable target
profile, BoundaryABI/runtime contracts, execution instance, and typed VM. It excludes frontend,
CoreIR writer, reference evaluator, TargetPlan, legacy Proto VM, and AOT. The runtime-only test
links a build-produced XrProgram byte array against this archive and verifies the resulting symbol
closure.

The executor covers all forty-four current CoreSpec operations. Wave 1 source activates scalar,
control, block arguments, and sealed calls; Wave 2 adds logical aggregate construction/projection
and variant construction/test/projection. Aggregate and variant values use VM-private typed arena
cells and never expose a shared physical layout. Aggregate update and the walking-skeleton
trap/error/profile rows remain source-gated by the operation matrix even though their typed handlers
exist. Native LP64 and explicit foreign ILP32 profiles, resource limits, and generation invalidation
remain covered. Full-language operation families, public embedding ABI, adaptive quickening, and any
persistent private-code cache remain inactive for later tasks.

anchor-sha256: CMakeLists.txt 55e5ce6bd4d0ba52f0f6a361d934796dc7e4d40f9e079e0e220a7e5ce4aeaf65
anchor-sha256: xisa/core/registry.json 86a6f14d8971d7217de112cef7147c78b9b2328f7e778530ab9317cd8525555c
anchor-sha256: src/vm/xr_program_vm.h f378acaffbccb1fd354fe97ee5a5e56ba1e9cd31172f301dcedfece2254257eb
anchor-sha256: src/vm/xr_program_vm.c 40d4714032983ede92dde1c24f218ffa06cde433d60f8f032437f41e73ab688a
anchor-sha256: src/program/xr_program_verify.h 74916d1083b6a680e29ce698eb89321270f21c679ef56e7f6d73c542e5dfad61
anchor-sha256: src/program/xr_program_verify.c 0f75d31063f6900c42ef205280301642a56d3a99738bcb40b1f3e86b2fa2d875
anchor-sha256: src/execution/xr_execution.h 104ca36e8a0e459c6eb03eba4fbd864657b100acc54da83ba9614a0e15ee00aa
anchor-sha256: src/execution/xr_execution.c 864117dd29e54cb589890b0a0e353320bf6f9fbe2705e01eef1d6b50690c7c4f
anchor-sha256: scripts/check_xr_program_vm_contracts.py 2fdd58bf97eaf786abe2fab5bc26663bd197128426e146d6f430511d5daecc0c
anchor-sha256: contracts/canonical-program/xrprogram-vm-coverage.json 6012cd4ff8aad15dc2f4571bfa893934d6cb62b11c02c743bc76ab83caea3582
anchor-sha256: tests/unit/vm/test_xr_program_vm.c 95bd45aecb32cda1364ff5b621243e7d3deef30035080854585e453758f70a5c
anchor-sha256: tests/unit/vm/test_xr_program_vm_runtime.c 75e731e0d36264735ad2f9625206b2ec323e14de9101f91d32827fdffbcce570
