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

The executor covers all forty-seven current CoreSpec operations. Indirect calls borrow an affine
callable without consuming it, whether the current SSA value owns the callable or is a non-owner
`READ` function parameter; the callable SignatureId remains the only visible dispatch contract.
Provider-backed output uses a
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
anchor-sha256: xisa/core/registry.json c5c2b2be5583b82ab439a631fe71ac47f346b92a62d5c169d587b9effaf3758b
anchor-sha256: src/vm/xr_program_vm.h 6fb1fe3ecd7b24ab92dbed9acda97a0855d7582d027eea7720b94a300d7a6f60
anchor-sha256: src/vm/xr_program_vm.c 2a6d1f99dfa30fc2497ec9f538312dd08b4099bfb720c1be2d73129acf76c1ec
anchor-sha256: src/program/xr_program_verify.h 05a87dca25a389c21133915c9684fc2560f21d02c888e6117a6da3337e4f6d9e
anchor-sha256: src/program/xr_program_verify.c 2cd2942c3901bb276bee29b0293c66a8ec3a2caeafbb24d74bfac4c01048d379
anchor-sha256: src/execution/xr_execution.h 45c05cfd6e222e1eda22ce10a719d3ba5703a4da27d6861dd0c99c51b29fe4b3
anchor-sha256: src/execution/xr_execution.c ee0cc7951f61f7e45c89476bd0c760b1e028cc116afe093b5a7f69a5a67e3b65
anchor-sha256: src/execution/xr_execution_identity.h 5783c870cd0d642c6d60983e24efcd183edbfbb63380ffae3254e5617af5fd51
anchor-sha256: src/execution/xr_execution_identity.c 857dc89de900a4eda6e71c9498aac3657779a93e68d9593cd4fefb374b84f0bf
anchor-sha256: scripts/check_xr_program_vm_contracts.py c38952179d9d09b0e9a9c390b981e9d78b0801da4b550a5fc6b5be90893a7c72
anchor-sha256: contracts/canonical-program/xrprogram-vm-coverage.json 8eccd6e64e13488f8f0a04c2f1d4b06bc5657902d9cef2e16c9e6c2451e4672f
anchor-sha256: tests/unit/vm/test_xr_program_vm.c aae5c8f9cfcc544b439a80c403ca029d7bf64cd00fe8962a005aaa33f3176929
anchor-sha256: tests/unit/vm/test_xr_program_vm_runtime.c 75e731e0d36264735ad2f9625206b2ec323e14de9101f91d32827fdffbcce570
