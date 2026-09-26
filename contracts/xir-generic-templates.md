# Definition-checked ordinary generic templates

Ordinary type parameters range over copyable, storable values. Unit, views and
noncopyable resources are not ordinary arguments. The currently implemented
concrete domain is bool, i64, string and Atomic<i64>; all four are Sendable.
An unconstrained parameter does not acquire Sendable merely because the current
concrete domain happens to satisfy it. Constraint entailment uses declarations.

The admitted source declarations are named `fn f<T, U:Sendable>(x:T)->T` and
explicit ordinary calls `f<string, i64>(x)`. An own-parameter `where T:Sendable`
is normalized by the parser into the same declaration constraint; it does not
create a second checking path. Sendable is reserved as a type-parameter name. Type parameters have distinct local
names, no defaults and no inferred members. Their only additional admitted
constraint is the builtin Sendable marker. Copy/assignment/read passing/return
follow from the ordinary argument domain. Marker constraints confer no methods,
operators, reflection, zero construction or display capability. Every body,
including unused templates, is checked once against these facts. Generic
forwarding proves each callee marker from the caller parameter's own constraints.
Concrete instances must not repair invalid definitions through AST rechecking.

Built/Checked use function-local type parameter IDs 65536+ordinal. A module may
own one generic metadata row per function: parameter constraint bits and a type
argument table. Only bit 1 (Sendable) is admitted. CALL targets[0]/targets[1]
describe a first/count range in that caller's type argument table; nongeneric
calls use zero/zero. Nonempty ranges exactly partition the table in instruction
order, with no gaps, overlap or trailing entries. Value argument ranges remain
in the value operand table. Callee parameter/result types are substituted into
the caller's type context before comparison and dominance checking. Initializers
and the root entry cannot have type parameters.

All metadata is owned, copied across stages and bounded by aggregate parameter,
metadata and work budgets. Checked packet schema 2 / semantic contract 4 carries a generic-presence
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
