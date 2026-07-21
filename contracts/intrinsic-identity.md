# Intrinsic identity contract

Status: frozen by task 220.

1. Each compiler intrinsic has one canonical, stable numeric identity in the
   registry. The identity, not a spelling, import path, method name, or emitted
   C helper name, selects intrinsic semantics.
2. Renaming or moving a source-level API does not silently allocate a new
   semantic identity. Removing or reassigning an identity is a contract change.
3. Analyzer, Xi lowering, optimization, AOT, and VM consumers may carry the
   canonical identity forward; they must not reconstruct it from strings.
4. A registry change must migrate registry uniqueness tests, affected Xi/AOT
   filetests, backend differential cases, and dependent ports evidence.

## Digest anchors

anchor-sha256: src/frontend/analyzer/xa_intrinsic_registry.def b8153ae9cb8609c2c8d6bfeb4b65f3ee2f39f76d92f2a829030dbf30268a8b62
