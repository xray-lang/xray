# Definition-checked ordinary generic templates

## Error marker admission contract

Error proves that a value is an enum error, without granting methods, payload
access, visibility, construction, Sendable or a concrete future type. A concrete
enum or erased Error value satisfies Error; a type parameter does so only through its own declaration's
Error bit. Integers, strings, structs, arrays, callable values and unconstrained
parameters do not satisfy it. Named functions, nominal declarations and method
parameters use the same marker parser and entailment check. Conjunction requires
every marker; unknown or duplicate markers reject. Error and Sendable cannot be
type-parameter names. Nominal Sendable remains a separate unimplemented proof.

THROW accepts an enum, erased Error or Error-constrained parameter at definition.
Ordinary specialization substitutes and rechecks the operand before execution;
runtime transport still receives concrete enum values. No dynamic dictionary,
AST recheck, implicit payload access or compatibility constraint encoding is added.
Checked semantic 30 introduced the Error bit. The current schema 10 / semantic 31
also separates generic argument ranges from invoke CFG targets. The marker
itself requires no runtime dictionary because open parameters never execute.
The subsequent Error value representation uses Value ABI 12 and Call/Program
ABI 17 under the resumable-call ownership contract. ERROR_ERASE proves its
operand constraint and preserves owned Error values after specialization.
Checked and source conversion/rethrow share that representation; source catch
and checked narrowing control flow remain separate unimplemented capabilities.

## Ordinary templates

Ordinary type parameters range over copyable, storable values. Unit, views and
noncopyable resources and internal CELL types are not ordinary arguments.
Admitted primitives, including the fixed-width numeric family, string and
Atomic<i64>, are Sendable. Ordinary CALLABLE types are copyable/storable but
have no Sendable proof. ARRAY descriptor admission derives storage and Sendable
obligations from its element. `xir-array-values.md` separately governs the
NEW/GET/SET/PUSH/LEN family: ordinary T supplies its copyable/storable element
obligation, but no arithmetic, display, comparison or implicit construction
witness. Empty NEW creates no T and still requires a valid explicit ARRAY<T>
type. Source syntax and runtime qualification remain separate. An unconstrained
parameter gains no additional Sendable authority. Constraint entailment uses declarations and spends work budget.

The admitted source declarations are named `fn f<T, U:Sendable>(x:T)->T` and
explicit ordinary calls `f<string, i64>(x)`. An own-parameter `where T:Sendable`
is normalized by the parser into the same declaration constraint; it does not
create a second checking path. Error and Sendable are reserved as type-parameter names. Type parameters have distinct local
names, no defaults and no inferred members. Their only additional admitted
constraints are the builtin Sendable and Error markers. Copy/assignment/read passing/return
follow from the ordinary argument domain. Marker constraints confer no methods,
operators, reflection, zero construction or display capability. Every body,
including unused templates, is checked once against these facts. Generic
forwarding proves each callee marker from the caller parameter's own constraints.
Concrete instances must not repair invalid definitions through AST rechecking.

Built/Checked use function-local type parameter IDs 65536+ordinal, bounded above
by 131071 and by that function's declared parameter count. Equal ordinals in
different declarations do not identify the same type. A module may
own one generic metadata row per function: parameter constraint bits and a type
argument table. Bits 1 (Sendable) and 2 (Error) are admitted. CALL type_arguments[0]/type_arguments[1]
describe a first/count range in that caller's type argument table; nongeneric
calls use zero/zero. Nonempty ranges exactly partition the table in instruction
order, with no gaps, overlap or trailing entries. Value argument ranges remain
in the value operand table. Callee parameter/result types are substituted into
the caller's type context before comparison and dominance checking. Initializers
and the root entry cannot have type parameters.

All metadata is owned, copied across stages and bounded by aggregate parameter,
metadata and work budgets. Checked packet schema 7 / semantic contract 19 carries a generic-presence
u32 after declarations, followed when present by each row's constraint count/
u32 bits and argument count/u32 type IDs. Older schema or semantic revisions are rejected, with no reader
or alias. Decode performs the same definition and forwarding checks.

Lowered cannot contain type parameters or generic argument tables. Concrete
specialization must operate on Checked, intern instances by declaration identity
plus ordered concrete arguments, finish the reachable closure before sealing,
and reverify before physical lowering. The current host function-ID interface
retains every ordinary definition as a root; only generic instances reachable
from those roots are emitted. Recursive instances reuse the same work-queue
entry, with no C recursion. Instance count, total parameters/blocks/instructions,
metadata and comparison/substitution work are bounded; allocation failure rolls
back the whole result. A template-only package without ordinary roots is not
an executable closure. Debug names are not native-cache identity proofs.
Source type inference, member witnesses,
conditional methods, user interfaces, symbolic callback effects, generic type
constructors and full generic package publication require further contracts and
implementation; this marker/template family does not qualify them.

verification-test: test_xir_generics
verification-test: test_xir_source_generics
verification-test: test_xir_checked_allocations

Constructed components may use enclosing parameter ordinals under
`xir-constructed-types.md`. Callable, ARRAY and internal CELL nodes share one
ordered pool. Calls compare contracts structurally under explicit substitution;
specialization interns closed nodes and remaps all uses, including capture cells
and slots, before rechecking. Array instruction result types and projected
CELL/slot/local receiver types are remapped in the same pass; operand IDs retain
their value/place roles and exact table partition. The specialized result is
rechecked for element equality, place permission and closure before lowering.
Each instance's node cache remains bound to its ordered arguments and declaration
identity. Signature identity alone supplies
no Sendable proof.

Type-expression constraint checking uses the current declaration's ordered
constraint vector each time. A shared expression node is not a cached proof for
another declaration with the same parameter ordinals. The bounded type-layer
context verifier visits expression edges, checks parameter bounds and each
nominal argument obligation, and never traverses derived field edges as authority.
Both stored-field declarations and ordinary function type uses invoke this
check; visibility and storage admission remain separate mandatory checks.
This verifier does not itself grant pool or execution admission. Parameterized
and nested nominal expressions follow the semantic-contract extension in
`xir-nominal-struct-types.md`; source generic declarations remain separate.

Constructed-type substitution uses an explicit pending-node stack bounded by the
verified source pool size and charged to metadata budget. It spends work on
component visits and exact interning comparisons; there is no additional fixed
C-recursion depth limit in specialization. Earlier-index component edges bound
the stack. Closed-node caches are shared, parameterized caches remain per
instance, and partially substituted nodes are never published. Allocation or
work exhaustion returns no artifact. Definition-site call matching and derived nominal field verification share
the type-layer substitution matcher, with budgeted explicit stack storage and
no fixed C-recursion limit. Deep local type expressions exercise specialization;
deep generic call contracts independently exercise matching.

Closed nominal instances and ordinary function templates may coexist in one
Checked package. Their specialization shares one context, type interner and
resource budget. Nominal definitions retain source expression IDs until the
combined result is rechecked; concrete fields and function bodies reuse equal
closed constructed nodes. Each declaration's parameter cache stays independent.
No intermediate field-only artifact is published and no second specialization
pipeline is introduced. Lowering alone removes remaining declaration expressions
and remaps all uses. Abstract nominal expressions follow the same substitution stack and growing closed field queue. Closed nominal
value transport follows `xir-nominal-struct-types.md`; its struct constructors
and member instructions require separate executable admission.
