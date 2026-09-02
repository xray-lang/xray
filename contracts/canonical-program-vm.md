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
anchor-sha256: xisa/core/registry.json 00044c7993db9758a2357ee0805159bd083818945ee5833b8d19c39d256caf88
anchor-sha256: src/vm/xr_program_vm.h 76316a1271c256f9899a895ebd20b7d4a86ae2e2a42f7bbc77823a83f7b56c15
anchor-sha256: src/vm/xr_program_vm.c 4e887da6562ca3ceb6ecd1004f601ccb187a3d0e8a962847987ba8f8672e45b9
anchor-sha256: src/program/xr_program_verify.h 4deae77110a835e6bbdd4c83e76e875a706b68098bc78f87fd2f3f27d7bd97ea
anchor-sha256: src/program/xr_program_verify.c a74bfe1ce44b3d03199f6928203427c2043f1850edc10cd020570968b355080a
anchor-sha256: src/execution/xr_execution.h d9d777969c65f282ff70a7cc755f26d11b2ebf0a044e05d059fde0049c88d357
anchor-sha256: src/execution/xr_execution.c abb49afef5a5a9233c0e471ad9d0bc71a22cc65726938a6ce613e337f43d06c9
anchor-sha256: scripts/check_xr_program_vm_contracts.py 5fba416acc72582bb02243b56205767512d9b1fe2352f33a90774b19d08c770a
anchor-sha256: contracts/canonical-program/xrprogram-vm-coverage.json 59075f3212680d55036a15733a79d00e693c0d98815b1ac05c14f6ce70b7d7c1
anchor-sha256: tests/unit/vm/test_xr_program_vm.c 34f2f00fac7af2f6d76375c9fb97f9e271aab24c9aac9889dd6d9a5b66faf70c
anchor-sha256: tests/unit/vm/test_xr_program_vm_runtime.c 72af794b01720c7a78f0e8886975ca15a892738ebd75116424cd1c7861e7afdd
