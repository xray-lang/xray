# XIR Array storage, mutation, and physical accounting

Normative runtime contract for the next Array execution slice. Implementation
and qualification are OPEN. Existing verification-test names identify assertion
owners; they do not state that these new assertions already execute or pass.

The source/IR operation family is governed by `xir-array-values.md`, constructed
identity by `xir-constructed-types.md`, and fault/result propagation by
`xir-resumable-calls.md` and `xir-program-instance.md`. This contract introduces
one Array backing and mutation implementation, not an alternate executor or
compatibility representation. It does not implement other container methods,
views, user structs/classes, noncopyable elements, or tracing collection.

## Compact owned storage and the sole layout authority

An Array owns a reference-counted backing whose header records its exact
TypeArena/local ARRAY type identity, allocation domain, initialized length,
capacity, and the information needed for physical release. Every initialized
element has the ARRAY descriptor's exact element type. Unit, internal CELL,
views, abstract parameters, and other non-storable types are rejected as ordinary
Array elements. An empty Array still owns an exact arena/type; it is not an
untyped null carrier or permanently assigned to an execution Instance.

Element storage uses the existing `xr_xir_layout(types, element, target,
XR_XIR_LAYOUT_STORAGE, ...)` authority for size and alignment. Move that pure
service to a runtime-accessible owner when needed; do not create a second width
table in the value runtime or emitter. Header/data offset rounding, capacity
multiplication, and total allocation sizes are checked before allocation.
Length and capacity are representable as nonnegative i64 values and valid native
allocation sizes. Capacity policy is an implementation choice, not a source API.

Backing elements store compact typed payloads, not an array of sixteen-byte
XrXirValue carriers. For a managed element, each initialized slot owns one
payload reference; its type comes from the backing descriptor. No per-element
type tag, reserved carrier word, or separately allocated element box is needed.
The public/native boundary still uses canonical XrXirValue carriers. Load/store
helpers reconstruct or pack those carriers using the sole storage layout and
preserve existing integer, floating-point, boolean and managed-value rules.
Unused capacity contains no owners and is neither inspected nor released.

An owned Array copy retains its backing. A successful indexed read returns an
independently owned element. A shared detach logically copies each initialized
element and acquires its ownership; exclusive relocation transfers existing
owners without incrementing every element reference. Raw byte relocation is
valid only under that exclusive-ownership proof.

## Allocation domains are physical accounting, not write authority

A value domain limits the actual physical allocations charged to that domain;
it is not a sum of all bytes reachable from the current Instance. Importing or
retaining an existing pure Array does not charge the same backing again. The
backing keeps its allocation domain alive after the originating Instance is
released, and its final physical release subtracts from that original domain.
An element retained from another domain keeps its own existing allocation owner.

Every new allocation required by an Array constructor or mutation is charged to
the current admission domain. In particular:

- Shared detachment creates replacement backing in the current admission domain.
- Unique capacity-sufficient replacement or append performs no backing allocation
  and may reuse storage from a different domain. Its existing bytes remain
  charged to that original domain.
- Unique growth in the current domain may use an admitted in-place reallocation.
  Unique growth across domains prepares replacement backing in the current
  admission domain, then transfers owners and publishes the new root. It does
  not expand the old domain or silently reassign an old allocation's accounting.
- New backing headers, element capacity and temporary heap ownership/traversal
  storage are charged to their actual allocation domain. The Array does not
  rehome an existing TypeArena merely because its receiving Instance changes.

The current domain's hard limit applies to the actual temporary peak before
publication; pending release of old storage is not advance credit. Failed work
restores live allocation charges and unpublished owners. Monotonic peak and
allocation/free counters may record a completed allocation and rollback, and
consumed admission work is not refunded merely because the operation failed.

Shared detachment and every exclusive growth precharge one work unit per
initialized element visited or potentially moved, including the replaced slot
for shared set. This includes native realloc in the current domain, whose hidden
relocation is still bounded work. Admission/element validation is charged
separately. Capacity-sufficient exclusive writes have no whole-array traversal
charge. Insufficient work rejects before replacement allocation or realloc and
restores any prepared new-element owner.

Domain equality is neither an exclusive loan nor permission to modify a root.
Write authority comes from the checked writable place and the current execution
context. ARRAY admission must not apply the function/CELL same-domain guard to
all backing objects. Existing direct function and CELL domain/gate rules remain
in force. Same-Program pure Array transfer and foreign-arena rejection retain
their constructed-type contract meanings.

## Synchronous root resolution and one mutation implementation

A resolved Array place contains the exact ARRAY type and a borrowed address of
the actual owner's eight-byte payload storage. It is not an Xray value, Array
snapshot, element address or capability that may escape into a frame, callback,
suspension, capture or result. Local/CELL/module-slot permissions are validated
by the owning execution layer. The root holder remains alive for the entire
synchronous operation. Resolving a CELL additionally validates its exact arena
and allocation domain; exporting that cell's payload does not weaken CELL rules.

The source/IR layer binds the logical root once, evaluates index/arguments/RHS in
the already frozen order, and only then resolves its current contents. All roots
enter the same Array set/push implementation. A late RHS rebind therefore does
not leave a cached old backing or element address. Any source form not yet
admitted is rejected as unimplemented rather than silently reordered.

For set/push, validate the current receiver and new element types, arena/gate
admission, bounds where applicable, and length/capacity arithmetic. Prepare all
fallible new-element ownership before detachment or growth can invalidate an
alias. Complete all allocations and all required shared-element retains before
publication. Then publish to the actual root/element, and only afterwards release
replaced ownership. There is no fallible step after the commit point.

Failure preserves the destination's logical value at the beginning of this
synchronous access and all independent copies. Earlier RHS effects and rebinding
are not rolled back. A failed constructor/read leaves its required canonical
unit output unpublished. Set/push return unit; an index-assignment expression's
independently owned T result is prepared by the source/IR layer before mutation,
so successful storage is not followed by a potentially failing result retain.

The general indexed read uses the receiver snapshot selected before index
execution. For the separately admitted side-effect-free index forms, the
execution wrapper may resolve a readable place and take one temporary receiver
retain, invoke the same getter, then drop that retain before returning. The value
helper borrows an already-live receiver; it does not add an undocumented second
receiver retain. Retain saturation remains observable in the wrapper's frozen
order. No complete Array SSA owner remains solely because of that read.

## Bounded transitive admission

Constructor, get, set/push and every existing host/call/action/return/inbox
boundary use the expected arena/type and the current XrXirValueAdmission. The
same admission work account covers descriptor inspection, visited values, and
real function-gate validation. Any temporary traversal storage respects the
explicit scratch limit as well as physical allocation accounting.

Inside an activation this is the cumulative account owned by the call driver,
shared with its other typed boundaries and borrowed only for the current real
resume view. Array wrappers must not recreate work from Instance poll_limit on
each new/get/len/set/push or cell-place access. Constructor loops, repeated COW
detachment and suspend/resume continue spending the same remaining credit.
Direct host value APIs consume the caller-supplied admission account. Failed
operations preserve value ownership/publication but do not refund consumed work.

Pure arrays may avoid inspecting each element only through an independently
verified no-callable fact derived from the admitted element graph and maintained
by every constructor/mutator. A function-capable element type is not proof of a
particular value's Instance identity: empty arrays can transfer, while every
encountered function must pass the current live gate and signature check.
Nested arrays are traversed without unbounded C recursion. A validated function
gate ends traversal of its already-admitted captures; it does not recurse through
closure/CELL cycles. No unmetered native fallback or trusted cached positive
result replaces these checks.

Foreign/stopped functions inside arrays reject in the same circumstances as a
direct function. A pure backing from another domain can accept a function from
the receiving Instance when the Array's element type permits it and all current
admission checks succeed; the backing's allocation domain is not that function's
execution authority. Foreign-arena elements and illegal CELL elements reject
before publication. Copy/drop self-validation does not invoke execution gates
and remains usable after Program/Instance destruction or revocation. Pure
nonallocating get still requires expected-arena and work admission but does not
require an originating/current Instance lease or allocation domain. Constructors
and mutators require a current allocation domain; encountered functions and
CELL values retain their current domain/gate requirements.

Length is a distinct allocation-free metadata query. It verifies the receiver
header, expected arena/type/kind, length/capacity and storage-layout invariants
under fixed work; it neither reads/returns/calls elements nor performs transitive
function admission. It allocates no scratch storage and needs no current domain,
Instance or live function gate. An owned array containing revoked functions can
therefore report its length without acquiring invocation authority. A readable-
place length wrapper borrows its still-live owner synchronously and introduces
no temporary retain or retain-saturation failure. All whole-value transfer and
actual element-read boundaries keep their separate admission requirements.

## Bounds detail and failure channels

Get/set use the allocation-free XrXirFaultDetail from `xxir_fault.h`: u32 code,
u32 zero reserved, signed i64 index, signed i64 observed length. The supported
native layout is twenty-four bytes with eight-byte alignment. A bounds failure
returns XR_XIR_VALUE_BOUNDS and code 430 with the unmodified signed index and the
current observed length. It requires length >= 0 and index < 0 or index >= length.
Negative indices must not be converted to an unsigned/native index first.

Every get/set call initializes a supplied fault output to all zero; every
non-bounds return leaves it zero. No error string or panic object is allocated.
Output pointers and the canonical unit result precondition are checked before
publication. Length/capacity overflow, admission-work exhaustion and physical
budget failure use LIMIT; allocation failure uses OOM; retain saturation uses
REFCOUNT_LIMIT; malformed type/arena/owner input uses BAD_ARGUMENT. None of those
statuses substitutes for an observed bounds fault. Push has no indexed bounds
result. Call/Instance propagation, sticky initialization failure and malformed
native fault rejection are owned by the fault/call contracts.

## Physical release and required assertions

Array destruction joins the existing allocation-free managed release worklist.
At final backing ownership, queue exactly the initialized owned elements, release
the backing's real physical bytes in its allocation domain, and release its arena
and domain references once. Do not recurse through nested arrays or allocate a
cleanup queue. Each queued child owns any metadata it still requires. Partial
construction/detachment cleanup applies the same rules to its initialized prefix.

Pure Array results remain readable/copyable/droppable after activation, original
Instance, Program and input artifacts die, retaining only backing/element/arena/
domain owners. Function elements retain only their existing required code lease;
a stopped gate does not prevent safe cleanup. Strong cycles retain the existing
cycle limitation and must not be claimed physically freed by reference counting.

Required assertions in the existing value/allocation tests include independent
expected contents for copy/set/push/get, Array<string>, primitive-width storage,
nested arrays, and Array<fn>; shared and unique self-input append; zero-backing-
allocation unique capacity-sufficient writes; cross-domain shared detachment and
unique growth charged only to the current domain; origin teardown before receiver
use; and final physical live counts returning to the expected baseline.

Reject foreign arena/type-ID collisions, nested foreign/stopped gates, malformed
owners, insufficient work/scratch, OOM, and retain saturation without publishing
partial values. Inject failure at every new allocation/retain transition. Test
both directions of asymmetric source/receiver budgets, preserve original
bounds indices -1/INT64_MIN/length/INT64_MAX and empty index zero, and release a
deep finite nested Array without cleanup allocation or native recursion. A
separate execution test must verify writable-place authority; unique backing
alone does not satisfy it. Runtime-only assertions do not qualify VM/native
source behavior, fault propagation or the complete program golden.

verification-test: test_xir_values
verification-test: test_xir_value_allocations
