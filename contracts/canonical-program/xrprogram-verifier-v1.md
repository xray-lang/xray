# XrProgram verifier v1 contract

`XrValidatedProgram` is the only semantic execution admission type. It owns an exact copy of the
canonical bytes and can only be constructed by `xr_program_validate` after structural and semantic
validation succeed. A digest or successful structural decode is not an execution capability.

The Xi writer returns both distributable bytes and its owned validated program. The source owner
transfers that reference under the same default admission policy, without immediately decoding and
validating the identical bytes again. The two outputs own separate byte storage and survive release
of the compiler session and graph. Source request limits may only tighten the default budget;
over-budget or session-commit failure releases both outputs. External bytes, modified artifacts and
consumers with a different admission policy still require full structural and semantic validation.
The source-owner lifetime test also checks a tight byte limit, artifact mutation and rejection under
a stricter verifier work budget; the hostile-input verifier suite remains independent of the writer.

Construction failure transfers no ownership to the caller and releases each acquired allocation
exactly once. CoreIR child constructors publish only owned memory into a zero-initialized tree;
the program owner performs the single failure unwind. Unvisited string constants never retain
producer-owned pointers. Independent verification likewise permits destruction of every partial
table without dereferencing unallocated rows. Allocation exhaustion remains distinguishable from
invalid type semantics. The existing Program test compiles the real constructors with test-only
allocator substitution and injects failure at every allocation in provider, text, nested aggregate
and coroutine fixtures, checking empty rejected outputs and balanced physical allocation/free counts.

Format 3.3 adds exact i8/u8/i16/i32/u64 builtin identities alongside i64/u32/u16. Runtime builtin
rows have fixed identity, kind, ownership and copy fields; dynamic TypeIds and dynamic wire kinds
start at 32. Format 3.2 artifacts are rejected, without an alternate decoder. Structural mutations
of every field of the five new rows remain rejected, and all eight integer types retain exact
parameter and result identities. This representation contract alone does not claim that every
numeric operation is implemented.

Format 3.2 introduced each runtime module's declaration-key-ordered data slots.
The pair of module identity and declaration key identifies one storage owner;
an import or re-export must resolve that owner rather than allocate a second slot.
Each row contains a nonzero key, exact non-void runtime TypeId and declaration
flags, currently only const. REF existentials remain confined and cannot become
module storage. Dynamic type labels are remapped through the same canonical type
owner as function and aggregate references. Slot counts have one program-wide
ceiling in addition to structural record and verifier work budgets.

Direct hostile wire tests reject missing types, meta/void storage, duplicate or
zero declaration keys and unknown flags. Constructor tests cover malformed
storage/count pairs, function-only units with slots, const/type identity changes,
declaration reorder stability and input deep copies. The slot-bearing diamond
also participates in every-allocation failure injection. These are immutable
declaration facts; mutable storage, initialization and drop still belong to the
execution instance and are not established by admitting the table alone.

Module place construction resolves immediate kind 13, a bounded pair of module
and declaration-slot indices, against that immutable table. Its result has the
slot's exact TypeId, place category and non-owner disposition. First publication
accepts only a direct module place in its owning initializer and consumes the
type's required ownership disposition. It cannot initialize a local, projected
or foreign module place. Duplicate publication remains a runtime trap because
control flow determines whether the first publication executed.

Const restrictions follow projections and every incoming SSA edge, including
loop backedges. A const-origin place cannot be stored, exchanged or passed to a
mutable REF parameter. The traversal uses bounded temporary worklists and leaves
the validated Program as the sole semantic owner. Accessing module storage, or
passing it to a REF call, contributes the trap effect for uninitialized storage;
ordinary local-place rules remain unchanged. Module places cannot satisfy the
local-storage origin required by take. These admission checks do not establish
instance storage lifetime, cross-entry scheduling or backend execution: VM and
native activation are still refused until those mechanisms are implemented.

The CoreSpec semantic fingerprint excludes consumer coverage, KAT routing, editorial descriptions
and tombstone prose. Advancing a verifier/VM/AOT implementation therefore cannot invalidate a
program whose language meaning did not change; changing a normative type, operation, effect,
ownership, control-flow or trap rule must change the fingerprint.

Format 3.4 adds an affine dynamic array type with one canonical element TypeId.
Element copy-forbidden contracts propagate through nested arrays; array edges
terminate by-value cycle checking, and confined REF existentials cannot be
stored as elements. Independent wire checks use the full 32-byte type key,
verify exact canonical element remapping, and mutate ownership, copy and type
fields. Constructor/writer and verifier allocation failures are exhausted
separately. Type admission alone grants no array execution operations.

Format 3.5 appends optional type and declaration-ordered variant display names
to each dynamic type row. Each name is bounded to 4096 UTF-8 bytes; minimal
length encoding, no NUL or ASCII control bytes, and section bounds are checked
before allocation. The constructor copies source spellings and the independent
verifier owns its own copies. Names affect the artifact content identity but
never replace nominal keys, TypeIds or variant ordinals for dispatch or lookup.
Duplicate display names remain legal. Independent wire, source-owner lifetime,
UTF-8/boundary and allocation-failure checks cover the new metadata. The runtime
reader rejects older formats; builtin-only fixture payloads remain byte-identical
apart from the version field. This metadata alone does not complete error outcome
ownership or VM/native diagnostic rendering.

Value structs may contain immutable strings and nested value shapes containing
strings. The type graph proves absence of mutable or confined descendants;
reference counting alone cannot reject a value field. Class references and
arrays remain forbidden through aggregate or variant wrappers. Constructor
checks and independent hostile-wire checks enforce this boundary separately.
The Xi producer delegates the same restriction to these type owners instead
of maintaining another blanket rejection of affine fields. Affine value updates
compose existing aggregate projection, explicit owner copy and construction;
they do not extend the trivial aggregate-update operation or add backend rules.
Source tests independently expect 42 after copying a string-bearing struct and
changing string and integer fields while preserving the original. Native fault
injection checks every allocation attempt and physical release on failure.

The v1 verifier is ordered and fail closed:

1. canonical structural decode and resource admission;
2. known CoreSpec semantic identity, computed from normative semantics only;
3. logical type graph, constants, function signatures, entry point and bounded allocation;
4. canonical value definitions and block-local SSA use;
5. block-parameter CFG, terminators, successor arity and reachability;
6. per-operation result, operand, immediate and successor rules;
7. monotone SSA ownership: every affine owner is consumed once or transferred once on every
   successor edge, while READ calls create only call-bound non-consuming borrows;
8. declared effect and capability closure, including sealed direct callees;
9. immutable validated type-state construction.

Values cannot flow implicitly between blocks. A successor receives all cross-block values through
typed block arguments. This makes dominance local and keeps verification work linear in records,
operands and edges. Dynamic aggregate and variant rows are declaration-ordered logical value shapes:
their IDs are canonical semantic-key order, every referenced type must exist, and recursive-by-value
cycles are rejected. No offset, alignment, executor slot, register class or target ABI fact is admitted.
Every type has an explicit logical ownership class (`TRIVIAL` or `AFFINE`) and copy contract
(`TRIVIAL`, `EXPLICIT` or `FORBIDDEN`). Every SSA definition carries `NON_OWNER` or `OWNER`.
`core.owner.copy` admits only a permitted copy contract and creates an independent affine owner;
move, drop, MOVE calls and affine returns consume owners. Aggregate and variant construction copy
trivial operands and consume each affine operand once into the result. Nested ownership and copy
contracts propagate through the logical type graph. A non-owner affine operand cannot become an
owning field implicitly; repeated transfer or use after construction is rejected. Copy-forbidden
values can move into containers whose derived contract remains forbidden, but explicit copy of
either the value or such a container is rejected. Calls, cleanup, coroutine and boundary rows must
satisfy their explicit active operation contracts; supported opcode spelling is not a substitute
for those semantic checks.

Sealed, callable and witness direct calls all propagate their declared PanicInfo
channel and release the caller's remaining owners on abnormal exit. Business
errors still require explicit invoke continuations. A direct callable remains
non-suspending; visible callable and interface contracts must preserve exact
panic types and effect closure. Malicious wire substitutions of error for
PanicInfo and callers that omit a callee's panic effect are rejected. Source
defer regions still require an explicit reason-preserving panic continuation;
implicit propagation does not authorize bypassing them.

Class sharing preserves object identity and creates one owned reference, including for
copy-forbidden classes. A borrowed class reached through aggregate, variant or guarded
existential value projections may be shared only when its root remains alive. The verifier
follows non-owning projections and exact incoming SSA arguments to a frame-stable READ
parameter, or uses the existing scoped-owner proof and consumption checks for a local owner.
A dropped local root cannot become frame-stable by forwarding its borrow to another block.
The source pipeline executes successful and failed interface downcasts, checks its independent
result and class reclamation, and emits the same program for real native execution. Independent
wire fixtures retain rejection of unguarded existential projections, dead roots and explicit
copy of forbidden values. Flow-proven Optional payloads use the existing variant projection;
neither source narrowing nor class sharing introduces an executor-specific operation.

Intrinsic panic, provider-failed and cancellation successors enter reason-preserving control-flow subgraphs, not
necessarily terminal blocks. Reason is derived from the exact operation and successor ordinal;
there is no serialized reason flag or runtime cleanup stack. A bounded per-function traversal
propagates normal execution, intrinsic panic, provider-failed trap 7, and cancellation independently. Ordinary
branches and locally handled typed invoke outcomes preserve the incoming reason. An exact
provider/call refusal edge enters the trap-7 domain, including from cancellation. Distinct reason
domains cannot share blocks, and a normal path cannot enter cancellation through an intermediate
adapter. Every explicit cleanup exit preserves its reason: trap 7 cannot return or publish error,
panic, or cancellation; cancellation cannot become an explicit trap along an ordinary edge.
An intrinsic panic continuation may forward its owned PanicInfo and live owners through branches;
its explicit exits must publish panic. Dropping the payload and returning normally is rejected even
when all owners have been consumed. This rule preserves the previous terminal-only continuation
responsibility across the entire admitted graph.
Existing edge types, exact operand segments, unique owner transfers, and safepoint facts are still
verified. An adapter may drop owners not used by the remaining cleanup and branch onward, but may
not omit or duplicate an incoming owner. Loops inside a cleanup body are legal; finite graph
validation proves explicit exit closure, not termination of arbitrary language programs. Coroutine
yield or a suspending child call inside cleanup is rejected even when its resume edge would
eventually return to the same reason domain. Escaping language error/panic during a defer retains
the separate fatal E0443 rule and is not converted into
provider refusal or cancellation recovery.

`core.output.group` uses the same optional trap-7 continuation as other provider operations.
Its display operands are the prefix before the target block's explicit argument suffix. Normal
execution borrows that prefix; refusal transfers every currently live owner exactly once through
the suffix. The independent verifier checks the suffix types, ownership, local availability and
trap-reason closure. Omitted/duplicated owners, extra successors, implicit class-owner exits,
wrong trap codes and returning from a refusal cleanup are rejected. Allocation-only string
carriers retain their existing domain-release rule for implicit exits. No runtime cleanup stack
or output-specific owner table is introduced.

Sealed and callable child-coroutine calls use normal, cancel, declared error,
declared panic, and optional trap continuations in that order. Error and panic
carry one implicit first argument of the exact signature type and ownership;
cancel carries no implicit value. Every explicit failure input must belong to
the safepoint live set, and each remaining owner transfers exactly once on every
selected edge. Independent wire mutations reject swapped or missing channels,
cancel aliases, wrong payload types or ownership, non-live values and omitted
drops. The Reference evaluator's older coroutine subset is not the authority for
these extended outcomes; independent semantic KATs, admission, VM and real native
observations retain those responsibilities.

An explicitly handled error or panic is not an escaping caller effect. Ordinary
and suspended calls apply the same effect rule after validating their declared
payload continuations. A handler that republishes the payload contributes its
own effect; all other callee effects and capabilities still propagate. Independent
handled-outcome inputs omit both caller failure signatures, consume the panic
owner and return normally. Removing an unrelated callee effect from the caller
remains a semantic rejection.

Array construction accepts only ordered value operands of the exact element type. Every
affine input transfers once; trivial inputs remain available. The result is an affine owner,
including an empty array, and no immediate or successor is allowed. Independent mutations
reject duplicate owners, borrowed affine inputs, wrong element types, a non-owner result,
unexpected immediates and reads after transfer or drop. Sequence length borrows either an
array or immutable string and returns a non-owning i64 without an immediate or successor.
`core.owner.alias` accepts exactly one rooted class or array of the same exact result
TypeId and requires an owning result, with no immediate or successor. It preserves source
availability and identity. Independent mutations reject a non-owner result, unexpected
immediate, changed operand type and acquisition after the source owner has been dropped.

Diagnostics have a stable kind plus section/function/block/instruction/value coordinates. They do
not expose compiler pointers, source paths or backend implementation details. Every allocation,
record, operation, operand, successor and work unit is bounded. Reference and VM execution also
bound aggregate value cells independently from instruction steps, so a small looping program cannot
turn a wide logical aggregate into unbounded executor memory growth.

The reference evaluator consumes only `XrValidatedProgram`. It implements checked and wrapping i64
arithmetic without C signed overflow, explicit traps/errors, logical aggregate/variant values,
block arguments, branches, calls and the pointer-width profile query. Aggregate update produces a
fresh logical value, and variant projection publishes the named tag-mismatch trap before reading a
payload. Explicit owner copy recursively clones logical aggregate carriers under the same bounded
value-cell budget used for ordinary construction. Before a one-shot call releases its arena, an
aggregate return or uncaught typed error is recursively detached into evaluator-owned logical
storage; callers use the typed aggregate view and explicitly dispose the outcome. The evaluator
does not include or call compiler planners, VM handlers, AOT lowering or generated-C helpers.
