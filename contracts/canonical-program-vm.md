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

The executor covers all fifty-five current CoreSpec operations. `core.logical.not`,
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
Explicit Program trap continuations carry every live owner into the same static Pipe cleanup when
provider failure crosses direct, sealed-direct, indirect-direct, indirect-invoke, witness-direct,
witness-invoke, or sealed-invoke calls. A two-frame source fixture gives the callee and caller
distinct owned Pipes and proves LIFO composition: the inner two closes precede the outer two closes
before the final trap. The executor transfers only trap 7 through each verified edge; it does not
synthesize an unwind policy or catch other traps. The Pipe
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

Coroutine suspension has separate normal and cancel successors. A private VM session remembers the
cancel target only while it is suspended, rejects cancellation at every other lifecycle point,
recursively cancels an active sealed child, charges the child's cleanup steps to the parent budget,
and terminates through `core.cancel.publish`. Resume operands and the exact live-owner cancel suffix
are distinct Program segments. The VM materializes only the chosen segment; cancellation transfers
each owner once to the verified static cleanup block and executes its explicit `owner.drop`, while a
normal resume leaves that suffix untouched. Cancellation is a distinct outcome, retains the
cancelled safepoint state for cross-executor comparison, and releases the exact generation lease;
there is no frame-dispose fallback, place-address reconstruction shortcut, or cancellation-as-error
compatibility path.

anchor-sha256: CMakeLists.txt a91db5257863b84ca8e55cd78ba0cd2623db866f1a3def15d2a4af972cec4c66
anchor-sha256: xisa/core/registry.json 6ef47b9c8055809d8856bd01aed88390c4c4c2a5ba44b6cf23b831d194b0f71b
anchor-sha256: src/vm/xr_program_vm.h 39db3f37a2da2c3c16688f97a29ef880d08a1216ed3fe10eb1b24795a55d0acc
anchor-sha256: src/vm/xr_program_vm.c aa99c4bf627ebdd364c600fde7fad24e1560cff33c83dbc838142f2683470b60
anchor-sha256: src/program/xr_program_verify.h 05a87dca25a389c21133915c9684fc2560f21d02c888e6117a6da3337e4f6d9e
anchor-sha256: src/program/xr_program_verify.c e327f7b2a9354cca67fff4706adb24a159e8923668ef829be73e4087ccbe7476
anchor-sha256: src/execution/xr_execution.h 3a09783038967320ae3566e258de5ae7108b60d7ed5f4f01a4a020067b447867
anchor-sha256: src/execution/xr_execution.c 9edba6e59f290cda924a6a337aba73554f8bd7aa1ac5a0e6dfba958d4b998046
anchor-sha256: src/execution/xr_execution_identity.h 5783c870cd0d642c6d60983e24efcd183edbfbb63380ffae3254e5617af5fd51
anchor-sha256: src/execution/xr_execution_identity.c 857dc89de900a4eda6e71c9498aac3657779a93e68d9593cd4fefb374b84f0bf
anchor-sha256: scripts/check_xr_program_vm_contracts.py c38952179d9d09b0e9a9c390b981e9d78b0801da4b550a5fc6b5be90893a7c72
anchor-sha256: contracts/canonical-program/xrprogram-vm-coverage.json dd8b3088756d093a260be71a4663e4e5942153ec98369aedf9de42c9fb32bdaf
anchor-sha256: tests/unit/vm/test_xr_program_vm.c 94b3787558d81789ec3a67aad3de10db669c0404c2541a1b99a7bf8c119784b6
anchor-sha256: tests/unit/vm/test_xr_program_vm_runtime.c 75e731e0d36264735ad2f9625206b2ec323e14de9101f91d32827fdffbcce570
anchor-sha256: tests/unit/program/test_xr_program_source_build.c e46f4556291ff152f9d86cc18973d6642c3ed543c167107ab6e32fa92d44006c
