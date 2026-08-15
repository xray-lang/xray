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
and disjoint task-state rows selected by canonical task index. A callback
failure does not race downstream work into existence: all tasks already
started in that level finish, the lowest canonical failing task is reported,
and later levels remain untouched. Artifact fingerprints and diagnostics are
consumed only in canonical task order, so worker completion order is never
observable. Callbacks cannot publish shared compiler state; a caller may merge
task-local output only after the executor reports complete success.

The compiler-session module-task transaction adds the publication boundary.
It deep-copies the exact candidate graph, prepares every SCC task, and then
preflights every task-state row in canonical order before invoking any
publication callback. An authority, artifact, or diagnostic mismatch therefore
fails before publication. Publication callbacks may expose only independently
verified immutable content-addressed artifacts; mutable or generation-addressed
compiler state remains owned by the session. The candidate dependency graph
and workspace generation are installed atomically only after every canonical
publication succeeds. An I/O failure after an earlier immutable publication
may leave that object as an unaddressed cache orphan, but it is not part of the
failed session generation and a later operation must request its exact key and
run its verifier again before use. Fatal prepare, preflight, publication, or
commit failure keeps the previous graph and workspace generation.

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
  published-versus-abandoned accounting. It also proves real module-task
  execution across one, two, and all ready workers, byte-identical artifacts,
  diagnostics and canonical publication order, prepare/preflight rejection
  before publication, and publication-I/O recovery without exposing a partial
  graph generation. Its orphan probe proves that a later request revalidates
  the exact immutable artifact before the candidate graph can commit.
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
anchor-sha256: src/incremental/xr_module_task_graph.h 3347ef1fadffbddfb53b5cff0b2ee0d5ca1ad82a7630f176a4e50c497c67b667
anchor-sha256: src/incremental/xr_module_task_graph.c 265336dd0d6e422447e882c2d1bcb8f48be8d98ddcb2639b2b571bc00556cff0
anchor-sha256: src/incremental/xr_cache_invalidate.h d546845678f14a1c84a112370380b58f063f2b3a7d859b7c326ebb17c0a81c3c
anchor-sha256: src/incremental/xr_cache_invalidate.c f134e5e0ea324161a54cf4d9e352faf630ce7c27bb8c3c7aaaa2856d195ef828
anchor-sha256: src/toolchain/xcompiler_session.h 0ff91ac1eb3fbde2dffabf65b2db3cf5fed5becae55f6e10438347206f1841c0
anchor-sha256: src/toolchain/xcompiler_session.c 4c1e1523c822763b1a883bdae2bdb2968d2494a6b10067ef29d37bcd8717b5ab
anchor-sha256: src/api/xrepl.c 2317cdfb203e2c7c57dd9b1b8409fbf434d4d3eadc9386a9f1ad7ed485ade1ab
anchor-sha256: tests/unit/incremental/test_dependency_graph.c 968768a0272e1fd5d06767e55698d00449f52bee1b67b4316e1e39042eddb888
anchor-sha256: tests/unit/toolchain/test_compiler_session_generation.c 43ad2f750a17ca036dda5c09f958c6dc6878b2a1bd90caa360566a07bfa40984
