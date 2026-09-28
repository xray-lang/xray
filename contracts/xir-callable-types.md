# XIR callable signature identity

Callable contracts belong to typed XIR metadata, not to an enumerated set of
implementation targets. A source function type uses the existing fn(...) spelling,
ordered parameter modes/types and its result. Missing effect promises stay unknown;
they must not be inferred from one selected body and exported as ABI guarantees.

The admitted metadata family records read-only signatures over the admitted
primitive values, enclosing-function type parameters and earlier CALLABLE,
ARRAY or NOMINAL nodes in the unified pool. Unit is a result only. ARRAY descriptor admission
does not qualify source Array syntax, runtime Array construction or operations.
Parameter mode zero means read; signature flags zero mean an ordinary function
with no Sendable, no_suspend, no_blocking or borrow-call promise. Nonzero modes or
flags and unsupported families fail closed
until their declaration contracts and implementation are admitted. This does not
remove those broader language capabilities from the roadmap.

Type IDs 256 through 65535 name nodes in the module-owned constructed pool under
`xir-constructed-types.md`. Only a node with kind CALLABLE authorizes signature
interpretation; that numeric range also contains ARRAY, internal CELL and NOMINAL nodes.
Constructed child references point strictly backwards. Exact kind, ordered
parameter contracts, flags and result identity are interned once; duplicate nodes,
unit or CELL parameters, forward/self references, invalid types and noncanonical
empty parameter payloads reject. All nodes are verified, including unused ones.
Work, parameter and metadata budgets cover pool traversal and uniqueness.
An ordinary callable has no Sendable proof, including when supplied as a generic
argument; definition checking cannot gain a constraint from a future body.

Checked transitions and packets own every descriptor and parameter record; parser
arenas, borrowed construction buffers and input bytes may be destroyed afterwards.
Specialization substitutes components, interns a fresh closed constructed pool and
remaps all type uses before rechecking. Checked schema 7, semantic contract 19
places the unified tagged pool after generic metadata; earlier schema/contract
pairs reject without an alternate reader. The exact record layout belongs to
`xir-checked-packet.md`. Wire records contain only bounded IDs and descriptor
integers, never code pointers, arena addresses or instances.

Lowered consumes this same pool for owned function values, reference construction
and indirect calls. Layout, boxed ownership, Program sealing, execution admission
and code-result leases follow `xir-function-values.md`. Target implementations are
not substituted for signature contracts, and unsupported promises remain closed.

verification-test: test_xir_checked
verification-test: test_xir_checked_allocations
verification-test: test_xir_generics

## Enclosing generic parameters

A signature's `parameter_span` is exactly one plus its largest free parameter
ordinal, or zero when closed; nested constructed nodes contribute their verified span.
Ordinals refer to the function using the type, never to an implementation body.
Every use must fit that function's declared parameter list. Unused descriptors
are still structurally checked. Global slots and a sealed Program require span
zero; a module without templates and every Lowered module reject open entries.

Definition checking compares structural call contracts under the explicit type
argument substitution. It cannot infer extra capabilities, inspect future target
bodies or revisit AST during specialization. Read-only ordinary callable types
still do not satisfy Sendable. Substitution and matching spend the aggregate work
and memory budgets; recursive structural traversal is bounded to 128 levels.
The tagged wire record carries the verified span immediately after kind, before
the callable payload. Schema 5/contract 16, Value ABI 9, Call ABI 14 and Program
ABI 9 replace previous versions atomically, without an alternate reader.

## Nominal signature components

Semantic contract 23 admits earlier nominal nodes as callable parameters and
results. Their exact declaration identity, arguments, visibility and verified
parameter span are retained through substitution and Checked revalidation.
Nominal components grant no construction or member authority. Forward/self
references and CELL components remain invalid; nominal Array components are
not enabled by this change. Closed field layout and ordinary owned transport
remain mandatory before lowering and execution. Independently checked packets,
VM and native execution must cover nominal-returning function values.
