# XIR source Array declarations and semantic admission

Normative contract for the first source Array family. Implementation and
qualification are OPEN. Existing test names below identify assertion owners;
their registration does not establish that the new assertions execute or pass.
This source contract consumes the constructed-type, Array-operation/storage,
source-query, generic-template and call-fault contracts. It adds no executable
IR, alternate checker, runtime spelling lookup or compatibility interface.

## One source-backed native declaration authority

`stdlib/types/array.xr` declares `struct Array<T>`, a value type with one ordinary
copyable/storable element binder. Its first admitted members are public READ
`get(index: i64) -> T`, public REF `set(index: i64, value: T) -> ()`, and public
REF `push(value: T) -> ()`. The default parameter mode is READ. Unit results are
explicit. The declaration does not require comparison, hashing, display, or
Sendable for every element. Methods needing additional constraints acquire them
only when their own declaration families are admitted.

A deterministic native-declaration generator projects this governed source into
one pure immutable metadata table. The table records the source's logical path,
LF-normalized content identity and positions; value/identity kind; generic
declaration/binder; member identity and visibility; receiver mode; parameter and
result type expressions; allocation/failure and result-ownership facts; and the
lowering identity of admitted members. The generated metadata has no dependency
on a legacy analyzer type, runtime object, parser arena or mutable compiler
session. It is a declaration projection, not another checker or executable IR.

The prelude's Array registration binds to this exact declaration and arity.
Receiver and member spelling are lookup inputs only. Successful lookup produces
the governed declaration/member identity before a consumer selects an operation.
Local files, similarly named declarations, request stdlib paths, import aliases,
and source text copying the schema cannot install or replace this compiler-owned
authority. Source content and generated metadata must agree; stale or malformed
generated declarations fail the generation/freshness gate.

The generator rejects malformed declarations, duplicate type/member identities,
unbalanced or malformed member signatures, and invalid admitted generic binders.
Admitted signatures, value kind, receiver modes, visibility and result contracts
must exactly match their lowering authority. Missing or inconsistent used rows
cannot be filled from a second handwritten signature table. Unadmitted members
remain visible declaration inventory, with no XIR execution qualification. Their
unimplemented semantic constraints do not block the admitted family, but the
inventory cannot silently discard syntactically malformed rows.

Changing Array to a struct must migrate every current reader of its class-shaped
schema in the same change. The existing native/LSP member adapter consumes the
same generated Array declaration and retains its value kind; its former raw
class-only Array parse path is removed. The typed receiver registry and native
member contract view project or validate against the same signature facts. They
must not supply an independent Array signature or make get/set unsafe by default.
An old physical `XR_TID_ARRAY` carrier is not a language identity declaration.
Other native declaration families need not migrate until used, but no second
Array parser or old class alias remains. These metadata changes do not qualify
old Array execution as the new value-semantic pipeline.
The legacy analyzer explicitly rejects Array get/set by their resolved receiver
method identities, including optional chains and unsafe blocks: its former raw
unchecked operations cannot consume the newly safe declaration contract. This
rejection does not restore an unsafe declaration or grant other old operations
XIR qualification.

## Global length identity

`len` has one stable global source identity in the existing core intrinsic
registry. Append `XR_CORE_BUILTIN_LEN` as stable ID 6 without reassigning IDs 1-5.
Its descriptor records one READ length-query operand, i64 result and no allocation
by the length operation. The registry distinguishes this result/effect family
from assertion and output rows and validates its complete shape. The first XIR
specialization admits only the governed ARRAY descriptor and produces ARRAY_LEN.
Other Lengthable families are not thereby admitted by this source owner.

Ordinary lexical/module/import resolution precedes prelude lookup. A user or
imported function named len remains an ordinary call with its own signature,
visibility and result; a value with that name follows normal callability checks.
Neither an alias nor an unrecognized receiver recovers builtin authority. The
existing analyzer's global registration and binding tests use the same identity;
its unrelated legacy execution families receive no new XIR qualification.

## Source type and operation family

The single source owner admits explicit Array<T> annotations, homogeneous array
literals, contextual empty literals, logical value copies, parameters/returns,
index get/set, the three admitted methods, and global len. Nested admitted element
types use the same unified constructed pool. Array elements use the common
copyable/storable/context verifier; source code does not maintain a second type
whitelist. Internal CELL, unit, views and unsupported noncopyable values cannot
be ordinary elements. Empty literals require an Array element context; an
unconstrained empty literal fails. Element evaluation is left to right and uses
the ordinary contextual type/conversion rules before ARRAY_NEW publication.

Function-local T remains owned by its lexical generic declaration. Ordinary
generic bodies using Array<T> are checked at their definitions under their stated
constraints. Constructed interning and specialization use the common type DAG;
ordinary monomorphization occurs on Checked, followed by rechecking. No old AST
monomorphization result grants Checked or native-cache authority.

Module slots, mutable local CELL bindings, and existing LOCAL places all use the
same logical-place operations and runtime mutation implementation. The entry
module's mutable Array bindings are required, not an omitted case. Read parameters
and const bindings cannot produce writable Array places. Import resolution never
grants another module's mutable-slot authority. Existing rejection of mutable
library-module state remains in force. A readable place does not become writable
because its backing happens to be unique.

Mutation binds one supported root place once, evaluates index and all arguments
or RHS exactly once in source order, then resolves that binding's current Array.
A RHS that rebinds the same root therefore changes the value and length on which
the mutation operates. Exclusive access and bounds checks occur after argument
evaluation. Earlier argument effects and rebinding remain visible on failure;
the mutation's pre-commit logical value is unchanged. The first source root forms
are normal named local/captured/module bindings; unsupported member roots or
effectful receiver-place forms fail explicitly, without evaluating a different
program. `a.push(a[0])` is required.

Array get and index reads preserve the receiver's logical value selected before
index evaluation. General effectful index expressions use the owned value form;
their possible calls, writes, suspension and failures cannot be moved across the
snapshot. A named readable place with a literal or ordinary i64-name index may
use the frozen synchronous transient-retain/copy-element/drop helper, provided
index evaluation has no call, write, suspension, allocation or runtime fault.
The initial implementation proves this only for literals and ordinary local i64
bindings. A module index slot may be uninitialized when an initializer calls a
function, so it keeps the owned receiver form unless publication is proved.
The retained receiver is released before that read returns, rather than saved as
an unnecessary Array SSA owner for all later writes. No potentially failing
retain is silently eliminated. General receiver-value temporaries and initial
Array SSA owners retain their real lifetime and measured first-detach costs.

ARRAY_SET and ref set return unit. An index-assignment expression returns the
already evaluated and converted RHS T as an independent logical value. All
fallible result ownership occurs before mutation commits; no later retain may
turn a successful store into a failed assignment. ref push also returns unit.
Get/index return an independently owned element. len borrows the valid receiver,
does not scan its elements or allocate scratch, and produces an exact i64 length.
Bounds faults retain signed index and observed length as E0430 details through
the existing call/action and sticky-initialization fault path.
GET's complete transitive element admission can allocate bounded scratch for
nested function values; its source-backed contract therefore records may-heap
and allocation failure. This does not change LEN's allocation-free constant-work
contract.

Array constructor calls, withCapacity/capacity, slices/views, ptr/mutPtr, map,
filter, iteration and other unadmitted methods remain explicit errors in this
source family. This contract does not implement all user struct declarations,
all generic member functions, const T, copy(array), source ref parameters or move
semantics. These omissions cannot be used to substitute class identity for Array
value semantics or to reject supported ordinary READ value copies.

## Query ownership and failure publication

Source checking records successful governed type/member/global bindings and
their source identities in its existing query snapshot. A used native declaration
can have an owned query-only source module/provenance entry without adding an
executable module initializer. Schema and user generic declaration owners remain
distinct even when they use the same parameter ordinal. Query metadata identifies
the real ARRAY kind and owns all copied names, signature nodes, source positions
and content identities. No generated-row pointer or compiler context is retained
as a borrowed query owner. Queries do not resolve, infer or execute.

Failed private/import/unadmitted member lookups do not record successful bindings.
The existing complete/incomplete/parse-failure boundaries remain unchanged.
Resource failures publish neither Checked nor a query snapshot. New metadata
copies, type interning, operand ranges and facts share the existing metadata/work
budgets and clean up atomically on every allocation failure. Pure compile-time
generated tables may be borrowed during the check but are not a second query
owner or an exemption from snapshot accounting.
Used schema names and signatures first copy into the source construction arena,
then independently into the snapshot. A core len query records stable ID 6 with
unknown source location/content provenance, rather than inventing a source hash.

## Required verification and explicit limits

Generation tests check deterministic output and LF-normalized source identity,
struct/value kind, exact admitted signatures and modes, binder substitution,
uniqueness, invalid/missing rows, and changed source versus stale output. A native
surface test proves Array is still present after its class parse path is removed
and that both metadata consumers observe the same governed rows. The core
intrinsic uniqueness/shape tests cover len's new identity and malformed shapes.

Source and query tests cover Array<string> and Array<i64>, nested admitted types,
contextual empty and wrong/mixed element types, read/const rejection, local and
imported len shadowing, private/alias failures, unused-method refusal, distinct
generic T owners, snapshot lifetime, budget exhaustion and allocation-fault
atomic publication. No new declaration table is accepted merely because its
display names match.

Both VM and native independently verify the complete two-module string program:
`red\nblue\n2\ngreen\nblue\ngreen\n3\n`. The producer returns an Array; the entry
copies it, replaces an element and pushes a copied element while the other copy
remains unchanged. Test module-state isolation, first-initialization and sticky
bounds failure, returned result ownership, Array self-input, RHS root rebinding,
general index snapshot order, and final physical release. Separate runtime/IR
tests own OOM/retain saturation, exact failure atomicity, COW/growth counters,
function-gate admission and transitive release; the output golden cannot stand
in for those assertions. Report actual detach/allocation counts, including a
retained initializer owner, rather than promising zero COW.

Fresh affected Ninja targets, focused source/generator/native-surface tests,
relevant regression, sanitizer and contract gates are required. Compilation-only
metadata tests do not qualify Array execution. Windows evidence does not imply
macOS or another provider passed. No stdlib Checked/native package publication,
default CLI migration or final old-chain deletion is certified by this slice.

verification-test: test_xir_source
verification-test: test_xir_source_native
verification-test: test_xir_source_mixed
verification-test: test_xir_source_generics
verification-test: test_xir_source_admission
verification-test: test_xir_source_allocations
verification-test: test_xir_source_query
verification-test: test_xir_source_arrays
verification-test: test_xir_array_packet_vm
verification-test: test_xir_source_array_native
verification-test: test_xir_source_array_mixed
verification-test: test_native_type_surface
verification-test: test_native_declaration_schema
verification-test: test_api_inventory_native_declarations
verification-test: test_xi_intrinsic
