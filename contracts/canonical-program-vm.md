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

anchor-sha256: CMakeLists.txt 830633d6559aded38a0233a76f93d73abe770412e7e77cc283e594e260b4661c
anchor-sha256: xisa/core/registry.json 8b1f4a8cf46d316c4818809466283fb8b1e633e5e022b7b1d66ad120b9487300
anchor-sha256: src/vm/xr_program_vm.h edf12e3eec2eab5115eb76625e2b53ff18fc4a191aef37986e2f141fcb05838b
anchor-sha256: src/vm/xr_program_vm.c 44997b940572cc58693e31b9622de63bcf4b20f377de566742a07cb4252a74da
anchor-sha256: src/program/xr_program_verify.h 74916d1083b6a680e29ce698eb89321270f21c679ef56e7f6d73c542e5dfad61
anchor-sha256: src/program/xr_program_verify.c 76abf74e7dc0522511dccfad81d46c1318972d05a3fb4eb9e9af75f893b4c9a3
anchor-sha256: src/execution/xr_execution.h 03bf038c0a855f2244e98dff0ac126f982a4c76662501c7cb3cc5a03c0c75148
anchor-sha256: src/execution/xr_execution.c 63bf9ed5c42f5ec6522eadd65c388ffca0783b78b3cadc0c61aa5e3d5e2b7945
anchor-sha256: scripts/check_xr_program_vm_contracts.py 2fdd58bf97eaf786abe2fab5bc26663bd197128426e146d6f430511d5daecc0c
anchor-sha256: contracts/canonical-program/xrprogram-vm-coverage.json 9eb578cfc826c18f131a076208c9340321a3867196058fd0b5353454bd3c3e47
anchor-sha256: tests/unit/vm/test_xr_program_vm.c 68c17c0cf3a251cb755b6da255e447d43249ad7459130a5a2aab8f709fdb1691
anchor-sha256: tests/unit/vm/test_xr_program_vm_runtime.c 75e731e0d36264735ad2f9625206b2ec323e14de9101f91d32827fdffbcce570
