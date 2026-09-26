# XIR callable signature identity

Callable contracts belong to typed XIR metadata, not to an enumerated set of
implementation targets. A source function type uses the existing fn(...) spelling,
ordered parameter modes/types and its result. Missing effect promises stay unknown;
they must not be inferred from one selected body and exported as ABI guarantees.

The admitted metadata family records read-only signatures over bool, i64,
string, Atomic<i64>, enclosing-function type parameters and previously declared
callable types. Unit is a result only.
Parameter mode zero means read; signature flags zero mean an ordinary function
with no Sendable, no_suspend, no_blocking or borrow-call promise. Nonzero modes or
flags and unsupported families fail closed
until their declaration contracts and implementation are admitted. This does not
remove those broader language capabilities from the roadmap.

Type IDs 256 through 65535 name entries in a module-owned signature table. Nested
references point strictly backwards, so no recursive expansion or hidden cycle is
needed. Exact ordered parameter contracts and result identity are interned once;
duplicate signatures, unit parameters, forward/self references, invalid types and
uncanonical empty arrays are rejected. All entries are verified, including unused
ones. Work, parameter and metadata budgets cover table traversal and uniqueness.
An ordinary callable has no Sendable proof, including when supplied as a generic
argument; definition checking cannot gain a constraint from a future body.

Checked transitions and packets own every descriptor and parameter record; parser
arenas, borrowed construction buffers and input bytes may be destroyed afterwards.
Specialization substitutes components, interns a fresh closed signature table and
remaps all type uses before rechecking. Checked schema 4, semantic contract 10
appends the canonical callable table after generic
metadata; older schema/contract pairs are rejected without an alternate reader.
Wire records contain only type/mode/flag integers, never code pointers or instances.

Lowered consumes this same table for owned function values, reference construction
and indirect calls. Layout, boxed ownership, Program sealing, execution admission
and code-result leases follow `xir-function-values.md`. Target implementations are
not substituted for signature contracts, and unsupported promises remain closed.

verification-test: test_xir_checked
verification-test: test_xir_checked_allocations
verification-test: test_xir_generics

## Enclosing generic parameters

A signature's `parameter_span` is exactly one plus its largest free parameter
ordinal, or zero when closed; nested signatures contribute their verified span.
Ordinals refer to the function using the type, never to an implementation body.
Every use must fit that function's declared parameter list. Unused descriptors
are still structurally checked. Global slots and a sealed Program require span
zero; a module without templates and every Lowered module reject open entries.

Definition checking compares structural call contracts under the explicit type
argument substitution. It cannot infer extra capabilities, inspect future target
bodies or revisit AST during specialization. Read-only ordinary callable types
still do not satisfy Sendable. Substitution and matching spend the aggregate work
and memory budgets; recursive structural traversal is bounded to 128 levels.
The wire record includes the verified span after flags. Schema 4/contract 10 and
Program ABI 4 replace previous versions atomically, without an alternate reader.
