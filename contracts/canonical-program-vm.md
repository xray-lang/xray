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

anchor-sha256: CMakeLists.txt d047f474b10bcc45b5dfb0dc3ebbff8099a20617a8f2791e8f74936ae13d6525
anchor-sha256: xisa/core/registry.json 8fb9c70b946a02d637c3c74ff2e1dcf93e34376b3eff9e9699df180f01e4819c
anchor-sha256: src/vm/xr_program_vm.h e3b6ee502236bb50d167f8609dd7bc94333f261e45236705fe0e5544cceff3a9
anchor-sha256: src/vm/xr_program_vm.c 3c676da6f630e61a2a0c6e992c1d3f5a4acd8d7c4f16afa6e54ecf6c0201d416
anchor-sha256: src/program/xr_program_verify.h f5d0d3216940bc6001e91e1fad27e350750e0ba863c85618e12e74e626397b0f
anchor-sha256: src/program/xr_program_verify.c b0325e178815ed22a31879b84756318d7203060ed79733774dd9437e594cec11
anchor-sha256: src/execution/xr_execution.h d9d777969c65f282ff70a7cc755f26d11b2ebf0a044e05d059fde0049c88d357
anchor-sha256: src/execution/xr_execution.c abb49afef5a5a9233c0e471ad9d0bc71a22cc65726938a6ce613e337f43d06c9
anchor-sha256: scripts/check_xr_program_vm_contracts.py 5fba416acc72582bb02243b56205767512d9b1fe2352f33a90774b19d08c770a
anchor-sha256: contracts/canonical-program/xrprogram-vm-coverage.json 9eb578cfc826c18f131a076208c9340321a3867196058fd0b5353454bd3c3e47
anchor-sha256: tests/unit/vm/test_xr_program_vm.c e9bf642d7022f74779f1d078cf1c95e37bb518dc44de9fc64c54e7df0431eca4
anchor-sha256: tests/unit/vm/test_xr_program_vm_runtime.c 72af794b01720c7a78f0e8886975ca15a892738ebd75116424cd1c7861e7afdd
