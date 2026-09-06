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

The executor covers all fifty-two current CoreSpec operations. `core.logical.not`,
`core.logical.and`, and `core.logical.or` operate only on canonical `bool` SSA values. The binary
operations are eager at the Program level because their operands are already evaluated; source
`&&` and `||` expressions whose right-hand side can trap or perform effects are instead projected
as explicit control flow, preserving source short-circuit behavior without executor-local policy.
Indirect calls borrow an affine
callable without consuming it, whether the current SSA value owns the callable or is a non-owner
`READ` function parameter; the callable SignatureId remains the only visible dispatch contract.
Provider calls use shape-specific lease-pinned trampolines for nullary/unary i64,
`bool(i64)`, nullary optional-i64-pair, and byte-sink operations. The byte sink is checked
byte-for-byte against the reference evaluator under both VM decode policies. The Pipe source
slice additionally verifies exact open/close operation identities, endpoint projection, taking
optional projection, a `MOVE` close receiver, and two close calls under one pinned execution
generation. A real-source `time.now()` refusal fixture proves that the independent reference and
VM executors each issue exactly one provider event and publish the same provider-call-failed trap.
This is transport-outcome evidence only: trap-path deferred cleanup remains source-gated until the
Program has an explicit trap continuation rather than an executor-local unwind policy. The Pipe
slice uses natural `!`, `&&`, and `||` syntax and guarded division-by-zero right-hand
sides to verify the same result and short-circuit behavior in the reference evaluator and the
source-path VM. A dedicated logical-operation fixture covers both VM decode policies. Wave 1 source activates scalar,
control, block arguments, and sealed calls; Wave 2 adds logical aggregate construction/projection
and variant construction/test/projection. Aggregate and variant values use VM-private typed arena
cells and never expose a shared physical layout. A one-shot aggregate return or uncaught typed
error is recursively detached into executor-owned logical storage before that arena is released;
the caller may inspect it only through the typed aggregate view and must dispose the outcome.
Aggregate update and the walking-skeleton
trap/error/profile rows remain source-gated by the operation matrix even though their typed handlers
exist. Native LP64 and explicit foreign ILP32 profiles, resource limits, and generation invalidation
remain covered. Full-language operation families, public embedding ABI, adaptive quickening, and any
persistent private-code cache remain inactive for later tasks.

anchor-sha256: CMakeLists.txt 2a14f3d1a43bfc8b6f7050c4317b3b7254a70593550a3a47d9c30c9970da899a
anchor-sha256: xisa/core/registry.json 01fc4b13d8cda3469e9e69a3e6ad9dd26ddd0792eeb6303dbb67d6f758d55cd9
anchor-sha256: src/vm/xr_program_vm.h 76bc53a625384448931283a37449d326cc25848bb4737d7fda15e212463e9da5
anchor-sha256: src/vm/xr_program_vm.c e2b7e5fc590b34d9c293301e0aa6c2e74fa240c199c8a834d52f1a8bd9fc3497
anchor-sha256: src/program/xr_program_verify.h 05a87dca25a389c21133915c9684fc2560f21d02c888e6117a6da3337e4f6d9e
anchor-sha256: src/program/xr_program_verify.c 7f29044bda025b49b4087067400c5136a713ff4878aeb4e10923264183e6ffd0
anchor-sha256: src/execution/xr_execution.h 3a09783038967320ae3566e258de5ae7108b60d7ed5f4f01a4a020067b447867
anchor-sha256: src/execution/xr_execution.c 9edba6e59f290cda924a6a337aba73554f8bd7aa1ac5a0e6dfba958d4b998046
anchor-sha256: src/execution/xr_execution_identity.h 5783c870cd0d642c6d60983e24efcd183edbfbb63380ffae3254e5617af5fd51
anchor-sha256: src/execution/xr_execution_identity.c 857dc89de900a4eda6e71c9498aac3657779a93e68d9593cd4fefb374b84f0bf
anchor-sha256: scripts/check_xr_program_vm_contracts.py c38952179d9d09b0e9a9c390b981e9d78b0801da4b550a5fc6b5be90893a7c72
anchor-sha256: contracts/canonical-program/xrprogram-vm-coverage.json 6ff5a35cbb08102a139e7c2ba6f8b8695154e54b78fed65ab05be501b0c7a79f
anchor-sha256: tests/unit/vm/test_xr_program_vm.c 99dab9f9a3a365ff9cfb6ee29336699fbcdeea5c86ca393b726d4d271ea7f3ae
anchor-sha256: tests/unit/vm/test_xr_program_vm_runtime.c 75e731e0d36264735ad2f9625206b2ec323e14de9101f91d32827fdffbcce570
anchor-sha256: tests/unit/program/test_xr_program_source_build.c fdd4c0c71658fede51e00e7adbbe02cba9da04de95f3177d171eee9d7a5799b9
