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

The executor covers all thirty-eight current CoreSpec operations. Wave 1 source activates scalar,
control, block arguments, and sealed calls; Wave 2 adds logical aggregate construction/projection
and variant construction/test/projection. Aggregate and variant values use VM-private typed arena
cells and never expose a shared physical layout. Aggregate update and the walking-skeleton
trap/error/profile rows remain source-gated by the operation matrix even though their typed handlers
exist. Native LP64 and explicit foreign ILP32 profiles, resource limits, and generation invalidation
remain covered. Full-language operation families, public embedding ABI, adaptive quickening, and any
persistent private-code cache remain inactive for later tasks.

anchor-sha256: CMakeLists.txt 830633d6559aded38a0233a76f93d73abe770412e7e77cc283e594e260b4661c
anchor-sha256: xisa/core/registry.json 5a3008c03058fe1d8c426848d1cc077d4a17156abf34c1393d654e10acb0a1e4
anchor-sha256: src/vm/xr_program_vm.h ae78422822acb1fac0e95718d3d562ea3f78fe93fcfbc2ee9f7877706aa0c2fc
anchor-sha256: src/vm/xr_program_vm.c f64d662c4cdcb347f1c7bedd5991b75af360468680c92f04b42ae50872aa0651
anchor-sha256: src/program/xr_program_verify.h 74916d1083b6a680e29ce698eb89321270f21c679ef56e7f6d73c542e5dfad61
anchor-sha256: src/program/xr_program_verify.c 7b44b9d3bdc0f3f9120363e7718e8e31dfc0a63b9b873e6e598ad3e6d8ba41c3
anchor-sha256: src/execution/xr_execution.h 104ca36e8a0e459c6eb03eba4fbd864657b100acc54da83ba9614a0e15ee00aa
anchor-sha256: src/execution/xr_execution.c 864117dd29e54cb589890b0a0e353320bf6f9fbe2705e01eef1d6b50690c7c4f
anchor-sha256: scripts/check_xr_program_vm_contracts.py 2fdd58bf97eaf786abe2fab5bc26663bd197128426e146d6f430511d5daecc0c
anchor-sha256: contracts/canonical-program/xrprogram-vm-coverage.json a00af2793ebc3f08a73f44ef9cb51e1eedb86b9356fea3131cdf1ddbd30cdbdd
anchor-sha256: tests/unit/vm/test_xr_program_vm.c 580499883c0ac1ad81f60214ad959048161e10caeffabb4fd6bb7ecd64836669
anchor-sha256: tests/unit/vm/test_xr_program_vm_runtime.c 75e731e0d36264735ad2f9625206b2ec323e14de9101f91d32827fdffbcce570
