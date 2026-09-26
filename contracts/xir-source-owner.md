# XIR source declaration admission

The source owner reuses the real lexer/parser, module resolver and parsed module
graph. It performs its own declaration/type checks and directly constructs Built;
Checked is the only published compiler result. It never invokes the legacy
analyzer, IR producer or executor. Parsed AST storage and resolver identities are
borrowed only during construction; Checked owns its complete snapshot.

The admitted family is ordinary named functions with explicit read bool/i64/string/Atomic<i64>
parameters and explicit results (absent annotation denotes unit in this subset),
direct calls, blocks, local bindings, module bindings, returns, literals, string
concatenation, print and Atomic<i64> construction/load/fetchAdd. Integer arithmetic,
ref/move, user aggregates, reflection, coroutine syntax, attributes and
unimplemented declarations fail closed. Every function body is checked, including
unreachable functions. Ordinary generic read functions with explicit type arguments and the optional
Sendable marker follow `xir-generic-templates.md`. Unsupported constraints and
members reject in the definition, including unused templates. No ordinary generic
body is instantiated by duck typing.
Calls and print use owned function operand tables, admitting up to 65536
arguments within metadata, work, parameter and physical frame budgets. Arguments
are evaluated left to right before appending the owning instruction range; nested
calls cannot interleave ranges. This does not qualify unimplemented source types.

Top-level functions are hoisted; top-level executable statements and binding
initializers execute in source order once per instance. A function named main is
ordinary and never automatically invoked. A private unit initializer represents
each module, and a separate synthesized i64 entry returns zero after initialization.
Imports bind exact resolver-owned module identities. Cross-module calls require
direct imports and exported declarations. Duplicate names, unresolved/private
imports, cyclic dependencies and mutable library-module state are rejected.
Library const Atomic<i64> handles are Sendable shared identities within one
instance; different instances allocate independent objects. Root mutable bindings
remain instance-private. Only owning initializers publish global slots.

Source string literals are already decoded by the parser and are never decoded a
second time. Source literal NUL rejection remains the current lexical contract;
runtime strings still support NUL. Print follows the output-group contract.
Atomic construction infers i64 from the admitted argument; load and fetchAdd use
default SeqCst ordering, fetchAdd returns the prior value and wraps as specified.
Unsupported overloads/orderings are rejected rather than silently ignored.

Standard-library source submodules use the explicit std/<namespace>/<path>
specifier, with ASCII identifier path segments and no extension, index fallback,
parent traversal or alias to the namespace's existing top-level entry. Resolution
requires an explicit compiler-owned stdlib source root and a registered namespace.
The canonical physical file must stay in that root and match the requested logical
path. Its durable identity carries the stdlib namespace and root-relative path;
package or local identities never gain standard-library privileges from filenames.

The io/output.xr submodule exports writeStdout(string)->bool and
writeStderr(string)->bool over the host typed-output capability. Only its exact
stdlib identity admits the private __writeStdout/__writeStderr primitives. They
require one read string and lower to WRITE_STREAM; ordinary local declarations
still shadow primitive names. Imports require exported source functions, so users
cannot import a private primitive. All wrapper bodies are checked normally.
This capability reports provider acceptance; it neither opens File handles nor
promises filesystem flush/short-write behavior. Existing yieldable File operations
require separate migration. Source loading is an implementation step toward the
same-source Checked/native package, not serialized/cache publication qualification.

This admission work does not qualify complete parser OOM recovery, source input
budgets, stdlib publication, default CLI migration, foreign output providers,
ordinary generics or product cutover. The reused parser currently contains fatal
allocation checks; those remain explicit qualification debt and must be repaired.

verification-test: test_xir_source
verification-test: test_xir_source_native
verification-test: test_xir_source_mixed
verification-test: test_xir_source_admission
verification-test: test_xir_source_allocations
