# XIR nominal declarations and field type ownership

Current executable identities are Checked schema 9 / semantic 30, Program ABI 17,
Value ABI 12 and Call ABI 17. Later admission sections define the supported source
subsets; historical implementation order does not introduce alternative protocols.

## Abstract nominal expression admission

Semantic contract 27 uses the sole schema 9 pool with nominal expressions
whose ordered arguments may contain enclosing parameters and earlier nominal
expressions. The exact parameter span is derived from the argument sequence.
Declaration identity and ordered arguments form identity; derived field vectors
do not. Semantic contract 23 additionally admits nominal components in callable
signatures under `xir-callable-types.md`. Array nominal components and nominal
Sendable remain outside the current executable source family.

Each declaration field and function use proves nominal argument constraints in
its own parameter context. Visibility follows every nominal expression argument
as well as its root; expression metadata supplies no construction authority.
Closed unused nodes still prove concrete constraints. Abstract pool nodes alone
carry no lexical proof. Derived abstract fields, when supplied, must match exact
substitution and fit the expression's parameter span.

The same Checked specializer substitutes nominal arguments, interns exact
identities, drains fields for the growing closed pool and rechecks the complete
result. Abstract descriptors remain declaration expressions only and are removed
at Lowered projection. Closed descriptor fields must be complete. Parameter
contexts remain separate, allocation/work exhaustion publishes no result, and
the closed field graph must have finite value layout before physical execution.
No AST instantiation, second reader or runtime instantiation is admitted.

The declaration layer owns recursive type-visibility checking. Checked checks
module slot types in their declaring module, including unused slots and nominal
argument types. Native Program sealing uses shared Checked admission and exact
descriptor correspondence instead of repeating naming checks on substituted
signatures. A public outer declaration cannot hide an explicitly named
inaccessible argument. Scratch allocation and visits spend cumulative budgets;
failure publishes no Program. Native admission depends on the shared XIR
verifier and lowering library, without frontend, specialization production,
VM or CGen dependencies.

This owns the declaration/type boundary for user structs. Registration of assertion owners is not implementation or
qualification. Closed value transport and STRUCT_NEW/GET/SET follow the contracts
below. Source struct admission is limited to the concrete family recorded below;
complete struct qualification remains OPEN. Existing source admission must continue to reject
unsupported declarations rather than constructing a partially checked type.

## Identity and the sole owner

### Specialization provenance and opaque substitution

Checked wire admission now uses schema 9 / semantic 27. Program ABI 14 now requires complete specialized Checked evidence during sealing.
Ordinary generic specialization attaches the original definition and exact origins.
Cross-module substitution reuses verified definition authority without adding imports.
The complete cutover removes the preceding revisions; it adds no optional
compatibility reader. Value ABI 11 includes enum arena leases; Call ABI 14 is unchanged.

A specialized Checked module owns one original, unspecialized Checked module
and one origin record per output function. A record contains a source function
index and its exact ordered type arguments, expressed in the output type pool.
The original module has no provenance child. Repeated specialization preserves
this original and the canonical mapping instead of nesting provenance or treating
the closed output as a new template. Ordinary source functions have zero type
arguments and exactly one output mapping. Generic instances are justified by the
reachable closure of calls and function references rooted at ordinary functions;
an unreferenced, self-authorizing instance is not a valid witness.

The module wire traversal appends a u32 provenance-present word after its type
pool. Zero ends the module. One is followed by the original module in the same
canonical module encoding, then exactly output-function-count origin records.
Each record is source-function-index u32, argument-count u32, then ordered type
IDs u32. The nested original must encode provenance-present zero. All presence
words, counts, source indices and type IDs are checked before use. Packet
integrity covers both modules and all mappings. A valid digest alone grants no
authority. The reader owns partial allocations before advancing the cursor and
must release every prefix on malformed data, budget exhaustion or OOM.

Verification structurally checks the output while deferring substituted type naming.
Before publication it independently validates the original under its generic
constraints and definition permissions, then checks mappings, exact function
body/type/call correspondence and instance reachability. Each origin argument
independently satisfies its definition constraint, including unused parameters. Modules, dependency
edges, literal bytes, slots and nominal declaration identities must correspond;
only documented function/type index remapping is permitted. Explicit nominal
names and member operations retain definition authority. A type introduced only
through a verified parameter substitution does not require a reverse import into
the defining module. No instance may gain the caller's nominal-owner privileges.

The proof and output consume a single cumulative work and metadata budget.
Copying the source is not an uncharged second verification budget. Ownership
must survive destruction of the input artifact and packet buffers. Lowered
projection remaps destination type IDs in the origin arguments and preserves
the original template identities; it cannot discard the proof before verification.
Native descriptors need a verified signature-origin projection and bind it to
the same Checked input/cache identity. Descriptor validation is not validation
of arbitrary native machine code, and must not be described as such.

Native Program 14 binds this projection to the complete specialized Checked
packet retained at the Checked-to-Lowered boundary. The packet carries the
original template and origins through schema 9; its digest is the cache content
identity, not authentication. Emitted C carries these bytes and their expected
identity. Native sealing uses the same bounded Checked admission and lowering
implementation, then compares target, function signatures/layouts, type pool,
module identities/imports, literals and slots against the supplied descriptor.
Only successful correspondence can replace the explicit-name check for parameter
substitutions. A projection flag or caller-supplied access whitelist is forbidden.
The native entry callbacks remain trusted compiler-produced code; matching
metadata cannot prove arbitrary callback machine code correct.

Admission and lowering are one reusable XIR library. Frontend parsing, ordinary
specialization production, C generation and the VM are not native runtime
dependencies. Retained packet bytes, temporary decoded/Lowered owners, relation
checking and cleanup must be budgeted; descriptor publication and code-lease
transfer happen only after verification. Temporary proof artifacts are released
before sealing returns. Startup cost and physical failure cleanup require tests;
neither the library split nor packet retention alone qualifies Program 13.

Admission remains closed until compiler ownership, packet round trips and hostile
re-signing, repeated specialization, Lowered remapping, native sealing, physical
failure cleanup and independent VM/native execution pass. Required attacks
include nested provenance, missing/extra origins, wrong source function/arity,
swapped arguments, unreachable invented instances, changed imports/literals/slots,
changed opcode/control flow/callee, explicit inaccessible types disguised as
substitutions and forged native signature projections. The existing external
private/public nominal characterization cases then become success regressions;
the library-private-helper case must continue to use definition permissions.

### Explicit generic source admission contract

Generic struct fields resolve parameters in their own declaration scope. Exact
explicit ordered arguments are required in type annotations and brace literals;
a bare generic name does not infer arguments. A field read or write substitutes
the receiver arguments into the declared field type without gaining access rights.
Declarations support the existing unconstrained and Sendable marker parameters;
Checked proves every field and function use in its own constraint context.
Generic bodies are checked once before Checked specialization, never cloned ASTs.
Generic field defaults are ordinary generic functions checked under the owning
struct parameters and constraints, even when unused or overridden. Omission calls
the function with the receiver's exact ordered type arguments; the result type
is substituted before operand admission. Closures inherit the same parameter
scope and nominal owner. Explicit fields execute first in source order, then
omitted defaults in declaration order. No concrete instance grants a missing
definition constraint. A generic struct has an unconditional default constructor
only when its fields are defaultable in its definition context. The constructor
inherits its parameters and constraints and calls defaults with those parameters.
Explicit type arguments and typed uninitialized bindings use this same function.
Conditional default construction remains outside this admission; in particular
unconstrained T has no implicit zero value even in an i64 instance.
Query snapshots own the same parameterized nominal metadata, with each abstract
field's generic owner identifying its struct declaration. Initial source tests
exercise nested numeric/string instances; complete family qualification is OPEN.

### Concrete source family

The source owner hoists struct identities across the parsed module
closure before checking stored field annotations and ordinary function signatures.
Names resolve through the same lexical/module/import tables. Exported types may
be named through a module alias or a named import. Distinct declarations never
collapse by field shape. Every declaration's fields are checked, including unused
types; the final Checked verifier proves field graphs, visibility and storage.

Admitted fields have explicit admitted value types, normal alignment, ordinary
value storage, and public/private/protected plus const/mutable access metadata.
Except for the public READ direct methods specified below, methods remain closed.
User-declared constructors, inheritance/interfaces,
layout attributes, static/weak/flexible fields and nested
declarations remain errors. This admission does not silently synthesize defaults
or authorize user constructors or other method families.

`Type{field: expression, ...}` requires each required field exactly once. The
source path must resolve to an admitted nominal declaration, including qualified
brace syntax represented by the reused parser's enum-construction AST shape.
That AST shape does not grant enum admission. Expressions run in written order;
only the completed operand vector is permuted into declaration order. Unknown,
duplicate, inaccessible, missing and incorrectly typed fields reject.

An explicit declaration default is compiled once as an ordinary zero-parameter
XIR function in the declaring module with that nominal owner. Its result is the
declared field type and its body is checked even if no literal uses it. It cannot
capture caller locals, acquire generic facts from a future instance, or bypass
field construction permissions. Omitted fields call these Checked functions
after all explicit values, in field declaration order. Explicit fields suppress
their defaults. Ordinary calls and suspension follow the existing execution
pipeline; failure publishes no partial struct and existing cleanup drops prepared
values. Hidden initializer functions have no source-addressable name; a default
for a public field of an exported type is callable across the admitted module
edge, while inaccessible fields retain their construction restriction.
Nested closures inherit their lexical nominal owner; an external caller of the
closure acquires no field access or construction authority. Calling a stored
function field evaluates its receiver and reads the callable once before arguments.
For a struct without a user constructor, `S()` and `var value:S` use the same
ordinary member-owned default constructor, with the visibility of S. Availability
requires every field to have an explicit declaration default or an admitted
default-initializable type. Numeric fields start at zero, bool at false; nested
nominal fields call their own proven default constructor. Non-nullable strings,
Arrays, callable values and identity handles never acquire an implicit zero value.
The availability proof is a budgeted fixed point over the concrete field graph,
not C recursion or a fixed nesting limit. It does not replace the independent
finite-layout proof. Default constructors initialize fields in declaration order
and publish a value only after all fields succeed. Private fields are initialized
under declaration ownership; callers gain no direct field authority. Typed mutable
bindings may omit their initializer only through this same proof; const still
requires an explicit initializer. Unit/nullable and other unadmitted source type
families are not broadened by this concrete admission. Literal omission keeps
the separate explicit-default rule and cannot request zero values implicitly.

Field reads operate on an evaluated owned value. Direct field assignment admits
a mutable named module/local root, records that logical root before evaluating
the RHS, and commits to its current value after RHS calls or suspension. Existing
STRUCT_SET performs ownership admission and COW before publishing. The assignment
expression returns the evaluated RHS value already owned in SSA. Const bindings,
READ parameters and immutable/inaccessible fields cannot grant write authority.
Nested writable access paths and compound field assignment remain unimplemented.

The source query owns nominal declarations and fields and records admitted field
references without conferring runtime access. The existing source program fixture
checks independently fixed typed and rendered output in VM, packet VM, native AOT
and mixed execution, including separate instances, reordered literals, local COW,
root replacement across suspension and string assignment results. Runtime fault
injection and counted physical release exercise this actual source closure.

A named struct has a declaration identity in its declaring module. A concrete
instantiation is that identity plus the ordered, exact type arguments. Field
shape, layout, spelling without its module, an AST address, source position, or
the order of imports cannot substitute for declaration identity. Two named
structs with identical fields remain different types. Anonymous records retain
their separate structural contract; this does not make them nominal structs.

The existing owned XIR type metadata is extended with nominal declaration and
field records. It is not accompanied by a second analyzer type database,
runtime class object, legacy layout reader, or process-global type registry.
Built views may borrow input metadata. Checked snapshots, specialized snapshots,
packets, Lowered artifacts and runtime TypeArena snapshots own everything that
outlives their producer, including names, ordered fields, type expressions and
generic constraints. An independently retained runtime type arena must not
retain an Instance, a Program, compiler storage, or an entire module graph.

Each declaration records its module identity, name, visibility, ordered generic
parameters and constraints, and ordered stored fields. Each field records its
declaration identity/ordinal, name, declared type and visibility/mutability.
Default expressions and constructors have separately checked function bodies;
neither a name nor a metadata flag authorizes their execution or field access.
References to modules, declarations and fields must resolve in the verified
owning artifact. Duplicate nominal identities, duplicate field names, invalid
ordinals, unknown flags, missing storage and noncanonical empty ranges reject.

Runtime identity remains the pair of owning TypeArena and local type identity.
Matching numeric IDs, declaration spellings or field shapes from another arena
do not authorize host value import. Explicit structural import across Programs
remains separately governed. Debug/type-description queries expose no field
read/write, invocation or construction permission.

## Generic definitions and specialization

Every field type is checked in the declaration's own type-parameter and module
scope, including unused declarations. It cannot capture an unrelated function's
type parameters merely because their numeric ordinals match. Fields must meet
their declared storage and copying obligations; a declaration with noncopyable
fields must follow the noncopyable declaration contract before it is admitted.
Unit, internal CELL and views do not become ordinary storable fields through a
nominal wrapper. A field's class or synchronization identity remains an identity
reference when its containing value is copied.

Ordinary constraints remain definition-site authority. Instantiating a struct
checks each declared constraint against the ordered argument at that position.
Knowing a field's name or reflecting on a concrete future instance cannot add
an absent generic constraint. A known `Box<T>` field of type T permits the
already admitted copying/storage of T, not arithmetic, comparison, formatting,
default construction or Sendable without the corresponding proof.

Checked specialization substitutes field types and nested nominal arguments,
interns exact resulting identities, and rechecks the result before layout or
execution. The instance cache includes the declaration and complete argument
list. There is no source-AST monomorphization path or runtime instantiation.
All ordinary executable instances and their required closed type descriptors
must be present before immutable Program sealing.

## Recursive type dependencies and bounded work

The existing CALLABLE/ARRAY/CELL expression nodes remain canonically interned;
adding nominal declarations does not permit arbitrary unverified forward edges
in the existing expression pool. A declaration reference and a physical field
layout dependency are different edges and are verified as such.

An infinitely recursive by-value field layout rejects. An indirect dependency
through an admitted Array or class carrier must not be rejected solely because
the nominal declaration graph has a cycle. A closed regular recursive instance
is represented once per exact identity, with bounded graph traversal rather
than unbounded C recursion. An expanding chain of distinct generic instances
must spend the same cumulative instantiation/metadata/work budgets; exceeding
them reports the budget failure and publishes no partial artifact.

Cached parameter spans, copyability, storability, Sendable and contained-owned
facts are derived and reverified, including dependencies through nominal fields.
A cycle alone does not prove or disprove Sendable; the admitted field and
identity contracts determine it. Hash matches do not replace exact equality.
Validation must account for repeated edges, deep nesting, cycle discovery,
substitution, duplicate checks and error cleanup. The old default-initializer
helper's fixed depth of sixteen is not a language rule or a replacement for
these budgets.

## Layout and ownership

The existing target/context/ABI-bound XIR layout service remains the only owner
of size and alignment. Closed field layout determines each offset, padding,
aggregate size/alignment and relevant copy/drop obligation. Checked packets do
not supply trusted physical offsets. Cached layout is recomputed or verified
against the same service. Generic field layout cannot be guessed before
specialization. Boxed carriers, frame lanes and parameter/result representations
must not silently define Array element or aggregate field stride.

Copying a struct preserves a logical copy of each value field and retains the
identity of each class/synchronization field. It does not deep-copy a class
object graph. COW is an implementation choice with the same observable rules;
RC uniqueness does not grant mutability. Const/READ access cannot modify a
value field. A successful field read owns the resulting logical value with a
lifetime independent of the aggregate storage it came from.

Construction publishes only a fully initialized value. On failure, every
successfully initialized field is released exactly once, in the cleanup order
frozen for the construction operation. Earlier expression side effects are not
rolled back. Field replacement prepares every fallible ownership/layout step
before publishing the replacement, then releases the replaced owner. Aliasing,
self-assignment and independently retained copies remain valid.

Nested mutation follows the checked access path and writes modified value
layers back to their root while stopping value copying at identity fields.
Neither a field index nor a reflective description grants access permission.
No raw field/Array-element address may survive a call, suspension, relocation
or root rebinding without the separately verified borrowing/lifetime contract.
Receiver effects, evaluation order, root resolution and failure order must be
frozen for the actual field operations before those operations are implemented.

## Construction admission and staged implementation

The actual language rules remain in spec sections 2, 5.1 and 5.4. An omitted
constructor is not authority to zero-initialize arbitrary fields. A field
literal is not an omitted-initializer declaration: required explicit fields
cannot be silently filled with numeric zero. Unknown, duplicate and missing
fields, visibility and construction permission each require rejection cases.
The respective default-expression and constructor bodies need ordinary type,
ownership and effect checking before source admission is broadened.

The metadata foundation may precede executable struct values, but it must keep
unsupported signatures, instructions and source forms fail-closed. Internal
type tests do not establish source execution. Before adding executable struct
operations, the companion instruction/operand, callable/receiver, initialization
and concrete wire/ABI cutover contracts must be frozen and implemented atomically
with all their consumers. There is no old packet reader or compatibility alias.
No current protocol version is changed by this preparatory contract alone.

## Concrete metadata representation and admission order

The nominal extension belongs to `XrXirTypes`. It adds an owned declaration
sequence; each declaration owns its module identity bytes, declaration name,
visibility, ordered generic constraints and ordered field records. A field owns
its name and stores its declared type expression and access flags. Field ordinal
is its position in that sequence, not a second caller-authored identity. The
module identity bytes must resolve to the module declaration when verifying a
complete compiler artifact. The independently retained runtime arena copies the
identity bytes without retaining source-module dependency or function tables.
Duplicate module/name declaration pairs reject; names and module names cannot
contain embedded NUL. Zero-length names and unknown flags reject.

A NOMINAL node stores a declaration index and an ordered type-argument sequence.
Its other kind-specific payloads are canonically empty. Existing kinds keep their
payloads and have no nominal payload. Equality compares the declaration and each
argument exactly. The derived parameter span describes the node's argument
expressions only; fields are checked in their declaration's parameter scope.
Array, callable and nominal argument expression edges still point to earlier
nodes. Declaration-to-field-expression references are a separate relation and
may point anywhere in the verified pool. This permits a finite expression pool
for `Node` with an `Array<Node>` field without admitting arbitrary node cycles.
A pool containing declarations but no expression nodes is valid when its fields
use only admitted primitive or declaration-local parameter types; a completely
empty non-null pool remains noncanonical.

Verification first checks table bounds and canonical payloads, then declaration
identity uniqueness, field names, flags, field parameter scope, and argument
constraints. All declarations are checked, including declarations not reachable
from an executable entry. It then evaluates instantiated field dependencies with
explicit work records. A closed instance is interned by declaration plus exact
ordered arguments before following its fields. Revisiting the same instance
through Array is not an infinitely sized layout; revisiting through only stored
by-value fields is rejected. An expanding sequence of distinct instances spends
cumulative metadata and work budgets and cannot publish a partial pool.
Declaration-level cycle detection alone is insufficient: substituted field types
must be considered, including cycles introduced through generic arguments.

Owned cloning and packet decoding use zero-initialized destination records and
publish counts only for cleanup-safe storage. Every string, argument sequence,
constraint sequence and field table has one receiving owner. Failure may expose
neither a borrowed producer pointer nor a partially validated output. Runtime
arena sizing includes aligned table spans and name bytes before its domain
allocation; it must not keep compiler-side auxiliary allocations outside the
charged arena. Poisoning producer storage and failing each allocation prefix
are required before claiming this metadata boundary complete.

The existing Checked type traversal must be extended in the same change that
admits NOMINAL nodes. Schema 7 and semantic contract 19 carry the declaration table and reject the
previous schema 5 / semantic contract 16. Exact-byte fixtures and protocol
reference checks follow the same sole reader/writer. Closed NOMINAL expression
nodes use the explicit kind-4 wire rule below. Closed fields are substituted and
reverified; parameterized expressions use the context proofs and closure defined
above.
No protocol constant changes during preparation alone. Executable value/call/VM
and native ABI changes are separately frozen with struct operations; merely
recognizing metadata cannot make a struct signature, layout or opcode executable.

## Metadata helper implementation boundary

`XrXirNominalTable` is the declaration-table component owned by
`XrXirTypes`; it is not another type registry or an executable artifact. Its
current helper API is `xr_xir_nominal_verify`, `xr_xir_nominal_clone`, and
`xr_xir_nominal_free`. The verifier checks an accompanying constructed pool
before interpreting field type IDs. It admits only the existing field type
kinds at this boundary; closed NOMINAL expression nodes have separate validation
below. Direct closed nominal dependencies and the concrete executable source
family follow the later sections; recursive generic instance expansion remains
unavailable.

The declaration record carries length-delimited module/name bytes, exported
boolean, constraint pointer/count and field pointer/count. Field visibility uses
PRIVATE=1 or PROTECTED=2, with neither meaning public; MUTABLE=4 is independent.
The two visibility bits together and every other bit reject. Recording these
bits grants no access, construction or inheritance permission. Empty pointer
ranges are canonical, names are nonempty without embedded NUL, and name copies
include an owned terminator outside their identity length.

Verification and clone leave the caller budget unchanged on failure. Clone
clears its output before validation and publishes it only after all declarations
are copied. A malformed declaration table rejects before output ownership allocation;
bounded type-validation scratch may allocate. The independently
owned helper copy does not copy the accompanying expression pool; its IDs only
have meaning with that owning pool. Full artifact integration must attach both
components to the same receiving owner before publishing an artifact.

## Required evidence

The following existing owners must gain explicit assertions for this family;
their existing passing tests alone are not proof of the new obligations.

verification-test: test_xir_stages
verification-test: test_xir_allocations
verification-test: test_xir_generics
verification-test: test_xir_checked
verification-test: test_xir_checked_allocations
verification-test: test_xir_values
verification-test: test_xir_value_allocations
verification-test: test_xir_source_admission
verification-test: test_xir_source_query
verification-test: test_xir_emit_program
verification-test: test_xir_program_native

Required metadata cases include same-shaped distinct declarations, same names
from different modules, ordered generic argument identity, field substitution,
invalid module/field references, duplicate names, noncanonical payloads,
by-value cycles, guarded regular recursion, expanding instantiation budgets,
deep graphs and every allocation-failure prefix. Producer buffers are poisoned
or freed before using each owned snapshot, packet and runtime arena.

The executable family additionally needs independently expected VM, native,
Checked-reload and mixed programs with scalar/string/Array/class fields,
cross-module visibility, initialized-prefix failure, nested COW, instance
isolation, suspension/cancellation, escaped results and final physical release.
Complete struct methods, constructors, interfaces and noncopyable values remain
requirements of the overall task and cannot be replaced by a scalar-field demo.

## Closed declaration arena projection

The runtime arena stores the projected nominal identity table in the same domain-charged
allocation as expression nodes and callable parameters. One aligned span walker
owns both sizing and copying: nominal table, identity records, per-declaration
field records, then owned module/declaration/field name bytes including terminators.
Padding, tables and names all count toward allocation bytes; copy traversal spends
work before allocating. Rejected admission and allocation failure preserve the
caller budget and publish no arena. A domain allocation failure releases its
extra domain reference. Releasing the last arena reference frees that allocation
without allocating or visiting a Program, Instance or compiler object.

Runtime projection requires complete closed instance field vectors and owns only
identity records; generic declaration constraints and field expressions remain
Checked-only. The original declaration arity is retained for identity validation.
Projection grants no struct construction or field access authority. Tests must
poison producer storage, exercise retain/drop after dropping the original domain
reference, reject one-byte/work-short budgets, and fail the actual domain
allocation with physical accounting restored.

## Nominal expression payload and current closed admission

Kind 4 is NOMINAL. Its explicit payload contains a declaration index, owned
ordered argument and derived-field pointer/count pairs, and no callable/element/result/flags payload.
Other kinds require an entirely zero nominal payload. Checked schema 9 /
contract 27 encodes kind, parameter span, declaration index, argument count,
then that many u32 type IDs, followed by the derived field vector described below.
Declaration records follow the node sequence.
Zero-argument instances use a null argument pointer. The count must exactly
match the referenced declaration. Identity compares declaration and every
argument; a hash or equal field shape cannot collapse distinct declarations.

Node arguments use the admitted copyable/storable scalar, Array, callable,
parameter or earlier nominal expressions. The exact free-parameter span is
derived from arguments; declared constraints are proved in each use context.
Unit, CELL, unknown types and forward expression edges reject. Direct closed nominal field dependencies,
value operations, Lowered projection and runtime ownership follow the sections
below. Nominal components of callable signatures follow `xir-callable-types.md`.
The entire pool owns its argument arrays through clone, packet decode and
cleanup, including every failed allocation prefix. Runtime projection owns its
closed argument and field vectors instead of retaining compiler argument storage.

The native descriptor and declaration authority require Program ABI 14; owned
nominal values require Value ABI 11, with Call ABI 14 unchanged. Older Program
and Value admissions fail before descriptor dereference. The independent opaque
TypeArena is reached through runtime helpers. Checked uses schema 9 / semantic 27.
Full generated-native regressions rebuild descriptors
and their runtime together; old generated C must retain a failing ABI assertion.

## Derived closed field vectors in Checked

A closed NOMINAL node may carry an owned ordered field-type vector. Empty
means unresolved when its declaration has fields; a present vector has exactly
the declaration field count. The vector is derived data, excluded from nominal
identity. Each field must be closed, storable, and exactly equal to substitution
of that declaration's field expression under the node's ordered arguments.
Verification recomputes this relationship; a packet cannot certify it. Field
vector references may point anywhere in the pool because they are dependency
edges, not expression-construction edges. Nominal-valued fields require the
independent finite-layout proof below.

The single Checked specializer uses its existing bounded substitution stack
and exact constructed-type interner to fill these vectors. It keeps definition
metadata during Checked rechecking, independently owns the published result,
and never retains a source AST. Unresolved fields, allocation failure or budget
exhaustion publish no result. The same type-layer substitution matcher checks
ordinary function-call contracts and derived fields, using explicit bounded
work records instead of C recursion. Verification scratch is charged before
allocation and freed on every path; output ownership is allocated only after
validation succeeds.

In schema8/semantic20, NOMINAL's argument sequence is followed by field-count
and field-type u32 words. Current Value10/Call14/Program13 covers the owned
nominal value, expanded native descriptor and function declaration authority.
The derived vector alone grants no construction, field access or runtime
instantiation. Lowering performs the closed descriptor projection below,
removing template expressions and validating recursive layout.

## Checked definitions and Lowered identity projection

The sole nominal table has exactly one active payload: Checked declaration
records, or Lowered identity records. Identity records own module/name bytes,
exported visibility, original arity and ordered field names/access flags. They
have no constraints or field type expressions. This is a stage projection, not
a second registry. Built/Checked reject identity payloads; Lowered rejects
declaration payloads. Runtime arenas accept only the identity payload.

Before projection, every closed nominal instance must have a completely reverified
field vector. Lowering removes all abstract expression nodes, preserves the
relative order of closed nodes, and remaps every function signature, instruction
type, slot type, nominal argument and field type to the compacted pool. A mapped
reference to a removed or missing node rejects. Zero-expression identity tables
remain valid metadata. The projected owner contains no template type-parameter
IDs and keeps neither Checked storage nor source-module dependency tables alive.

The identity payload and nominal argument/field arrays are independently cloned
and packed into the runtime arena's single aligned, domain-charged allocation.
Generated native C emits the same closed descriptors, including identity-only
pools, and Program13 rejects older native layout contracts. The sole Checked
wire remains declaration-based; identity payloads cannot masquerade as Checked
packages. Type metadata projection alone admits no new struct value operations.
Closed nominal-valued fields follow the graph and layout validation below;
abstract nominal expressions remain Checked-only and their closed instances
must finish specialization before projection.

## Closed nominal field dependencies

A stored field may reference an already closed nominal expression in the same
pool, including a later node. Its target declaration must be exported or belong
to the field declaration's module. This permission is checked for every
declaration, including unused declarations; it grants no value/member access.
All target constraints retain their ordinary instance checks.

Direct nominal fields form a separate by-value dependency graph. Verify it in
Checked before field vectors exist, again after substitution, and for closed
runtime identities. Use verified declaration expressions for unresolved Checked
fields and the reverified field vectors otherwise. An active-path revisit
rejects an infinite layout; revisiting a completed node is valid sharing.
Traversal has explicit pool-bounded storage, charges edges/work and allocation
bytes, and releases scratch on success, budget exhaustion and failure. An
indirect Array/callable carrier is not a direct field-layout edge. This does not
admit Array<Nominal> or callable nominal components; those require their own
physical storage and runtime contracts. Nominal argument expressions and
executable struct operations follow their separate rules above and below.

## Nominal storage layout service

The existing target-bound layout service gains a budgeted nominal field entry.
It accepts a verified closed nominal instance with complete substituted fields,
computes direct nominal children inline, and obtains every non-nominal field's
storage size/alignment from the existing scalar/handle layout function. Fields
retain declaration order. Each offset is rounded up to the field alignment;
aggregate alignment is the maximum field alignment, and size includes final
alignment padding. Empty structs have storage size zero and alignment one.

The current x86_64/Value10 target uses 32-bit sizes/offsets; unrepresentable
addition or padding rejects rather than wrapping. Shared child layouts are
computed once per query with explicit pool-bounded scratch. Work and metadata
are charged, and failure publishes no offsets or layout and leaves the caller's
budget unchanged. The caller owns the exact-count offset buffer. No physical
offset enters the Checked wire or becomes independent type identity. This entry
does not by itself admit nominal frame/call representations or executable
instructions; the boxed runtime owner below is a separate consumer.

## Boxed nominal value owner

Value ABI 11 carries owned NOMINAL values without changing XrXirValue's
16-byte tagged layout. The carrier pins its domain and exact TypeArena, and
holds an ordered vector of owned boxed field values. This internal boxed object
representation is not compact field storage: the nominal storage layout service
alone determines inline offsets/stride when a consumer requests compact storage.
No sizeof(boxed field) may substitute for that storage layout.

Construction validates exact type/arena/count and execution admission for all
fields before publication, copies each owner, and rolls back every initialized
field on failure. Copy retains the immutable snapshot. Reads return independent
owned values. A mutable field update prepares ownership first, detaches a shared
snapshot, publishes once, then releases the replaced owner. Compiler-checked
construction/member authority is a precondition of these trusted runtime helpers;
metadata queries do not confer that authority. Source/opcode consumers apply the
same declaration-scope permissions specified below.

Nested nominal admission uses a budgeted explicit stack and validates each
contained callable through the existing execution gate. Exact arena identity
remains mandatory; pure values may move between instance domains just as arrays
do, while contained callable authority is never inferred from type names.
Destruction queues fields on the existing non-recursive release worklist, performs
no allocation, and drops the metadata/domain only after its owned fields have
been queued. Failed construction or detachment publishes no partial value.

## Function declaration ownership and member authority

Each function identity owns a nominal_owner word: zero for ordinary functions,
or the nominal declaration index plus one for its member/constructor bodies.
An owner must resolve in the same module as the function. Module initializers
and the module entry have no nominal owner. Specialization preserves this
declaration scope without granting additional generic constraints.

Checked records encode module, exported, nominal_owner and member_access as four u32 words.
The current schema9/semantic27 contract has only this representation. Native
function identity uses Program ABI14, Value ABI11 and Call ABI14.
Older Program layouts must reject before reading its declarations or type metadata.

One verified-module permission query serves construction and member operations.
Type access requires the declaring module or an explicit import of an exported
type. Public fields follow that type access; private/protected struct fields
require exactly that declaration owner, even for another same-module function.
Writes additionally require the mutable field flag. Explicit field construction
checks every field's access; defaults or named constructors require their checked
function bodies and are not conferred by this query. Invalid indices, absent
scope, exhausted work or forged owner records reject. No runtime metadata query
or ordinary type parameter gains this authority.

## Closed nominal transport through executable frames

A closed nominal type may be an ordinary parameter, result, root-module slot,
copy, local or PHI value once its pool is verified. Referencing the type in a
function requires same-module visibility or an explicit import of its exported
declaration; this type-use check grants no field or construction authority.
Specialization must produce complete field vectors before Lowered publication.
Nominal Sendable proofs and Array components containing nominal types remain
unavailable until their separate dependency proof is implemented. Callable
components follow `xir-callable-types.md`. Internal
CELL roots follow the checked closed-nominal rule below.

Executable SSA/frame payloads use the existing eight-byte owned carrier;
parameter/result/boxed boundaries use the existing sixteen-byte tagged value.
Copies, PHI snapshots, suspended calls, returns and failure/cancellation cleanup
use the same retain/drop authority as other managed values. The unbudgeted leaf
layout API rejects nominal STORAGE requests; the budgeted nominal storage API
is the sole inline layout authority. A transport descriptor never substitutes
handle width for compact aggregate storage. No alternate executor or entry ABI
is introduced. Source constructors and member instructions remain separately
checked consumers of these representations.

## Explicit field construction and owned field reads

STRUCT_NEW (tag74) and STRUCT_GET (tag75) are admitted in Built, Checked and
Lowered within the schema8/semantic20 contract. NEW's result is a
closed nominal type; args name the canonical operand-table range containing
exactly one value per declared field in declaration order. Its immediate and
targets are zero. It requires construction access to every field; this is not
default initialization or permission to call a named constructor.

GET takes one ordinary owned value operand and a nonnegative field ordinal in
immediate; unused args/targets are zero. It checks the receiver's exact nominal
identity, field read authority and result type. A place is not a value operand:
read the local/cell/slot through its ordinary checked load before GET. NEW inputs
are also values. Both instructions apply ordinary dominance and operand rules.
Definition-field expressions are matched under the nominal arguments at Checked;
Lowered uses the completely substituted field vector. Neither instruction may
infer a missing generic capability from a later concrete field type.

VM and generated C consume the same verified contract and trusted runtime value
helpers through the current call admission. NEW publishes one complete owner or
none; GET returns its own field owner. Result storage and every failure exit use
the ordinary frame cleanup list. Metadata, domain and callable admission are
still checked by the runtime helpers. Mutable field writeback follows the root
contract below; source syntax requires its own declaration admission.

## Mutable fields and owned roots

STRUCT_SET (tag76) has unit result, a writable receiver place in args[0], an
ordinary field value in args[1], and the field ordinal in immediate. Targets
are zero. It requires exact nominal identity, field WRITE authority, matching
substituted field type and dominance. A bare value or READ parameter is not a
write root. Root-module mutable slots, local owners and admitted cell projections
share one root resolver with Array mutation; no parallel struct root authority.

The resolver obtains the current root payload after argument evaluation and
checks cell domain, frame bounds or published same-module slot authority.
The runtime setter consumes a typed payload place, prepares the incoming owner,
detaches a shared snapshot, publishes a replacement payload once, then drops old
owners. Failure leaves the true root unchanged. It does not retain a raw element
pointer across calls. The same place/receiver types replace Array-specific root
carriers without aliases. Internal CELL may contain an earlier closed nominal
node; this does not admit Array<Nominal>, callable nominal components, nominal
Sendable or CELL as an ordinary stored struct field.

The native Program shape check admits structurally valid signatures without a
second naming-authority algorithm. Mandatory packet verification and exact
descriptor correspondence establish naming authority before sealing. Removing
provenance from a foreign opaque instance rejects; an invalid origin mapping or
an original definition explicitly naming an inaccessible caller type also rejects.
Construction/member checks remain active during output verification.

## Public READ method source admission

A public instance method with a READ receiver is an ordinary Checked function
with its exact nominal receiver as parameter zero. Its definition inherits the
struct parameters and constraints and its nominal owner. The receiver expression
is evaluated once before explicit arguments, which retain left-to-right order.
Direct calls pass exact struct arguments through the existing specialization
pipeline. This is an owned logical snapshot, not a writable alias to the caller.
Returning or capturing the receiver follows normal copyable value ownership.

All method bodies are checked even when unused. Direct calls use declaration
identity; field/method name collisions reject. Private field reads retain nominal
owner checking and nested closures inherit that owner. Source query method
signatures record the implicit receiver first. No caller acquires field authority.
Static, ref/move, method-local generics, accessors, operators,
default/rest parameters and user constructors remain outside
this initial admission. Reject these declarations explicitly; do not erase their
receiver or access promises to fit READ. Their full contracts remain required.

## Restricted struct method call authority

Function identities add member_access: PUBLIC=0, PRIVATE=1, PROTECTED=2.
This is independent of module export and nominal_owner. Restricted entries must
have a valid nominal owner and cannot be exported; unknown access values reject.
CALL and FUNCTION_REF both require the caller's exact declaration owner for a
restricted target, in addition to existing module/import/initializer checks.
Structs cannot inherit, so protected struct access has the same owner boundary
as private. This does not define future class inheritance authority.

The source producer admits private/protected READ instance methods and diagnoses
unauthorized access before publishing Checked. Nested closures preserve lexical
owner, including calls on another instance of the same declaration. Generic
specialization preserves owner/access from the original definition and verifies
that correspondence; concrete instantiation never grants extra permissions.

The wire record becomes four u32 words under schema8/semantic20. Program ABI13
updates native declaration layout atomically, rejecting earlier layouts before
reading them. Value11/Call14 apply. Packet admission, canonical native
matching and generated C must all preserve access. No compatibility reader.

Required evidence: same-owner generic and nested closure calls; outside/same-module
other-owner/cross-module rejection; forged access/owner/export and function-ref
rejection; packet roundtrip, specialization and native proof correspondence;
independent VM/native output, failed allocation release and full batch gates.
Static/ref/move, method-local generics and user
constructors remain separate obligations.

## Explicit struct construction

An explicit constructor is an ordinary declaration-owned Checked function with
explicit value parameters and a nominal result; it has no receiver argument.
Omitted parameter types come only from same-name declared fields. Declaration
defaults run first in field order, and every other field starts uninitialized.
Per-field places preserve must/may initialization, including const assign-once.
Normal completion and bare return assemble a complete STRUCT_NEW. Reading one
field needs that field initialized; whole-this use, method binding/call and
capture require every field. Captures own a complete value snapshot. A callable
field may be read/called independently once that field is initialized.

Constructor visibility uses ordinary function member authority. Field literals
retain independent field visibility and do not call the constructor; neither
form grants private field access. Constructor call queries retain the lexical
type/import binding and target the actual constructor declaration. Parameter
ranges come from parser parameter positions. This subset does not qualify class
inheritance, default arguments, variadic/ref/move constructors or every valid
constant-condition control-flow proof. Those remain open language obligations.


## Enum declaration metadata migration

The declaration table explicitly distinguishes struct (kind 0) and enum (kind 1).
A struct has no variants. An enum has at least one named variant, in declaration
order; names are unique within that declaration. Each variant owns a contiguous
range in the flat payload-field vector. Ranges start at the previous range end,
including empty variants, and together cover exactly the complete vector. Field
names are unique within their own variant. Enum payload fields have zero flags;
struct visibility and mutation flags retain their existing meaning.

Verification charges all names, ranges and field types before publication.
Constraints are checked at the declaration, including inactive variants. Clone,
Checked wire transport, specialization provenance, Lowered identity projection,
independent arena copies and native descriptor comparison preserve kind and all
ordered variant names/ranges. Failure, malformed data and exhausted budgets
publish no partial metadata and release every initialized allocation prefix.
The sole wire revision changes atomically; no older reader is retained.

Closed enum type nodes share the complete field-substitution and finite by-value
graph proof. STRUCT_NEW/GET/SET and struct runtime helpers require struct kind;
classification alone grants neither descriptor validity nor member authority.
Source enum construction and flat match follow the admission sections below.
Typed enum errors remain unimplemented.
Enum instruction admission is defined below. The single budgeted inline layout query computes sum-type storage as described
below; mutually exclusive payload sizes are never concatenated.

Runtime enum values use one nominal object record. The arena prebuilds immutable
empty-variant records and lookup tables inside its single charged allocation.
Descriptor backpointers are weak. Each empty value owns one arena reference;
copy/drop never changes the descriptor sentinel reference or release link.
Empty construction performs no allocation. Payload records own only the selected
variant's fields, with exact substituted types and reverse-order release.
Construction copies every field before publication and rolls back the initialized
prefix on failure. Payload reads require the expected active variant before access.
Runtime helpers are trusted consumers of checked authority, not source constructors.

Checked schema 9 / semantic 27 preserves ordered kind and variant metadata.
Native Program ABI 14 rejects earlier identity layouts. Value ABI 11 replaces
Value ABI 10 because enum references may own an arena lease instead of an object
reference; carrier size remains sixteen bytes and typed owned slots eight bytes.
Call ABI 14 is unchanged. Earlier value protocols reject at target admission;
there is no compatibility reader. Full source/VM/native/error qualification,
concurrency and complete batch gates remain required.


## Enum inline storage layout

The target-bound nominal layout query is the sole authority for complete inline
struct and enum storage. An enum discriminant has width zero for one variant,
one byte for 2..256 variants, two bytes for 257..65536, and four bytes above that.
Variant ordinals start at zero. This discriminant layout is not a second value
carrier protocol; executable frames and owned boundaries retain Value ABI 11.

Each variant lays out its own fields from offset zero using their exact closed
storage types. The payload size and alignment are the maxima across variants.
The shared payload starts after the discriminant, rounded up to payload alignment.
The aggregate alignment is the larger of tag and payload alignment (at least one);
the final size includes trailing aggregate padding. Root field offsets use the
complete flat field vector and may overlap across variants. A single empty
variant has size zero/alignment one. Nested structs/enums use the same iterative
dependency traversal; infinite by-value graphs, incomplete substitutions, target
mismatches, offset/size overflow, budget exhaustion and OOM publish no offsets or
spent budget. Logical inline layout does not by itself qualify compact runtime
storage, source construction, match, FFI export or enum errors.


## Enum instruction admission

ENUM_NEW (tag86, OP_COUNT89) has an enum result, nonnegative u32 variant ordinal in immediate, and
an operand-table range in args[0..1] containing exactly that variant's fields.
ENUM_TAG (tag87) returns i64, takes one enum value in args[0], and has zero remaining
arguments/immediate. ENUM_GET (tag88) takes an enum value in args[0], a variant-local
field ordinal in args[1] (metadata, not an SSA operand), and its expected variant
in immediate. All three have zero targets and ordinary value/dominance rules.

The verifier requires enum kind, declared variant/field bounds, declaration
visibility/import authority and exact field substitution in the definition's
constraint context. Construction grants no additional generic facts. Runtime
GET checks the active variant before payload access; a mismatch faults without
publishing a value. TAG does not construct values or grant payload access.
All owned results enter the existing frame cleanup list. VM and generated C use
the same helpers and admitted argument storage; no alternate executor is added.
The sole unpublished schema9/semantic27 revision includes these instructions.
Source match exhaustiveness, source constructors and typed errors remain separate
required consumers, not claims implied by instruction admission.


## Enum source declaration and construction admission

Source enums reuse the parser's EnumDeclNode, named payload fields and variant
paths. Declaration/type parameters use the same checked nominal identity and
constraint machinery as structs without reinterpreting an enum AST as a struct.
All variants and payload types are checked, including unused declarations.
Only explicit generic arguments are admitted initially; inference remains an
open language obligation. Unsupported methods/interfaces/attributes reject.

Unit variants use E.Case; payload variants use E.Case {name: value}. Payloads
require every declared field exactly once, without defaults. Evaluate provided
fields once in source order, then place values in declaration order for ENUM_NEW.
The same lexical/import path resolves the declaration and source query target;
query data grants no construction authority. Generic parameters retain their
owning declaration scope. Enums have no implicit default constructor. Member
.ordinal lowers to ENUM_TAG; arbitrary payload member reads reject. Payload
access uses the checked pattern bindings below. Name/string APIs, inference,
methods, derived conformances and typed errors remain unimplemented consumers.

## Enum match source admission

Evaluate the scrutinee once and retain its ordinary owned value across arms,
guards and suspension. Resolve every variant against that scrutinee's nominal
declaration, including module visibility. Omitted generic path arguments in a
pattern come from the scrutinee, never from payload values or missing constraints.
Test the tag before any ENUM_GET. Payload bindings are immutable owned values
in the arm scope, visible to the guard and body but not to subsequent arms.
Guards are bool and run only after their pattern succeeds; false advances in
source order. A guarded wildcard cannot prove exhaustiveness.

Admitted patterns include unit variants, named payload subsets with nested
enum, bool literal, binding and wildcard subpatterns, whole-value bindings and
wildcards. Unguarded rows must cover every constructor and its payload values. Blocks may precede their final
value expression with ordinary statements. Return/break/continue terminate an
arm through the existing function/loop exit path and contribute no PHI input.
A statement match whose every arm terminates has no join block. A value-context
match requires at least one continuing arm until general never-valued expression
propagation is implemented; this limit does not redefine the language. Arm results require
one exact admitted type and join through PHI; unit results need no PHI. Other scalar literals,
range/multiple patterns, union joins and general never-valued expressions remain
language obligations, explicitly rejected until implemented. Closure capture
scanning must respect match binding scope, not capture shadowed outer names.


## Structural enum pattern extension contract

The source producer normalizes enum payload patterns into one typed constructor
and field tree. Omitted fields are wildcards. Nested declaration paths prove
visibility and exact substituted types independently; a pattern cannot invent
constraints. Bool literals partition bool into false and true. A field's
constructor test precedes its payload projection, and failure transfers to the
next arm without evaluating that arm's guard or body. Bindings retain ordinary
owned snapshots and remain immutable and arm-scoped.

Exhaustiveness is a constructor/field matrix proof over unguarded rows, preserving
correlations between fields. Seeing a variant name alone does not cover its
refutable payload. Wildcards specialize into each constructor's fields. Recursive
analysis and code generation have explicit depth, metadata and work limits;
exhaustion rejects without publishing a partial artifact. The final remaining
pattern tests may be omitted only after the complete matrix proof establishes
that every value reaching that arm matches it. Existing flat cases use this same
owner, with no fallback matcher or second coverage table. Range/multiple/scalar
root patterns and union/never result generalization remain separate unfinished
language obligations, not reasons to weaken the actual language rules.

## Scalar literal payload pattern contract

Integer, decimal and string literal payload patterns use the scrutinee field's
ordinary contextual literal rules and the same typed equality operations as
expressions. Negative numeric literals retain their exact sign and width;
string comparison uses the complete byte length. Literal operands are emitted
only after enclosing constructor tests succeed, with normal value ownership.
No expression call, lookup side effect or missing generic constraint becomes a
literal pattern implicitly. A typed pattern does not create a second equality
implementation in either VM or native code.

Float and string coverage currently requires an unguarded binding/wildcard
path; finite integer literals and intervals use the partition proof below.
Unimplemented families remain admission boundaries, not new language policy.
Non-exhaustive scalar root matches still require a real E0442 fault path and
ultimately typed PanicInfo; integer THROW cannot impersonate that contract.
Ranges, multi-pattern binding joins, nullable/type/structural patterns and
union/never result propagation remain open until their own executable gates.

## Integer interval payload pattern contract

Integer literal and range patterns share contextual integer validation with
ordinary expressions. A half-open interval excludes its upper endpoint; a
closed interval includes it. Empty or reversed intervals match no values and
contribute no coverage. Integer-to-float literal context retains the ordinary
exact-representability rule; floating range patterns remain invalid.

Normalize finite integer intervals to inclusive unsigned order keys using the
source width and a sign-bit bias. Split matrix columns only at observed lower
bounds and checked successors of upper bounds, together with the domain start.
Never enumerate the integer domain, overflow MAX+1, depend on signed C overflow,
or require host int128. Each partition retains complete row correlations and
only unguarded rows prove coverage. Literal endpoints are admitted first;
general compile-time constant endpoints remain an unfinished CTFE obligation.
Endpoint validation and owned query facts precede publication; all allocation,
sorting, matrix work and recursion remain budgeted. Runtime range tests use
ordinary typed GE/LE after enclosing tag tests, with no second comparison ABI.

## Ordered multi-pattern binding contract

Comma alternatives share one arm. Check every alternative at definition time,
including unreachable alternatives, with identical binding-name sets and exact
substituted types. Publish one immutable query declaration per logical binding;
later binding occurrences reference that identity as initialization writes.
Contextual numeric validation runs during pattern normalization, not contingent
on generating its test. Patterns store the checked integer/float payload; runtime
emission consumes that payload after required constructor tests. Thus unreachable
alternatives still reject inexact floating integer literals and retain typed query
facts without executing or allocating their runtime literal values.
Try alternatives in source order, testing each constructor before projection.
Join successful binding values through ordinary typed PHI ownership and run
the guard once. Guard failure advances to the next arm, never another alternative.
Only unguarded rows enter the existing coverage matrix. Capture scanning treats
the common names as arm-local and does not capture shadowed outer names.
All collection, lookup, branching and merging obey existing budgets and fail
without publishing partial artifacts. No eager OR matcher or duplicated body.

## Scalar root matching and failure

Bool, fixed-width integer, float and string roots use the same typed pattern
tree, coverage matrix, ordered alternatives and owned bindings as enum payloads.
Only enum roots require static exhaustiveness. A non-exhaustive admitted scalar
match retains every refutable test, including the last arm, and reaches MATCH_FAIL
only after all patterns/guards fail. A proved complete match omits that unreachable
fault block. Result joins contain only successful continuing arms; the fault path
never fabricates a result value. Scrutinees are evaluated once and owned snapshots
survive guards and suspension. E0442 transport is governed by the resumable-call
contract; full PanicInfo/catch panic integration remains an open language obligation.
