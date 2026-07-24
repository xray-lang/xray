# Intrinsic identity contract

Status: re-frozen by task 237.

1. Each compiler intrinsic has one canonical, stable numeric identity in the
   registry. The identity, not a spelling, import path, method name, or emitted
   C helper name, selects intrinsic semantics.
2. Renaming or moving a source-level API does not silently allocate a new
   semantic identity. Removing or reassigning an identity is a contract change.
3. Analyzer, Xi lowering, optimization, AOT, and VM consumers may carry the
   canonical identity forward; they must not reconstruct it from strings.
4. A registry change must migrate registry uniqueness tests, affected Xi/AOT
   filetests, backend differential cases, and dependent ports evidence.
5. Boundary-only intrinsics such as `mem.assumeInitialized<T>` are sealed
   compiler identities. A library declaration or same-spelled user function
   cannot acquire their proof authority.

## Digest anchors

anchor-sha256: src/frontend/analyzer/xa_intrinsic_registry.def d55ba4d3dc2aaf3097aa7ccd04399842752f02edd0517eba173fd8f1ae6a82a7
