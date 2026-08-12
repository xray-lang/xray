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

anchor-sha256: src/toolchain/xcompiler_session.h bccc06c994cc8b9da45ce633612625c6d2892e168a083b8bf9d1e0bce368e836
anchor-sha256: src/toolchain/xcompiler_session.c 42ee1cca17f9282c6694d6712b05c1a81bfdb8ed314bec2be3bed7fee1d87b8d
anchor-sha256: tests/unit/toolchain/test_compiler_session_generation.c dcf20d560fbc97f7ca658a7082f6ab7173659972d168e94710f7e7e5baf6ac31
