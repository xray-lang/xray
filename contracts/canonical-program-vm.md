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

The executor covers all forty-six current CoreSpec operations. Provider-backed output uses a
dedicated lease-pinned byte-sink trampoline and is checked byte-for-byte against the reference
evaluator under both VM decode policies. Wave 1 source activates scalar,
control, block arguments, and sealed calls; Wave 2 adds logical aggregate construction/projection
and variant construction/test/projection. Aggregate and variant values use VM-private typed arena
cells and never expose a shared physical layout. Aggregate update and the walking-skeleton
trap/error/profile rows remain source-gated by the operation matrix even though their typed handlers
exist. Native LP64 and explicit foreign ILP32 profiles, resource limits, and generation invalidation
remain covered. Full-language operation families, public embedding ABI, adaptive quickening, and any
persistent private-code cache remain inactive for later tasks.

anchor-sha256: CMakeLists.txt 98c0a12f8f1d3c2cce6bb8e91c631c50f8dd0139b7f690e76fdacfad0b02fc2f
anchor-sha256: xisa/core/registry.json 59afc2fc87be973eb1bea089503b8a5998be2faa9a893d3fbe51ebe97c118bab
anchor-sha256: src/vm/xr_program_vm.h 6fb1fe3ecd7b24ab92dbed9acda97a0855d7582d027eea7720b94a300d7a6f60
anchor-sha256: src/vm/xr_program_vm.c 48956c4f6d4c1c9fe69e3f81c6085dc4c549f6083794565a53a5e8cf381efccc
anchor-sha256: src/program/xr_program_verify.h 05a87dca25a389c21133915c9684fc2560f21d02c888e6117a6da3337e4f6d9e
anchor-sha256: src/program/xr_program_verify.c 93c2ba321b41de6c941abed771161a9224ba426a1daa715657b897a70a36b6da
anchor-sha256: src/execution/xr_execution.h a22e903123436fa80fbd3baa1976230206631f1239dd7d4191bbb9dc0b12d780
anchor-sha256: src/execution/xr_execution.c e681cfb31faffd6870abbd87086368c85b5e12e85c477bef37031191c6760e4a
anchor-sha256: src/execution/xr_execution_identity.h 5783c870cd0d642c6d60983e24efcd183edbfbb63380ffae3254e5617af5fd51
anchor-sha256: src/execution/xr_execution_identity.c 857dc89de900a4eda6e71c9498aac3657779a93e68d9593cd4fefb374b84f0bf
anchor-sha256: scripts/check_xr_program_vm_contracts.py c38952179d9d09b0e9a9c390b981e9d78b0801da4b550a5fc6b5be90893a7c72
anchor-sha256: contracts/canonical-program/xrprogram-vm-coverage.json b327a66aff0c9ccd2d9d8e121c5d774b3fc8c4f4bda7a85af182521a1b6352b2
anchor-sha256: tests/unit/vm/test_xr_program_vm.c ad02fc014cd831dc2e92cdac7f555fd0c6a9084b1380357929c0c28733e36fe5
anchor-sha256: tests/unit/vm/test_xr_program_vm_runtime.c 75e731e0d36264735ad2f9625206b2ec323e14de9101f91d32827fdffbcce570
