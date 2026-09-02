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

anchor-sha256: CMakeLists.txt 9303257194858b202c3ddcc8004b912254826ef04234b8b9a3291748978bc004
anchor-sha256: xisa/core/registry.json fb8a16b3180835d0fb79da7f4f517cf14ef047f511fb2f10035c11c9b4477d33
anchor-sha256: src/vm/xr_program_vm.h d7546e32a278bc488c5a4fdc1290a5ed395b39fed31800f8aec6eea07a712d76
anchor-sha256: src/vm/xr_program_vm.c b06a35933c0aac44bf17296b84abb6ecad3afc11ee9eef7205f3c00baee6a092
anchor-sha256: src/program/xr_program_verify.h 4deae77110a835e6bbdd4c83e76e875a706b68098bc78f87fd2f3f27d7bd97ea
anchor-sha256: src/program/xr_program_verify.c b3a92d85a983e6221be6595c5579003205d2bd4c7a26580ae05206bd5a2da4c4
anchor-sha256: src/execution/xr_execution.h d9d777969c65f282ff70a7cc755f26d11b2ebf0a044e05d059fde0049c88d357
anchor-sha256: src/execution/xr_execution.c abb49afef5a5a9233c0e471ad9d0bc71a22cc65726938a6ce613e337f43d06c9
anchor-sha256: scripts/check_xr_program_vm_contracts.py 5fba416acc72582bb02243b56205767512d9b1fe2352f33a90774b19d08c770a
anchor-sha256: contracts/canonical-program/xrprogram-vm-coverage.json 60de4f733ba56a158d48dbf798926dcc71d040d295751e778fb95d2e5d4e1740
anchor-sha256: tests/unit/vm/test_xr_program_vm.c eced7b4a2a377f47fda79dd74cd4590dd6f9286e7f5c992e368d44aaab2960da
anchor-sha256: tests/unit/vm/test_xr_program_vm_runtime.c 72af794b01720c7a78f0e8886975ca15a892738ebd75116424cd1c7861e7afdd
