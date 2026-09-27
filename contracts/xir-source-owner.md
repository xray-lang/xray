# XIR source declaration admission

The source owner reuses the real lexer/parser, module resolver and parsed module
graph. It performs its own declaration/type checks and directly constructs Built;
Checked is the only published XIR artifact. It never invokes the legacy
analyzer, IR producer or executor. Parsed AST storage and resolver identities are
borrowed only during construction; Checked owns its complete snapshot.

## Owned source query snapshot

One source check publishes an owned result containing Checked and a read-only
semantic snapshot. The snapshot records decisions made by the same source owner;
queries never parse, infer, specialize, check, execute, or grant visibility. It is
not another executable IR. Neither result retains AST nodes, source-name objects,
parser arenas, the compiler session, or the construction context. Either owned
component may outlive the other, the session, and the source files.

Declaration IDs are nonzero indices stable within that snapshot. They are not
portable across edits, checks, or sessions. Module indices refer to owned durable
canonical identities and available source-content fingerprints; physical paths are locators
only. A query-only native source module owns its generated LF content fingerprint
and source path. A core registry module may expose only its stable identity: null
path, zero coordinates and zero fingerprint explicitly mean unavailable source
provenance, not a verified source-content digest. A reference preserves both the lexical declaration (including an import
alias) and the resolved target. Captures preserve the original binding identity.
Function declarations expose their checked signature; their type field is the
result type. Other typed declarations expose their value type. A type carries its
generic declaration owner: equal parameter ordinals in two functions do not mean
equal type identities. Constructed IDs refer only to the snapshot's owned copy
of the same unified type pool, interpreted under that generic owner. A node's
kind distinguishes CALLABLE, ARRAY and internal CELL; an ID range alone cannot
identify a callable. Callable parameter payloads are independently owned too.
Source interning places child nodes before parents. Mutable generic captures
preserve the original lexical owner even when their shared CELL node is reused
by another declaration with the same parameter ordinal.

Governed native type, type-parameter, member and core-intrinsic records use
distinct declaration kinds. The native identity field is interpreted under that
kind and parent declaration. A generic type constructor is not an ordinary value
type. Member signature text is independently owned; abstract element type facts
carry the schema declaration owner, not the calling function's T owner. A direct
core length query exposes its i64 result and one abstract operand with unknown
ordinary value type; it does not masquerade as an executable first-class callable.
Successful type uses and calls record resolved identities only after admission.

Source ranges use 1-based lines and UTF-8 byte columns, with exclusive ends.
Zero coordinates mean unavailable, never a guessed location; parameter names use
their own parser positions. These coordinates are not LSP UTF-16 positions.
Source AST IDs, when present on expression facts, are snapshot-local only.

Success publishes both Checked and a complete snapshot. A non-resource source
failure publishes no Checked and may publish an explicitly incomplete snapshot
with its first diagnostic and only facts established before failure. A declaration
without a successfully checked type is explicitly untyped. Facts in an incomplete
snapshot do not certify an entire declaration body or program. Parse/graph
failures and unsuccessful or incomplete resolver identities publish no snapshot because those reused boundaries do not distinguish
all allocation failures from invalid input. This does not qualify parser
or IDE recovery. OOM or budget exhaustion publishes neither component, including
when allocation fails while copying query metadata. Snapshot storage is charged
to the same source metadata/work budgets. Result destruction releases both owned
components and clears the result; transferring a component requires clearing its
field before destroying the remaining result.

The admitted family is ordinary named functions with explicit read bool/i64/string/Atomic<i64>
parameters and explicit results (absent annotation denotes unit in this subset),
direct calls, blocks, if/else, while/for, unlabelled break/continue, bool short-circuit/not and
grouping, variable compound assignment, mutable i64 increment/decrement statements, typed mutable local places,
module bindings, returns, literals, concrete i64 arithmetic and signed comparisons, string
concatenation, print and Atomic<i64> construction/load/fetchAdd. The integer family
follows `xir-integer-arithmetic.md`. Other numeric families,
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
the complete generic language surface or product cutover. The reused parser currently contains fatal
allocation checks; those remain explicit qualification debt and must be repaired.

verification-test: test_xir_source
verification-test: test_xir_source_native
verification-test: test_xir_source_mixed
verification-test: test_xir_source_admission
verification-test: test_xir_source_allocations
verification-test: test_xir_source_query
verification-test: test_parser_recoverable
verification-test: meta_ownership_inventory

Structured control and local place semantics follow `xir-local-control-flow.md`.

The source Array family follows `xir-array-source.md`: annotations, contextual
literals, ordinary value copies, indexed reads/writes, governed get/set/push,
and the stable global len identity use this same owner and the same
Built/Checked/specialization/Lowered pipeline. Native inventory members without
an admitted operation remain explicit errors. Root module slots and captured
mutable cells produce logical places; read parameters and const bindings cannot
grant mutation authority. No separate source checker or executable IR is added.

The unshadowed compiler namespace Coro admits only Coro.yield() in this source
family. It accepts no value/type arguments and returns unit. A lexical/module
binding named Coro takes precedence; the builtin is never selected through a
shadowing value, alias or similarly spelled member. Ordinary functions, generic
definitions and module initializers may call it without an async annotation.
Its Built/Checked representation is the existing SUSPEND operation, preserved
through specialization, rechecking and Lowered. An ordinary call can therefore
suspend transitively without changing type constraints or acquiring capabilities.

The effect is cooperative scheduler suspension and a cancellation boundary, not
generator value production, language throw or a blocking OS operation. The current
single-host-thread driver observes SUSPENDED and resumes exactly once with the
matching instance epoch and wake. It must not treat suspension as return, EOF or
initialization completion. Unselected source branches do not yield; both branches
are still checked. Unit return expressions execute their effect then use the
canonical valueless RETURN encoding. No explicit no_suspend/no_blocking proof,
work-stealing, generator/Task/source go or concurrent cancellation is certified by
this admission. Existing refusal of unsupported function contracts remains.
