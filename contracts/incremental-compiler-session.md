# Incremental compiler session ownership

`XrCompilerSession` is the only mutable owner for one compiler workspace. It
owns its dependency graph snapshot, bounded invalidation history, optional
cache-store handle, configuration paths, persistent analyzer state, and
generation counters. Two sessions never share mutable semantic or graph state;
they may share only independently verified immutable cache blobs.

Dependency graph publication deep-copies a verified graph and is atomic.
Invalidation uses the independent graph/result verifier before replacing the
owned graph, retains at most the fixed history limit, and advances the
workspace generation only after successful publication. Rejected graph or
invalidation transactions leave the prior graph, history, and generations
unchanged.

The native build publishes that graph once per build, after every module owns a
verified SemanticPlan and before target planning. Its nodes are module
summaries derived from verified authorities alone, and its edges carry the
conservative observation relation, because facet-precise relations would need
facet-granular plan digests that the semantic layer does not publish. A summary
that cannot be derived exactly fails the build instead of publishing a partial
graph.

Parser and lowering state is scoped to an explicit incremental operation.
Every non-REPL source/AST compiler entry opens or borrows a generation-bound
operation scope. The outermost caller alone commits success; a nested failure
poisons the operation, and the owner aborts only after graph and analyzer
cleanup has released borrowed views. Bundle compilation is therefore one
operation regardless of module count.

Every REPL submission reserves a distinct monotonic declaration generation
before parsing. Successful execution publishes that generation with its exact
parent and statement count. Parse, compile, and runtime failures retain an
immutable abandoned record while leaving the last published generation
unchanged; their identities are never reused. Only one declaration generation
may be active, and graph publication, invalidation, cache installation, reset,
or a non-REPL compiler operation cannot overlap it. The reservation allocates
its ledger slot before execution, so successful runtime mutation cannot be
followed by an allocation-dependent publication failure. Snapshot and record
accessors return detached values instead of mutable ledger pointers.
Incremental graph reset does not clear this ledger or rewind its next identity;
only destruction of the owning compiler session ends the declaration history.

The declaration ledger is the authority prerequisite for immutable typed REPL
generations. The transitional VM still stores REPL bindings in its name-keyed
global dictionary; this contract does not claim typed layout ownership,
closure/task pinning, or replacement of that runtime representation. REPL
program arenas remain retained because the persistent analyzer owns references
into prior declarations. REPL submissions must not borrow the transient source
operation contract as a substitute for this declaration-generation boundary.
Cancellation and fatal failure clear every transient view, advance the session
generation so stale arena scopes cannot restore abandoned state, and preserve
the last fully published graph and cache objects. Full reset clears the graph,
history, counters, transient state, and logical memory watermark while keeping
the configured cache-store handle open. Idle cleanup is unavailable during an
active operation and only removes owned invalidation history.

The logical memory watermark is an overflow-checked accounting metric for
owned graph capacity, canonical module keys, invalidation records, and reason
evidence. It is not process RSS and cannot be used as a performance completion
claim without the separate cold/warm/edit benchmark evidence.

The optional cache store is installed exactly once, either while constructing
the session or before its first active operation. Installation advances the
configuration generation. Replacement and installation during an operation
are forbidden, while each cache load or publication supplies its own exact
artifact authority and operation-local verifier output.

## Required evidence

- The compiler-session focused test proves deep-copy ownership, session
  isolation, exact invalidation publication, cancellation/fatal recovery,
  stale-scope rejection, bounded history, watermark/reset behavior, and
  cache-handle lifetime. It also proves declaration generation ordering,
  non-reuse, exact parents, overlap rejection, forged-scope rejection, and
  published-versus-abandoned accounting.
- The REPL focused test proves that a real successful submission publishes,
  a rejected submission abandons without replacing its parent, and the next
  successful submission receives a fresh generation while preserving prior
  value visibility.
- Dependency graph and cache-store focused tests remain mandatory; the session
  does not weaken their independent validation contracts.
- Runtime-only installed symbol gates must continue excluding compiler-session
  and cache-builder APIs.

## Digest anchors

anchor-sha256: src/toolchain/xcompiler_session.h 8dee7c7df5115c2af9f48015dcce11ef6341830de07c06c3b0b5febcce4d8fde
anchor-sha256: src/toolchain/xcompiler_session.c 5c15ee43fadd472cc0f2e7c577e214f5133360b4930e686b5df29228311ab017
anchor-sha256: src/api/xrepl.c 2317cdfb203e2c7c57dd9b1b8409fbf434d4d3eadc9386a9f1ad7ed485ade1ab
anchor-sha256: tests/unit/toolchain/test_compiler_session_generation.c 4b0d4aad37518ed2e90e12ac99df88f6afc7e14c3bf54ac10f5575cc19afffd5
