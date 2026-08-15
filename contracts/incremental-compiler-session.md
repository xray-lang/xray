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

An invalidation explanation selects one exact affected module and one exact
facet. It returns the direct root reason, the root old/new fingerprints, and a
deterministic shortest evidence path from the subject back to that root.
Cycles cannot make explanation traversal diverge, and canonical evidence order
breaks equal-length ties. Malformed result rows, multi-facet queries, missing
subjects, and facets outside the verified invalidation set fail closed instead
of producing a partial reason chain.

A source module's resolved module set is not represented as an unqualified
graph-change event. Its identity is the domain-separated digest of the exact
consumer stable ID plus canonically ordered dependency stable IDs and facet
relations. A module-resolution event supplies the exact root, exact changed
facet mask, canonical delta, and old/new resolution fingerprints. Apply first
recomputes and matches the old identity, derives the facet mask from the delta,
applies the delta to a private graph, then recomputes and matches the new
identity before publication. The independent verifier repeats the same
transaction. Stale old/new identities, wrong roots or facets, malformed rows,
and partial authority fail closed without changing the graph or history.

A module task graph is an immutable derivation of one exact verified
dependency graph. Its source identity binds the complete node summaries, edge
relations, and topology. Each task is one maximal strongly connected
component with canonically ordered member identities. Component dependencies
are deduplicated, dependency-first, and topologically numbered with stable-ID
tie-breaking, so node and edge insertion order cannot affect task identity.
The task graph carries a separate structural fingerprint; relation-only,
summary-only, and topology mutations all reject an older task graph.

The generic module executor runs only one dependency level at a time and joins
the entire level before exposing the next. Workers write disjoint result rows
selected by canonical task index. A callback failure does not race downstream
work into existence: all tasks already started in that level finish, the
lowest canonical failing task is reported, and later levels remain untouched.
Artifact fingerprints and diagnostics are consumed only in canonical task
order, so worker completion order is never observable. Callbacks cannot
publish shared compiler state; a caller may merge task-local output only after
the executor reports complete success.

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
- The dependency-graph focused test proves exact-facet explanations across
  direct, multi-parent, multi-facet, and cyclic graphs, including rejection of
  forged evidence rows. It also proves consumer-exact module-resolution
  identity, insertion-order independence, exact old/new/facet authority,
  atomic mutation rejection, and a deterministic downstream explanation. It
  also proves canonical SCC/task identity and byte-identical task outputs and
  diagnostic order across one, two, and all ready workers, including multiple
  failures in one level.
- Runtime-only installed symbol gates must continue excluding compiler-session
  and cache-builder APIs.

## Digest anchors

anchor-sha256: src/incremental/xr_dependency_graph.h 549030885cabac77a90280d75cb8b619b52b19c6c4277208330f399ccb471f6c
anchor-sha256: src/incremental/xr_dependency_graph.c 1148f7a8c0f0621298c6b05afff8ab35e5b99b1af1ee62fd175d302eec2ebedd
anchor-sha256: src/incremental/xr_module_task_graph.h b51c317d2639574e53bd321b288c56c7af2238f36ae570ad45c181a00f2b07a4
anchor-sha256: src/incremental/xr_module_task_graph.c 52d8c5a718d19e2e93b4aa5a9d4352a6bd47d1a01294b326aac4707caa21cad9
anchor-sha256: src/incremental/xr_cache_invalidate.h d546845678f14a1c84a112370380b58f063f2b3a7d859b7c326ebb17c0a81c3c
anchor-sha256: src/incremental/xr_cache_invalidate.c f134e5e0ea324161a54cf4d9e352faf630ce7c27bb8c3c7aaaa2856d195ef828
anchor-sha256: src/toolchain/xcompiler_session.h 8dee7c7df5115c2af9f48015dcce11ef6341830de07c06c3b0b5febcce4d8fde
anchor-sha256: src/toolchain/xcompiler_session.c 5c15ee43fadd472cc0f2e7c577e214f5133360b4930e686b5df29228311ab017
anchor-sha256: src/api/xrepl.c 2317cdfb203e2c7c57dd9b1b8409fbf434d4d3eadc9386a9f1ad7ed485ade1ab
anchor-sha256: tests/unit/incremental/test_dependency_graph.c 49030af2df17db693d6db69e2d064b7eec1b716db47e095b58509c96c6dbf347
anchor-sha256: tests/unit/toolchain/test_compiler_session_generation.c 4b0d4aad37518ed2e90e12ac99df88f6afc7e14c3bf54ac10f5575cc19afffd5
