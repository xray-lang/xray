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

anchor-sha256: CMakeLists.txt 1eb1ccd79493f4bd4875f42df5e84bb13bac5baf15432ea3bb89d4eef4734d44
anchor-sha256: xisa/core/registry.json 7fa5c92be0b89b9f1654d9bdeb7ffb23e6ea96c5a5c8619e09f37c63559f6e91
anchor-sha256: src/vm/xr_program_vm.h becb11511646a900c570ab7acdeebd493632c6fc98657f19e9e6a175edd90684
anchor-sha256: src/vm/xr_program_vm.c 9ff42936d7248cc83b37938799b737a1d9d4449d0b9892155a97a13e4322a1b6
anchor-sha256: src/program/xr_program_verify.h 74916d1083b6a680e29ce698eb89321270f21c679ef56e7f6d73c542e5dfad61
anchor-sha256: src/program/xr_program_verify.c 015a93ba589808deef762389cd4421a10a7cfa2549a2fa5db6c41451343cbe1d
anchor-sha256: src/execution/xr_execution.h 03bf038c0a855f2244e98dff0ac126f982a4c76662501c7cb3cc5a03c0c75148
anchor-sha256: src/execution/xr_execution.c 63bf9ed5c42f5ec6522eadd65c388ffca0783b78b3cadc0c61aa5e3d5e2b7945
anchor-sha256: scripts/check_xr_program_vm_contracts.py 2fdd58bf97eaf786abe2fab5bc26663bd197128426e146d6f430511d5daecc0c
anchor-sha256: contracts/canonical-program/xrprogram-vm-coverage.json 757e5b95218b1ae8f365e9ba60221d917c600d35a075d209afcfad2ea993594b
anchor-sha256: tests/unit/vm/test_xr_program_vm.c ba49cba0facfcb17411a08b996e36724bdf84b0a05c8eeb41372e9aa70980545
anchor-sha256: tests/unit/vm/test_xr_program_vm_runtime.c 75e731e0d36264735ad2f9625206b2ec323e14de9101f91d32827fdffbcce570
