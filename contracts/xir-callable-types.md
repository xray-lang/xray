# XIR callable signature identity

Callable contracts belong to typed XIR metadata, not to an enumerated set of
implementation targets. A source function type uses the existing fn(...) spelling,
ordered parameter modes/types and its result. Missing effect promises stay unknown;
they must not be inferred from one selected body and exported as ABI guarantees.

The initial metadata family records closed read-only signatures over bool, i64,
string, Atomic<i64> and previously declared callable types. Unit is a result only.
Parameter mode zero means read; signature flags zero mean an ordinary function
with no Sendable, no_suspend, no_blocking or borrow-call promise. Nonzero modes or
flags, generic parameters inside signatures, and unsupported families fail closed
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
Specialization preserves closed signature identities and rechecks them. Checked
schema 3, semantic contract 7 appends the canonical callable table after generic
metadata; older schema/contract pairs are rejected without an alternate reader.
Wire records contain only type/mode/flag integers, never code pointers or instances.

The metadata admission alone does not certify function-value execution. Lowered
admission currently rejects a callable table until the shared runtime owner and
indirect invocation consume it. No guessed integer callee or legacy executor is
used to bypass this boundary. Function results must eventually retain required
code and an execution-admission identity without retaining all instance slots;
stopped instances cannot acquire new execution merely through an escaped value.

verification-test: test_xir_checked
verification-test: test_xir_checked_allocations
verification-test: test_xir_generics
