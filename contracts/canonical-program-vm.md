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

anchor-sha256: CMakeLists.txt a2c23451b1bda53528dc233ac464c95c5434bb01fc165de4418c89645b7ca310
anchor-sha256: xisa/core/registry.json 5dbedbc83a541e456d67e15ec0ced2282f23aaf843f101d8d7eb33ae6a85c619
anchor-sha256: src/vm/xr_program_vm.h a473285eeaa66ab1e78a5f73e8262a44ce864ac73e35d27318ea07532d0c6905
anchor-sha256: src/vm/xr_program_vm.c d2f80ae4a2442bf3a9248598c95b6ee80493e0fd6644af808307a93d6555d29f
anchor-sha256: src/program/xr_program_verify.h 4deae77110a835e6bbdd4c83e76e875a706b68098bc78f87fd2f3f27d7bd97ea
anchor-sha256: src/program/xr_program_verify.c 0cf0a5f2af2f67f3ecafeab4e63f58632341f738805da83a966c262bf60a88a6
anchor-sha256: src/execution/xr_execution.h d9d777969c65f282ff70a7cc755f26d11b2ebf0a044e05d059fde0049c88d357
anchor-sha256: src/execution/xr_execution.c abb49afef5a5a9233c0e471ad9d0bc71a22cc65726938a6ce613e337f43d06c9
anchor-sha256: scripts/check_xr_program_vm_contracts.py 5fba416acc72582bb02243b56205767512d9b1fe2352f33a90774b19d08c770a
anchor-sha256: contracts/canonical-program/xrprogram-vm-coverage.json 459b6f0ca3c7608aed67ac648d2640438a2fe1479e4404389c288363556c1179
anchor-sha256: tests/unit/vm/test_xr_program_vm.c 8ad81cc8f86dc132d964cecafe4babb6a3d0c5385611f87ad634a89159a8c35c
anchor-sha256: tests/unit/vm/test_xr_program_vm_runtime.c 72af794b01720c7a78f0e8886975ca15a892738ebd75116424cd1c7861e7afdd
