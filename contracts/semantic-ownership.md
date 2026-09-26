# Backend-neutral semantic ownership contract

Every observable operation in the generated `semantic-owner-registry.json`
has exactly one canonical semantic owner, a stable 128-bit operation ID, a
stable 128-bit owner ID, and at least one declared production consumer. The
registry is generated from `xisa/xi/ops.def`; it is not a second planner or a
backend-owned name table. Unknown, unowned, or multiply owned operations,
duplicate stable IDs, missing consumer bindings, and stale fingerprints fail
closed during generation or contract verification.

Native and tagged implementations may use different representations, but
adapters may only marshal representation, ownership, errors, and ABI state.
They must not duplicate observable rules. VM, AOT, CGen, and runtime bindings
for a migrated shared owner consume its generated stable owner ID. CGen adapter
spelling is obtained by stable-ID lookup rather than reconstructed from an Xi
or source-language name.

Regex construction is an ordinary source-class operation. Post-analysis
canonicalization rewrites a regex literal to the module-qualified
`regex.Regex(pattern, flags)` constructor, and `stdlib/regex/regex.xr` owns the
object shape, flag spelling, program cache, and compile/match policy. There is
no regex-specific Xi operation, VM opcode, native class, or shared semantic
core. The C binding exposes only the two Unicode property-table ABI leaves;
reviving a C constructor, cache, flag parser, literal helper, or class
registration fails the owner ratchet.

Bounds semantics have one real operation family: canonical `xi.index.get` and
`xi.index.set`, whose generated VM and AOT consumers perform the access. The
retired `xi.bounds.check` row had no source-lowering producer and no executor
handler, so it is absent from the owner registry and target-machine inventory;
no alias, opcode hole, or alternate bounds owner remains.

The generated shared-core inventory records signatures, production callers,
test callers, representation class, and applicable profiles for every
`src/shared/xr_*_core.h`. A new or removed core, caller drift, a native kernel
that gains `XrValue`, loss of either production `Array.sort` consumer, or
revival of a retired private sort implementation fails the gate.

`shared.truthiness` is the first production owner migrated through this
registry. Its representation adapters classify null, bool, integer, float,
sized, and opaque-object values; the observable decision remains exclusively
in `xr_truthy_core_eval`. Removing a declared production binding, changing its
stable ID marker, or reviving a private truthiness decision fails the owner
ratchet.

Source control-flow admission requires a non-nullable `bool` in every `if`,
`while`, `for`, conditional expression and match guard. Nullable presence must
be expressed as an explicit comparison; an omitted `for` condition remains
legal. The frontend no longer publishes a bare-variable truthiness narrowing
rule or helper. Explicit null tests preserve the null branch and join it with
the non-null branch; a nullable type's null projection is not `never`.
The shared source producer rejects invalid conditions before publishing a
Program. Independent VM and native expectations cover explicit presence,
short-circuiting, loops, conditional values and match guards after compiler
teardown. The lower-level shared representation kernel does not grant source
condition admission.

`shared.assertion` owns the typed `xi.assertion` plan, action-channel
classification, failure schema, renderer, and target bindings. VM and AOT
consume that plan directly; retired condition/equality opcodes and source-name
switches are not semantic fallbacks.

`primitive.type-identity` owns canonical `xi.typeid`. VM, runtime, hosted AOT,
freestanding AOT, and CGen consume the generated owner ID and declared consumer
bit; representation adapters only classify local value tags into the shared
public TypeId core. CGen resolves `xrt_typeof_id` from the generated owner table,
and the legacy `typeOf` source-name dispatch has no remaining production path.
The generated registry, low-level owner header, SemanticPlan row, and
machine-readable fingerprint are all derived from the same `ops.def` row.
Restoring a source-name/literal adapter guess, borrowing another consumer bit,
or losing any of the six production bindings fails the owner ratchet.

`shared.range` owns the start, end, step, boundary mode, length, membership,
index, and display rules for `xi.range`. VM and AOT runtime code only allocate
and expose their local Range representation; CGen resolves
`xrt_range_semantics` by stable owner ID before calling that allocation
adapter. Negative indexing is rejected, inclusive and half-open boundaries are
decided in the shared core, and the INT64 edges are covered by the owner KAT.
Restoring backend-local Range formulas, raw construction helpers, or a literal
CGen adapter binding fails the owner ratchet.

`shared.bitwise-binary` owns `xi.band`, `xi.bor`, and `xi.bxor` for signed
64-bit patterns and arbitrary-precision BigInts. BigInt sign-magnitude storage
is converted according to unbounded two's-complement rules inside the shared
kernel; result sign and normalization are owner decisions. VM, optimizer,
hosted and freestanding AOT, CGen, and runtime code only adapt tags, allocation,
errors, and emitted representation. Per-operation BigInt helpers, operator
overload fallback, raw optimizer folds, or raw CGen `&`, `|`, and `^` emission
fail the owner ratchet.

`shared.shift` owns `xi.shl` and `xi.shr` for scalar and BigInt values. Scalar
counts use modulo 64, left shift wraps, and right shift is explicitly signed or
unsigned. BigInt counts are validated without modulo reduction and limb
planning/application remains in the same owner. VM, optimizer, hosted and
freestanding AOT, CGen, and runtime code may only adapt representation,
allocation, and errors; reviving private shift helpers or raw CGen shift
emission fails the owner ratchet.

`shared.numeric-neg` owns `xi.neg` across signed 64-bit integers, IEEE-754
floats, and normalized BigInts. Integer negation wraps at `INT64_MIN`; floating
negation toggles only the sign bit and therefore preserves signed zero and NaN
payloads; BigInt zero normalization and sign inversion come from the shared
plan. VM, optimizer, hosted and freestanding AOT, CGen, and runtime code may
only adapt value representation, allocation, and errors. Operator overloads,
raw C unary negation, private constant folding, and backend-local BigInt sign
rules are forbidden by the owner ratchet.

`shared.codegen-compiler-fence` owns the compiler-order edge of
`xi.codegen.compiler-fence`. The shared plan preserves program state, declares
no runtime memory effect, and requires a native provider to block memory
reordering across the boundary. VM bytecode projects this to a void value
because no native optimizer exists there; CGen emits the selected provider's
compiler fence. Neither executor may reinterpret the fence as a runtime memory
operation, drop the native ordering edge, or bypass the stable owner ID and
exact plan check.

Higher-order array operations retain the existing native typed fast path for
pure uncaptured callbacks. Captured, dynamic, generic, cross-module, and tagged
forms follow the explicit matrix in `hof-shape-matrix.toml`; unknown target or
effect evidence cannot be guessed inside CGen.

`shared.target-layout-query` owns `xi.target.sizeof` and `xi.target.alignof`.
The owner evaluates a frozen `XrTargetDataLayout` plus the native scalar tag;
VM only materializes the result and CGen emits the already-decided integer.
Generated C may not ask the host compiler to rediscover target layout through
`sizeof` or `_Alignof`, and invalid layout, query, or type inputs fail closed.

Channel buffering has one value-independent FIFO state kernel. The runtime
channel helpers, blocked-sender rotation, timer delivery and VM inline paths
reserve and consume slots through that kernel while holding the existing
channel lock. Payload adapters own their physical values and clear consumed
slots; the kernel neither retains nor destroys messages and does not publish
waiters or wakes. Graph traversal and abandoned-message cleanup enumerate only
occupied slots through the same overflow-safe logical-index projection.

The independent linear-queue model covers arbitrary typed message storage,
empty/full and zero-capacity rings, wraparound, invalid state without mutation,
and logical indexes near UINT32_MAX. Actual channel tests retain close/drain,
transfer, waiter, timer and cancellation assertions. This responsibility move
does not admit Channel types into canonical Program or establish its new
scheduler/transport boundary.

## Verification

verification-test: semantic_owner_inventory
verification-test: semantic_owner_inventory_self_test
verification-test: test_analyzer_strict_conditions
verification-test: test_xr_program_bool_conditions_aot_native
verification-test: test_channel_buffer
verification-test: test_channel_close

Class method override intent is explicit source authority. The contextual
`override` modifier survives AST identity, monomorphization and formatting;
the analyzer resolves the parent target independently and rejects missing or
spurious markers, signature/receiver mismatches and private overrides. It
never manufactures the declaration bit from a matching signature. Interface
implementation alone has no override obligation. This modifier is absent
from callable types and does not authorize a new dispatch or ownership path.

verification-test: test_analyzer
verification-test: test_parser_recoverable
verification-test: test_formatter_comments
verification-test: test_mono

## Repeated consuming operands

Each consuming operand transfers one ownership credit, even when several
operands of one instruction reference the same SSA value. ARC must count
operand multiplicity. An owned value dead after that instruction may transfer
its existing credit to only one operand; every other consume requires a retain.
A borrowed value requires a retain for every consume. Mutually exclusive CFG
paths remain independent, and later uses still require the original owner.

Canonical tuple construction publishes the ownership of its complete logical
type. For copyable owning fields it uses the existing aggregate expansion: one
explicit owner copy per field, then a consuming aggregate constructor. Repeated
source fields therefore remain distinct owner credits in the validated Program.

Public Channel send methods freeze the same payload transfer mode as internal
send instructions. The sealed SEND, TRY_SEND and SEND_TIMEOUT method symbols
identify this operand boundary on a non-null Channel receiver. Only operand 1
carries that mode; the receiver and timeout retain SHARE. Operation and payload
facts must agree before an execution consumer claims the call. Freezing these
facts alone does not admit a method in TargetPlan or establish its runtime ABI.

Prelude unit-enum identity is joined by the complete source declaration key and
stable identity, including prelude ownership, ordered members and zero payloads.
The semantic verifier remains responsible for canonical type/layout consistency.
A same-named user enum, reordered declaration, nullable record or altered identity
cannot stand in for the builtin SendResult declaration.

verification-test: test_target_plan

Runtime-managed Task/Coroutine parameters carry a borrowed semantic handle even
when ARC reports no reference-counting responsibility. Semantic publication must
preserve that borrow in both parameter and defining-operation records; it must
not invent a callee-owned reference or force ARC retain/release on the handle.
The declared READ/REF/MOVE mode and terminal-observation rules remain separate
facts. User classes with the same name do not acquire runtime-managed identity.

An exact frozen runtime-owned Task<T> handle is not a local RC-credit root in the
ownership certificate. This applies to its parameters, loads and publications,
not just its producing GO operation. Its reference-capable type and declared
borrow still exist; task terminal observation and payload ownership are checked
separately. The exemption requires the shared exact builtin Task type judgement;
a user class spelling, unknown type or malformed generic record cannot select it.

verification-test: test_semantic_runtime_task_ownership

Task awaitResult and awaitTimeout publish fresh owned TaskResult enums at the
Xi call boundary. Their runtime-owned Task receiver remains borrowed. ARC
return provenance must be complete OWNED for these exact builtin methods,
including timeout results; the receiver handle is not an RC owner of the enum.

verification-test: test_cgen_task_await_timeout_then_complete
verification-test: test_cgen_coro_await_timeout_passes_deadline

Task.poll follows the same fresh owned TaskResult return contract without
suspending. A pending enum owns no payload; a terminal success carries its
result under the existing Task observation policy. The Task handle remains
runtime-owned.

verification-test: test_cgen_task_poll_pending_and_complete

Null-coalescing expansion must give the null test and present-value branch
distinct AST occurrences. Flow narrowing attached to the present branch must
not overwrite the nullable type of the tested occurrence. Session-owned clones
preserve binding identity while assigning independent node identity.

verification-test: test_native_class_coalesce_authority
