# XIR owned function values and execution admission

A concrete ordinary callable value carries its canonical Program signature type,
entry identity and a retained execution-admission record. It is copyable through
explicit ownership operations. It conveys no Sendable or no_suspend proof.
Constructing a value checks the full signature and existing module import,
visibility and initializer restrictions. Describing a type never grants access.

Program sealing owns a verified signature table. Boxed value ABI 4, call ABI 7
and Program ABI 2 replace their predecessors without an alternate runtime path.
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
contract 8 rejects previous packets; wire schema remains 3. Function-valued root
slots are instance state; non-root ordinary callable slots have no Sendable proof.

Source admission uses the existing fn(...) -> R spelling (unit omits the arrow).
Closed read-mode signatures, declared non-generic function values, imported public
function values, nested return/parameter contracts, local and root-module storage,
conditional selection and ordinary generic identity transmission are admitted.
The callee value is captured before arguments. Function bodies remain checked at
definition; unsupported capture forms, signature-internal type parameters, modes,
borrow origins and effects are rejected rather than inferred from a selected body.

verification-test: test_xir_checked
verification-test: test_xir_source
verification-test: test_xir_source_native
verification-test: test_xir_source_mixed
verification-test: test_xir_packet_vm
verification-test: test_xir_source_admission
verification-test: test_xir_source_allocations
