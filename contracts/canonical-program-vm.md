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

The active walking skeleton covers all fifteen CoreSpec operations, native LP64 and explicit
foreign ILP32 profiles, direct calls, branch arguments, target queries, traps, typed errors,
resource limits, and generation invalidation. Full-language operation families, public embedding
ABI, adaptive quickening, and any persistent private-code cache remain inactive for later tasks.

anchor-sha256: CMakeLists.txt 1d37c7a42199eb7da2ae3ab69e3416d12fd57d3ae1c855579f0ed778b3bcd287
anchor-sha256: xisa/core/registry.json 6405e1887515cc69f35a4c08f85ded80a979b4acc1465a7e234dcceace8bcbcd
anchor-sha256: src/vm/xr_program_vm.h 787d8c95b3b4af0c679622f53e821724c596029c9f3d024e496cae53bcb22d9e
anchor-sha256: src/vm/xr_program_vm.c 3e4c1ad3ca23b17fef439834739b034043c83388d05dd298915a9c1b320c9eb7
anchor-sha256: src/program/xr_program_verify.h 4deae77110a835e6bbdd4c83e76e875a706b68098bc78f87fd2f3f27d7bd97ea
anchor-sha256: src/program/xr_program_verify.c 0229d01c60a13141f72abf31bd9b4b8ba4f0284250e8725b69072c24d1d02eb2
anchor-sha256: src/execution/xr_execution.h d9d777969c65f282ff70a7cc755f26d11b2ebf0a044e05d059fde0049c88d357
anchor-sha256: src/execution/xr_execution.c abb49afef5a5a9233c0e471ad9d0bc71a22cc65726938a6ce613e337f43d06c9
anchor-sha256: scripts/check_xr_program_vm_contracts.py 7b71be9b3ec6c06f88548e640b141baa15e472d9f546811e28b865f5ebd33ab9
anchor-sha256: contracts/canonical-program/xrprogram-vm-coverage.json 5f6a0e2fde008ce76e186636861542e95495393367f6fb6937afcfa95613dcdd
anchor-sha256: tests/unit/vm/test_xr_program_vm.c af91fc7272566e3da36e29dcf44e0965f14dc05d2e86cb20ca3cef17bb5aed84
anchor-sha256: tests/unit/vm/test_xr_program_vm_runtime.c 72af794b01720c7a78f0e8886975ca15a892738ebd75116424cd1c7861e7afdd
