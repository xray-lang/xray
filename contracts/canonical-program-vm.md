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
anchor-sha256: xisa/core/registry.json 27af9375dc228b7f2d45bb460237085d6cbd45888c8dd4d2b55a4be958f297c3
anchor-sha256: src/vm/xr_program_vm.h 76316a1271c256f9899a895ebd20b7d4a86ae2e2a42f7bbc77823a83f7b56c15
anchor-sha256: src/vm/xr_program_vm.c f72664b609099dde9dab634e78b22cc11796acbb841d30411191f8e30cf667ec
anchor-sha256: src/program/xr_program_verify.h 4deae77110a835e6bbdd4c83e76e875a706b68098bc78f87fd2f3f27d7bd97ea
anchor-sha256: src/program/xr_program_verify.c b511c81b15191f3c70cd5fc6597be6dbbb9349f3ddd702c1b0ec3e6820acde0e
anchor-sha256: src/execution/xr_execution.h d9d777969c65f282ff70a7cc755f26d11b2ebf0a044e05d059fde0049c88d357
anchor-sha256: src/execution/xr_execution.c abb49afef5a5a9233c0e471ad9d0bc71a22cc65726938a6ce613e337f43d06c9
anchor-sha256: scripts/check_xr_program_vm_contracts.py 5fba416acc72582bb02243b56205767512d9b1fe2352f33a90774b19d08c770a
anchor-sha256: contracts/canonical-program/xrprogram-vm-coverage.json c74b9ea64f8f5f0d17812f4a5e14c92267a4706c015fbd01ff789ad98bc65f50
anchor-sha256: tests/unit/vm/test_xr_program_vm.c 501a03bdd5a32fdbed65b4b4938c8509eb7e2beea4db5a392fcff954200a26ea
anchor-sha256: tests/unit/vm/test_xr_program_vm_runtime.c 72af794b01720c7a78f0e8886975ca15a892738ebd75116424cd1c7861e7afdd
