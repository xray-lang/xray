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

anchor-sha256: CMakeLists.txt 888ea2e2045ce386d4d828e8da360294ee72d3b73a7f2273a6d79ea7e3b02475
anchor-sha256: xisa/core/registry.json ee2b159b334319266244a237e4f75464f968d5cc705a02060bdb24b7ef4e58b1
anchor-sha256: src/vm/xr_program_vm.h 17d36a0fd008d8efc9db4d51190a73a9093d14f21783f7d0b01be107ccd6a5a4
anchor-sha256: src/vm/xr_program_vm.c 877f12a3de1521d2f9429630ef3b392f93b12bee66861ddf3e9f48643c007a98
anchor-sha256: src/program/xr_program_verify.h 05a87dca25a389c21133915c9684fc2560f21d02c888e6117a6da3337e4f6d9e
anchor-sha256: src/program/xr_program_verify.c d65f66069dd2c150a75fe5faf2d5434297e7326d5af56faa3e62e5f99e65a31d
anchor-sha256: src/execution/xr_execution.h 03bf038c0a855f2244e98dff0ac126f982a4c76662501c7cb3cc5a03c0c75148
anchor-sha256: src/execution/xr_execution.c 63bf9ed5c42f5ec6522eadd65c388ffca0783b78b3cadc0c61aa5e3d5e2b7945
anchor-sha256: scripts/check_xr_program_vm_contracts.py c48fe6e33f1488b904932c249ebb9c3bbfecf325543de2e83408fcff65fa4fc6
anchor-sha256: contracts/canonical-program/xrprogram-vm-coverage.json 433a3478a13ddab044a1037d57604d7aa2174d7e27b3a2a6cefa7833dda69004
anchor-sha256: tests/unit/vm/test_xr_program_vm.c 947735fbc12833d85b0878803c30740e0f7a3b8b763923490eeb32dbef87d500
anchor-sha256: tests/unit/vm/test_xr_program_vm_runtime.c 75e731e0d36264735ad2f9625206b2ec323e14de9101f91d32827fdffbcce570
