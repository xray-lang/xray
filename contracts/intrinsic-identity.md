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
11. Exact scalar text parsing has four stable intrinsic identities:
    `i64.parse`, `i64.tryParse`, `f64.parse`, and `f64.tryParse`. Numeric `as`
    conversion is a different semantic family. The receiver type plus the
    stable intrinsic ID selects parsing; a selector spelling, retired scalar
    alias, or backend helper name cannot recover that authority.
12. `StringBuilder.append` has stable compiler intrinsic identity `6007` and
    stable method symbol `253`. Analyzer binding selects that identity only for
    the builtin `StringBuilder` receiver. Xi and SemanticPlan carry and verify
    both numeric identities; `append` text is diagnostic metadata projected
    from the registry and a user method with the same spelling remains ordinary.
13. The exact native module scalar call family has one stable semantic
    identity, `XR_SEM_INTRINSIC_NATIVE_MODULE_SCALAR_CALL`. It authorizes a
    stdlib member call made through an imported module namespace, and it is the
    only authority covering that shape: the sibling target-leaf family answers a
    disjoint shape -- `XI_CALL` on a member import, whose `member_name` is
    non-empty where this family requires it empty -- so no widening of the leaf
    registry can reach a namespace callsite. Removing the family, or narrowing
    it so that a namespace callsite loses its proven target, is a contract
    change and not a cleanup: the native backend refuses an entire module over
    one callsite it cannot name, which is the fault this family was introduced
    to fix. Membership is a registry fact, never a spelling: the frozen
    definition row for the module path, selector, and arity is the authority.
14. Every layer that admits the family states the call shape through the one
    require an additional term only when that term is one its own artifact
    alone can state, and the reason belongs next to the term. Classification and
    exactness are verified in both directions: an exact callsite that carries no
    mark is as much a violation as a mark on an inexact row.

## Digest anchors

anchor-sha256: src/frontend/analyzer/xa_intrinsic_registry.def 2a2b5b90b2c739a55ea51207c67ab2c613396d3e3686ce0c913a1bd79ee91865
anchor-sha256: src/ir/xi_method_sym.def 0ec1ca5390eb9be96b1a1fcfbf932787a39d6af810630f88c360538451359702
anchor-sha256: src/plan/semantic/xr_semantic_native_module_call_shape.h 35d71f47cd448b0baa0980c489b6f807c5affdf0120e746f2d4cf91b81ee5194
anchor-sha256: src/ir/xi_semantic_intrinsic.c 20a45ccefbdbda7fe02cae622bc6f64187e080ec761b288a77764473a8e86068
    shared judgement in `xr_semantic_native_module_call_shape.h`. A layer may
anchor-sha256: src/shared/xr_core_intrinsic.def d40802b53e3333eee9cd18fbbf9680e79c9ae5dd903770f799e0ef69c1805baa
anchor-sha256: contracts/capability-deletions.tsv 0ce3ca872d9dafa777f75f8540cc92244edb082615b9733534e418afe2d40449
anchor-sha256: scripts/check_branch_hint_surface_residue.py 1e1950f0e6bcd58b96d56b35bf8230e95b32ee0915cf62ad10ce5b70589b99aa
anchor-sha256: tests/regression/05_functions/0582_removed_builtin_names_reusable.xr 037941a5256838f24279cf536a83ccbbaa84af5dde8bc386343addc97b849c49