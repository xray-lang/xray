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

Parser and lowering state is scoped to an explicit incremental operation.
Every non-REPL source/AST compiler entry opens or borrows a generation-bound
operation scope. The outermost caller alone commits success; a nested failure
poisons the operation, and the owner aborts only after graph and analyzer
cleanup has released borrowed views. Bundle compilation is therefore one
operation regardless of module count. REPL submissions retain declarations
across inputs and require a separate declaration-generation transaction; they
must not borrow this transient operation contract as a substitute for rollback.
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

## Required evidence

- The compiler-session focused test proves deep-copy ownership, session
  isolation, exact invalidation publication, cancellation/fatal recovery,
  stale-scope rejection, bounded history, watermark/reset behavior, and
  cache-handle lifetime.
- Dependency graph and cache-store focused tests remain mandatory; the session
  does not weaken their independent validation contracts.
- Runtime-only installed symbol gates must continue excluding compiler-session
  and cache-builder APIs.

## Digest anchors

anchor-sha256: src/toolchain/xcompiler_session.h 9c3fc0fcd1b47fe967d1dc3b55c55eb1fa97e1145dc2d9679d4b1d96f2ec2801
anchor-sha256: src/toolchain/xcompiler_session.c 7b36ae71943d07f48227a33a734069a774e3e7d861907f884a61cba754450989
anchor-sha256: tests/unit/toolchain/test_compiler_session_generation.c a5b9cebe2e332403e2086f0250ff31cdba42f9679ef3ec9768c921e871a31b6e
