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

Coroutine execution preserves the same boolean constants, selected conditional edges, and
parallel block-argument assignment as ordinary execution. Each execution owns bounded temporary
edge storage: it snapshots the selected payload and any implicit result before overwriting target
arguments, including on a self-loop. That storage is private to the parent or child execution and
is released with it. Multi-block cancellation and provider-failure cleanup follow only validated
reason-private edges; no executor-local search for a convenient terminal is permitted.

The `xray_program_vm_runtime` archive is the embeddable product boundary for this stage. Its exact
source closure contains the CoreSpec projection, XrProgram decoder/verifier, immutable target
profile, BoundaryABI/runtime contracts, execution instance, and typed VM. It excludes frontend,
CoreIR writer, reference evaluator, TargetPlan, legacy Proto VM, and AOT. The runtime-only test
links a source-committed canonical XrProgram artifact against this archive and verifies the
resulting symbol closure. The artifact is deliberately outside the executor build graph: schema or
CoreSpec drift fails closed during validation instead of rebuilding and executing a compiler-side
fixture writer whenever VM or verifier sources change. An excluded, opt-in writer remains available
to regenerate the committed artifact when its schema intentionally changes; it is never a runtime
test dependency.

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
and terminates through `core.cancel.publish`. Canonical safepoint metadata is an unordered exact set;
the suspension instruction's live operands are the ordered resume tuple, and its cancel operands are
the exact cleanup subset. The VM materializes only the chosen segment. Cancellation transfers every
owner once and any explicitly required non-owner at most once through the verified reason-private
cleanup graph, while a normal resume leaves the cancel segment untouched. A sealed child `REF`
parameter receives the exact place owned by its parent execution. VM value/place storage is execution-owned and remains stable while
the child is suspended; caller-local places are represented in the Program safepoint by their unique
storage owner and reconstructed in the selected continuation. Cancellation is a distinct outcome,
retains the cancelled safepoint state for cross-executor comparison, and releases the exact
generation lease; there is no frame-dispose fallback, raw stack-address spill, or
cancellation-as-error compatibility path.

Related field `REF` arguments may share one typed aggregate storage root. Each field place retains
its own checked ordinal, but the parent safepoint carries the aggregate owner exactly once and
reconstructs the field addresses from that root. A separately observed scalar field snapshot stays
a distinct live value: modifying the referenced field after suspension cannot change the snapshot.
The source fixture proves two fields of one affine aggregate together with an independent scalar
reference under normal resume and cancellation. This does not qualify their combination with
deferred provider cleanup, escaped views, or arbitrary nested projections.

A sealed child `READ` existential parameter receives only an explicit read-reborrow. The caller
safepoint must carry the unique affine existential owner exactly once, and the child frame keeps
the borrowed carrier as a non-owner across suspension. Witness-direct calls inside that child use
the same conformance row as synchronous execution. Normal resume leaves the caller owner live;
cancellation consumes only the verified owner suffix. Missing, duplicate, ambiguous, or
interface-mismatched owners are rejected before VM code construction.

Source-case registration and native fixture ownership share one manifest. Selecting one fixture
executes its complete source, reference, VM, and C-emission assertions before publishing that one
artifact; default execution still runs the complete source suite. Free-function and static-method
coroutine cases have independent native outputs and checks. Registry identity, declaration census,
and projection checks reject missing or mismatched evidence rather than silently narrowing the
canonical preflight. A branching deferred-Pipe fixture proves that provider failure can use the
shared cleanup graph while cancellation receives a private copy of the complete multi-block graph,
including the non-owner branch condition. This test-build decomposition changes no VM semantics or
supported capability.

anchor-sha256: tests/unit/program/xr_program_cleanup_graph_fixture.h a964247d2aacfd087417bc19ad30e2c81c870cb5a28521bb953b2dc8d84d2f1f
anchor-sha256: tests/unit/program/xr_program_coroutine_branch_fixture.h c51ff9a5892b84710d42cd5eceab4fc7f2887ed37062dd28b3a2f0bc68fc00ca
anchor-sha256: CMakeLists.txt 24ef3583c9df1d17d0d44ce5ede860b2acf6b70544facc5f05d5dbe8c600788d
anchor-sha256: xisa/core/registry.json 1066fb08ca4dae249c7fc59d26e1ce4f6c387fbbdd16d31480c72badb109483f
anchor-sha256: src/vm/xr_program_vm.h 2576452ee188a355ebf60ec19e405a95200304c5ce733c72aa623855640f6c7a
anchor-sha256: src/vm/xr_program_vm.c ed45b5dce766dfae80042e886e9b46816dc374ac3c68768fd2fee8bc0bd4c0c8
anchor-sha256: src/program/xr_program_verify.h 05a87dca25a389c21133915c9684fc2560f21d02c888e6117a6da3337e4f6d9e
anchor-sha256: src/program/xr_program_verify.c e582bf2d99821a9473a0621914168cccb730f904c93c6180603f2d36f2035eb7
anchor-sha256: src/execution/xr_execution.h 3a09783038967320ae3566e258de5ae7108b60d7ed5f4f01a4a020067b447867
anchor-sha256: src/execution/xr_execution.c 9edba6e59f290cda924a6a337aba73554f8bd7aa1ac5a0e6dfba958d4b998046
anchor-sha256: src/execution/xr_execution_identity.h 5783c870cd0d642c6d60983e24efcd183edbfbb63380ffae3254e5617af5fd51
anchor-sha256: src/execution/xr_execution_identity.c 857dc89de900a4eda6e71c9498aac3657779a93e68d9593cd4fefb374b84f0bf
anchor-sha256: scripts/check_xr_program_vm_contracts.py c38952179d9d09b0e9a9c390b981e9d78b0801da4b550a5fc6b5be90893a7c72
anchor-sha256: contracts/canonical-program/xrprogram-vm-coverage.json dd8b3088756d093a260be71a4663e4e5942153ec98369aedf9de42c9fb32bdaf
anchor-sha256: tests/unit/vm/test_xr_program_vm.c 28164a25784dd5f1b3b0c3397b10edab68e8b410e6d843ae92ca5ba8430fb4bb
anchor-sha256: tests/unit/vm/test_xr_program_vm_runtime.c 75e731e0d36264735ad2f9625206b2ec323e14de9101f91d32827fdffbcce570
anchor-sha256: tests/unit/vm/xr_program_vm_embedded_fixture.h 0e4d14ac337b31262cf3c2e6a51f7d8381c1164962dd81247a7e4cd748ee0695
anchor-sha256: tests/unit/program/xr_program_vm_fixture_writer.c 9e8418945a6b029c67e4d85d76d6603a68f0c77f72cab8ff5a3b6f37691865ab
anchor-sha256: tests/unit/program/test_xr_program_source_build.c 1857b41614f1d73cecf86dcd4c36d63e1c3ad2d9f9cfc7a72ec5f6224a728e6c
