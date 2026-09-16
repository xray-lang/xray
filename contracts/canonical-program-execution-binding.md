# Canonical program execution binding

`XrTargetProfile` is the sole immutable owner of four independently identified partitions:
target-observable semantics, public BoundaryABI, runtime-kernel policy, and the exact provider
contract set. It is derived only from explicit target facts. Native host probing belongs in a
profile constructor outside this contract; foreign profiles cannot consult compiler-host width,
endianness, preprocessor OS macros, or provider defaults.

`XrValidatedProgram` is the admitted program authority; an artifact or structurally decoded program
is not admissible. The pure `xr_execution_id_compute` owner hashes ProgramId, TargetProfileId,
BoundaryAbiId, and RuntimeKernelId without a live runtime instance. VM binding retains the program
and profile in `XrInstance`; AOT compilation consumes them directly. Generation is deliberately
excluded from that semantic identity and forms the second field of `XrExecutionCacheKey`, so
provider rebinding invalidates live caches without pretending that program/profile meaning changed.

Provider sets are ordered by complete contract IDs. The allocator and panic
roles are unique runtime foundations; arbitrarily many ordinary operation
contracts may coexist within the configured provider budget. Semantic
capabilities are derived independently of those roles. Output and assertion
capabilities require their exact contract and operation identities, so an
ordinary service category or a copied operation ID grants no authority.

Program format 3 stores a canonical logical contract beside each required
operation ID. Equal contract/operation IDs with unequal logical facts are a
builder error; changing those facts changes ProgramId. The structural reader
validates the bounded logical wire, and semantic admission independently joins
its signature and supported exit policies to every actual call. Validated
requirements own their decoded records after the source artifact is released.
The format has no version-2 reader or key-only requirement fallback.

Provider admission compares stable contract identity, exact provider-contract fingerprint,
ordered operation identities, non-null operation entries, and required thread/reentrancy/callback
behavior before the instance becomes ACTIVE. In particular, a provider compiled for a different
target call ABI is rejected even when it implements the same stable operation names.

Every required operation's complete canonical logical wire must equal its
ordinary provider operation in the exact profile. Stable IDs and matching C
shapes cannot substitute for this comparison. The pure execution identity
owner performs it before hashing, so AOT compilation rejects the same mismatch
without constructing a live instance. Thread, reentry and callback requirements
come from the logical declaration rather than a hosted-profile default.

TargetProfile verification joins every provider's runtime profile and physical
pointer width/alignment/endianness to the explicit target machine facts. Every
ordinary operation must permit that machine's operating system. Validation
does not infer the selected target from the compiler host.

Coroutine execution keeps the generation lease pinned while suspended. Resume and cancellation
are distinct operations on that same execution: cancellation is valid only at a verified
safepoint, follows the Program-owned cancel successor, recursively cancels an active sealed child,
and releases the lease only after canonical cancellation publication. Safepoint metadata is the
canonical unordered live-value set, while each suspension instruction carries the ordered tuple
used to transfer and restore those values. The separate cancel segment is an exact subset needed by
the selected reason-private cleanup graph: every live affine owner appears exactly once, and a
needed non-owner may appear at most once. Missing owners, duplicate values, values outside the live
set, and cleanup graphs that fail to consume an owner are invalid. Executors materialize only the
selected edge, so resume never pre-consumes cancellation resources. Exact caller-local `REF` places
and scoped `READ` existential reborrows are admitted only when the Program safepoint carries their unique
owner exactly once; ambiguous projections, missing owners, and duplicate owner paths remain
rejected. Disposing a frame is not an implicit cancellation operation, and no error, panic, or
trap outcome aliases cancellation.

A suspended outcome carries one Program-owned typed readiness request rather
than asking either executor to wait internally. Cooperative yield has no
operand. `timer-after-ms` has one i64 operand normalized into the closed range
0 through 86400000 before publication. The exact TargetProfile independently
requires general coroutine suspension and timer suspension; neither capability
is inferred from hosted execution. Product readiness drivers may resume only a
well-formed request, use monotonic elapsed time for the timer, and fail closed
on every unknown kind, arity, or non-normalized payload.

Callable dispatch is program-owned rather than provider-owned. An indirect call performs a
non-consuming `READ` of its affine callable operand, so an owned pack and a borrowed non-owner
function parameter are both valid; target identity and capture layout remain private to each
executor and never enter the execution binding. If such a call suspends, an owned callable carrier
must occur exactly once in the Program safepoint and in the cancel transfer; a borrowed carrier must
instead have a verified frame-stable origin. A sealed suspending child that receives an affine
callable through a `READ` parameter follows the same rule: the caller retains the unique owner on
normal and cancel continuations rather than converting that borrow into a move.

Provider invocation is available only through ABI-matched typed lease adapters. Nullary and unary
i64 calls, unary `bool(i64)` calls, nullary optional-i64-pair calls, and byte-output writes occupy
distinct trampoline kinds; an operation cannot enter through the wrong union member. Each adapter
resolves the dense program requirement and operation index under the instance lock, pins the lease
ticket for the duration of the call, and invokes user code without holding that lock. A copied
lease cannot be released while any nested or concurrent provider call is in flight.
Program/profile accessors return retained references so a racing release cannot invalidate a
borrowed pointer.

The generation protocol is ACTIVE -> DRAINING -> RETIRED. DRAINING rejects new pins; RETIRED
requires zero pins; a successor is created only from a retired instance at generation + 1. The
execution authority owns retained program/profile references and copied provider bindings. The
pre-existing language object allocation type is named `XrObjectInstance`; `XrInstance` has one
meaning only.

Immutable VM code is built from Program/Profile without acquiring an instance. Its ExecutionId
allows reuse by compatible instances, including a successor, but does not authorize cross-instance
resource use. Each invocation still acquires its actual instance lease and enters only that
instance's admitted provider binding. The binding owner keeps entry code, context objects and any
backing dynamic library alive until retirement and completion of all pinned calls and callbacks;
copying a binding pointer does not retain an arbitrary external object.

BoundaryABI is active for copy-value scalar, aggregate, and variant boundaries. From one exact
`XrInstance`, the materializer deterministically derives boundary-kind-specific type layouts and
function call frames. Aggregate fields use declaration order with natural target alignment;
variants use a `u32` tag and naturally aligned union payload. Every materialized type identity binds
ProgramId, BoundaryAbiId, BoundaryKind, TypeId, and the complete derived layout. Every call identity
binds ExecutionId, BoundaryKind, FunctionId, and all argument/result slots. This contract is a
public-boundary description, not an executor-local representation.

The public BoundaryABI remains active only for copy-value boundaries, so its root tables are empty
and its cleanup is explicitly trivial. Program-internal affine aggregate/variant values, taking
variant projection, and exact `MOVE` calls are admitted without becoming public boundary carriers.
The active Pipe lifecycle slice binds a consumed signed-i64 resource token to an exact
`bool(i64)` provider operation and relies on the enclosing `MOVE` method call to invalidate the
Pipe owner. It does not grant an implicit destructor, public owned boundary, general root/cleanup
rows, or arbitrary resource-token inference. Coroutine boundary state and concrete AOT, FFI,
hybrid, or reloadable adapters remain inactive. Runtime-kernel policy remains a walking skeleton
for the active object/string identity, RC/weak/panic/OOM policies, and generation protocol. No
executor slot, native register, or common local physical plan is stored here.

## Generated native host bindings

Generated typed host wrappers select only explicitly declared adapter kinds
and compile-time-check the complete C function pointer prototype. Source
symbols and legacy boxed AOT helpers cannot infer or replace the declaration.
The binding registry accepts only its immutable generated descriptor identity,
checks platform applicability, and derives behavior flags from the validated
logical thread/reentrancy/callback fields. Output parameters publish atomically
on successful binding. Instance admission remains responsible for the exact
profile and target ABI; this registry is not an instance-admission shortcut.

A UTC query checks conversion of the original signed timestamp into host
calendar fields, publishes no offset on failure, and computes local-minus-UTC
calendar time without reinterpreting either calendar in the host timezone.
Historical sub-minute offsets truncate toward zero. Host timestamp limits are
refusals and never justify clamping to another date. Generated wrappers carry
failure back through the typed provider status.

POSIX pipe operations validate pointer-width tokens against the nonnegative
int descriptor range before any descriptor-consuming or data-mutating call.
An out-of-range token cannot alias a different valid endpoint after narrowing.
The explicit invalid-token close no-op remains separate from malformed tokens.

The generated native registry has direct C call and refusal/resource evidence.
Program/profile logical admission has positive and hostile VM/AOT build tests.
Generated AOT adapter consumption and complete source-program parity remain
separate obligations; these local tests do not establish them.

## Digest anchors

anchor-sha256: src/plan/target/xr_target_profile.h d21c09134a6469510fddfe0e10e961448fe47d79086f223e0b48fb5ff01e31eb
anchor-sha256: src/plan/target/xr_target_profile.c 7b5cfb5d561174bd1b2e4c834efe4290d3297a00a97a847ea4c83ead36e0f3ac
anchor-sha256: src/plan/target/xr_target_profile_verify.c 45c0cd1a438551071bd767e0a966e5950a03b809cbfbad7cbfeab3ed0f020c26
anchor-sha256: src/plan/target/xr_target_verify.c 08188820ebc55c5d0bed9a22b4a21d74346c6d944c13300af5448a981a3417db
anchor-sha256: src/execution/xr_execution.h 3a09783038967320ae3566e258de5ae7108b60d7ed5f4f01a4a020067b447867
anchor-sha256: src/execution/xr_execution.c ec2d2268361393881707557b2331ae27b887bb35408f626681aa13ccd3817ded
anchor-sha256: src/execution/xr_execution_identity.h 5783c870cd0d642c6d60983e24efcd183edbfbb63380ffae3254e5617af5fd51
anchor-sha256: src/execution/xr_execution_identity.c 2b6c5b11049212bf0993b0bc95718004db2c4e51e7d08ee7ee75e960c368a5d1
anchor-sha256: src/execution/xr_boundary_materialization.h 337225749c98d6b0ae0ddce921c1e27b71765dc023ddfef2deb273fef45c8482
anchor-sha256: src/execution/xr_boundary_materialization.c d822157b7d686cd315434b08a730d04a61fba6018a3decbed566f45dfee45f22
anchor-sha256: src/program/xr_program_verify.h 0da4b3d5b59668a6e865bb7c1e20185049cfdbbb653c553418bee63d49119080
anchor-sha256: src/program/xr_program_verify.c e08f00e5a54513e7f50875a5d5247219484eb4bac6c2be62944decd4c632e8aa
anchor-sha256: src/program/xr_validated_program_internal.h b16db5739182ee5129fcd7b4183c2d9f4fe24349287010534aa182bf739a56bf
anchor-sha256: src/runtime/abi/xr_runtime_contract.h 6f59e9d8d7d5f48035db68c7bc4e32722a5c8bd09d573a5031bd87f7adffcf01
anchor-sha256: src/runtime/abi/xr_runtime_contract.c 8c68657d160545bbfc3fafb6b54e347e9a0dbc959122e71d1763b1b5866443bf
anchor-sha256: src/runtime/class/xinstance.h 5a19d7f36bf25723bf9f9c4cb47f60ed0d1abf3d4a7903f281af8d3132b62a97
anchor-sha256: tests/unit/plan/test_target_profile.c e54b53070db66b309a07673c57fb1112781b19cd8eecacacb496fefb04079da6
anchor-sha256: tests/unit/execution/test_xr_execution.c 0f61fac9edfa70cd1f3bae7aee32b0422bc4d2abb119fa00696cb7831bf44af6
anchor-sha256: tests/unit/execution/test_xr_boundary_materialization.c 177583c0f785168d4693a33d035ce52c04c6bfd37eeb4de7dacf0843aeffb601
anchor-sha256: tests/unit/runtime/test_runtime_abi_contract.c fa3d630a996a22e1d515a11f73f75f515906cb5ce6afad00c6e2b183486fd858
anchor-sha256: scripts/check_xr_execution_contracts.py 88b35a547b5f56febd0ccd625761dd47b18242c00f9fca858c616884c090234d
anchor-sha256: contracts/canonical-program/execution-binding-coverage.json 8ef1d631eac81e717054fdc5bb2d70502db86a4349fcfd8c1abaa060b1237429
anchor-sha256: src/execution/xr_stdlib_provider_binding.h 92aa759af130e7fae3e12125fe0508ffea282438f243ebc230402eb0cc9e052b
anchor-sha256: src/execution/xr_stdlib_provider_binding.c 29d2b6c9513aa5d6a054ac2242578fbaffc328aaa9f761960da4b05265a1ad2e
anchor-sha256: src/execution/xr_stdlib_provider_bindings_gen.inc.c 05bb3a050018e28a590208666ed7777181e9b064711e7f5424f5ef1803cf02bf
anchor-sha256: src/shared/xr_time_offset.h df5ee669531c5c252d2604f8cfea1c433148feb59070822eb70f1ec04eb7ff1c
anchor-sha256: stdlib/time/time.c 71bae1e19f5f0d74fac3065562f90c5ea159ef8505d2b2b8e4101f56cc7b8e42
anchor-sha256: src/os/unix/pipe_unix.c cd846b292d23a19a6f1889be1f6ec2905b4021287889afde22709f9cfc071f5b
anchor-sha256: tests/unit/runtime/test_time_utc_offset.c 6c0e67d39fc677f9f4c739d5e57e8747c6024c0a51562e7019e89962c683fc93
anchor-sha256: tests/unit/execution/test_stdlib_provider_binding.c 55c41086c8733bed2a2c4b2196c1725cb370608e88c10338ac41d7086b6de358
anchor-sha256: src/program/xr_program.h ff5c6b09e5226ff1206ac6ffabfc865f18764f101b8627208235a27a4062eb36
anchor-sha256: src/program/xr_program_internal.h 5bf60ec9d924df66d9867c0a872008eba311053afcea426ddfcf7adc0dd8a3c1
anchor-sha256: src/program/xr_core_ir.c 210f94e6b1e04e9987b573362419d088f7d2a52d48b4dcb674225b908af932be
anchor-sha256: src/program/xr_program_encode.c c52df53c20d4a11b1341d8911c54fd5fa2fe7f33f71a16523e0276b9efa6939d
anchor-sha256: src/program/xr_program_decode.c ab7ec45d4b3e0e20316e6bc5f274af7a007d3447e5bfbf2fda82b88e950166ae
anchor-sha256: src/program/xr_program_schema_gen.h a8854ccf5ce0a0c5cda994be76da18c85946a14e5c4d90a2b8d5c90c9eb1a926
anchor-sha256: xisa/program/schema.json 66ebcb692bbdca5f7c4e9e1e037fd4529959f8e7bd8476d039ae7e507e628afe
anchor-sha256: tools/programgen/programgen.py 039540bcc3bff25776373a061a9fbeb300da19d7a6c5e472b0f63295bf25206a
anchor-sha256: tests/unit/program/test_xr_program.c d95115c13dfc6b998e74663b39e9ec7b4292f4d8ce80ceea05ef6866eefcf457
anchor-sha256: tests/unit/program/test_xr_program_provider_requirements.c 0368d273bc7c783ff9c8e72f65e38cee49b2575af94c5a0bec03ec9a33390abf
anchor-sha256: tests/unit/program/xr_program_provider_fixture.h 9a7116c1fdcad29ea945a8e7b651e24490f35fd52f5f3aaab5367e755e8affea
anchor-sha256: contracts/canonical-program/xrprogram-format-v3.md 8bfe617f6cc769f2565e596d408c5bb0b8174ac852bc203e1876d862ebd12398
anchor-sha256: contracts/canonical-program/xrprogram-format-coverage.json ea94e345733d2614486476b74e5b05fc065588d00f49d56f86c1f8f54cf99679
anchor-sha256: contracts/canonical-program/xrprogram-semantic-coverage.json 5e51bc52585373d2134f123d6df5a92dc05634d83fe27132b14718230c29dfbb
anchor-sha256: tests/unit/execution/test_provider_logical_admission.c e18fa4f1c8dec8f96e4c5be777f639ce5b85b8258801a3355bbd4f54327facec
anchor-sha256: scripts/canonical_program_test_profile.py 20343ab8152f7b1f5bdf1cf465df9520fe8e780ed337f50399fdd7d8f4511df2
anchor-sha256: tests/lib/tests/test_canonical_program_test_profile.py a6ceb30eaaec443cd7c0f40a976735ae776caa8cd8a137a7174f1720416b9d6e
