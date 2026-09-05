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

The executor covers all twenty-one current CoreSpec operations. Wave 1 source activates scalar,
control, block arguments, and sealed calls; Wave 2 adds logical aggregate construction/projection
and variant construction/test/projection. Aggregate and variant values use VM-private typed arena
cells and never expose a shared physical layout. Aggregate update and the walking-skeleton
trap/error/profile rows remain source-gated by the operation matrix even though their typed handlers
exist. Native LP64 and explicit foreign ILP32 profiles, resource limits, and generation invalidation
remain covered. Full-language operation families, public embedding ABI, adaptive quickening, and any
persistent private-code cache remain inactive for later tasks.

anchor-sha256: CMakeLists.txt 8e2fe64fc2ec8cbb81dc48deab1ae0f38a7820f7fe610c463eb53d0a2e094d1c
anchor-sha256: xisa/core/registry.json c68d7bb1c78179795e889e879abcef82388fd153b5cb1ea26a52afe407ce5536
anchor-sha256: src/vm/xr_program_vm.h aa3033ed1744a26adc1be967c6ef792bd9ba39ec538aa8c90cf4deb0ed35c268
anchor-sha256: src/vm/xr_program_vm.c 8b4cec984053740342fddceb68aa820fbd831b5b25106c54bac880c3a6ebf79e
anchor-sha256: src/program/xr_program_verify.h 05a87dca25a389c21133915c9684fc2560f21d02c888e6117a6da3337e4f6d9e
anchor-sha256: src/program/xr_program_verify.c 99c266ff394d61445be9037b0e40d317fa791e6f060a119b07d882c57b6c3e53
anchor-sha256: src/execution/xr_execution.h 03bf038c0a855f2244e98dff0ac126f982a4c76662501c7cb3cc5a03c0c75148
anchor-sha256: src/execution/xr_execution.c 63bf9ed5c42f5ec6522eadd65c388ffca0783b78b3cadc0c61aa5e3d5e2b7945
anchor-sha256: scripts/check_xr_program_vm_contracts.py 2fdd58bf97eaf786abe2fab5bc26663bd197128426e146d6f430511d5daecc0c
anchor-sha256: contracts/canonical-program/xrprogram-vm-coverage.json be1fea21691897efc52871b3984e68a3f7d86366dc47151ca34236d53615760d
anchor-sha256: tests/unit/vm/test_xr_program_vm.c 152500886977e9410772709a5540d32b01611b31c24f48727da25b9464949a99
anchor-sha256: tests/unit/vm/test_xr_program_vm_runtime.c 75e731e0d36264735ad2f9625206b2ec323e14de9101f91d32827fdffbcce570
