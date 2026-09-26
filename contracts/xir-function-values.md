# XIR owned function values and execution admission

A concrete ordinary callable value carries its canonical Program signature type,
entry identity and a retained execution-admission record. It is copyable through
explicit ownership operations. It conveys no Sendable or no_suspend proof.
Constructing a value checks the full signature and existing module import,
visibility and initializer restrictions. Describing a type never grants access.

Program sealing owns a verified signature table. Boxed value ABI 4, call ABI 7
and Program ABI 3 replace their predecessors without an alternate runtime path.
A value owns its allocation domain and the admission record. The record retains
the immutable Program and code lease, but only observes its originating instance;
it does not own module slots, providers, active frames or instance allocations.

Stopping or freeing the originating instance revokes the record before cleanup.
Escaped values remain readable, copyable and releasable afterwards; they cannot
start execution. The last value and instance record reference releases the code
lease. Instance slots may consequently hold functions without an instance cycle.
Each allocation and failed construction leaves explicit owners and budgets intact.

The first runtime admission uses the existing single-host-thread instance driver.
A function is invoked only in its originating live instance; another instance or
Program rejects it even when numeric type or entry IDs coincide. Cross-instance
call transfer and concurrent admission remain pending and are not certified here.
An authorized private function value may be invoked through its capability; it
does not make its numeric entry a public host entry. Indirect calls use the same
resumable call action and cleanup path as ordinary calls, including suspension,
returns, cancellation, arguments and owned results. No pointer is serialized.

verification-test: test_xir_instances
verification-test: test_xir_allocations

FUNCTION_REF records a concrete callable type and a declaration ID; explicit
ordinary generic arguments use the existing Checked type-argument range and are
resolved before Program sealing. CALL_INDIRECT records an SSA callee ID in its
immediate and the existing ordered argument range. Callee dominance, value role,
full signature and result are checked without enumerating implementation targets.
Both operations use the canonical owned frame layout and resumable ABI. Semantic
contract 9 rejects previous packets; wire schema is 4. Function-valued root
slots are instance state; non-root ordinary callable slots have no Sendable proof.

Source admission uses the existing fn(...) -> R spelling (unit omits the arrow).
Read-mode signatures with Checked generic substitution, declared non-generic function values, imported public
function values, nested return/parameter contracts, local and root-module storage,
conditional selection and ordinary generic identity transmission are admitted.
The callee value is captured before arguments. Function bodies remain checked at
definition; unsupported capture forms, modes,
borrow origins and effects are rejected rather than inferred from a selected body.

verification-test: test_xir_checked
verification-test: test_xir_source
verification-test: test_xir_source_native
verification-test: test_xir_source_mixed
verification-test: test_xir_packet_vm
verification-test: test_xir_source_admission
verification-test: test_xir_source_allocations

## Explicit source specialization values

`name<T,...>` and `module.name<T,...>` form ordinary function values without
calling the target. `<` must be adjacent to the name, as for an explicit call.
The complete type list must end before an expression delimiter (comma, semicolon,
closing parenthesis/bracket/brace, colon or end of input), a member dot, or a new
line. A following parenthesis remains the existing direct generic call; grouping
`(name<T>)(value)` invokes the constructed value. Otherwise speculative parsing
restores the comparison interpretation, including `a<b>c` and `a < b`.

The source AST has a distinct reference node owning its type-argument array in
the parser arena. All explicit types must match the declaration's parameter count
and prove its constraints in the enclosing definition. Module visibility and
imports are unchanged; builtin/constructed types and ordinary value variables
cannot be specialized this way. No generic inference or new runtime dictionary
is introduced. FUNCTION_REF uses the existing Checked type-argument range and
instance collector; every referenced ordinary instance closes before Program
sealing, including reference-only targets and references inside generic bodies.

verification-test: test_xir_source_generics

A discarded standalone specialization value is effectless under E0208, just as
a discarded ordinary function name is; it must be used as a value or invoked.
