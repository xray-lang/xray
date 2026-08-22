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
8. Core prelude source bindings have stable numeric IDs in the core intrinsic
   registry. That registry owns source identity and typed call facts only;
   executable Xi behavior remains owned by the semantic operation registry.
9. `assertThrows` observes the typed-error channel and `assertPanics` observes
   the panic channel. Their expected channels are distinct typed facts and may
   not be inferred from display names. Grouped heterogeneous `print` output is
   direct-call-only because it requires one call-site plan.
10. Source-level branch-probability wrappers are deleted capabilities. Their
    spellings and former Xi copy variants remain only in governed negative
    evidence; compiler and runtime `XR_LIKELY` / `XR_UNLIKELY` macros are
    implementation details outside the source intrinsic surface. The deleted
    spellings are not reserved: user declarations with those names are ordinary
    functions and carry no compiler-owned branch-probability semantics.

## Digest anchors

anchor-sha256: src/frontend/analyzer/xa_intrinsic_registry.def 3acea091929e3d062fa5183ac6b219dddb1a7df34baa8607b7fac8d5f52b6872
anchor-sha256: src/shared/xr_core_intrinsic.def d40802b53e3333eee9cd18fbbf9680e79c9ae5dd903770f799e0ef69c1805baa
anchor-sha256: contracts/capability-deletions.tsv 0ce3ca872d9dafa777f75f8540cc92244edb082615b9733534e418afe2d40449
anchor-sha256: scripts/check_branch_hint_surface_residue.py 1e1950f0e6bcd58b96d56b35bf8230e95b32ee0915cf62ad10ce5b70589b99aa
anchor-sha256: tests/regression/05_functions/0582_removed_builtin_names_reusable.xr 037941a5256838f24279cf536a83ccbbaa84af5dde8bc386343addc97b849c49
