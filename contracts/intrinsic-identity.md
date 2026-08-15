# Intrinsic identity contract

Status: re-frozen after the xxHash AVX2 work added stable fixed-width
`U32x8`/`U64x4` construction, widening multiply, bitwise, shift, and
reinterpretation identities without reassigning the existing registry.

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
6. Target-mode queries such as `simd.Capabilities.isRuntimeSelected()` have
   their own stable identity; they are not inferred from `nativeBytes()` or
   from a backend helper spelling.
7. `Array.reserve` has the stable `core.array.reserve` identity. Its mutating
   receiver, capacity operand, receiver-alias result, VM opcode, and AOT effect
   statement are selected from that identity and the frozen plans; selector
   text is not retained as a second authority.

## Digest anchors

anchor-sha256: src/frontend/analyzer/xa_intrinsic_registry.def 3acea091929e3d062fa5183ac6b219dddb1a7df34baa8607b7fac8d5f52b6872
