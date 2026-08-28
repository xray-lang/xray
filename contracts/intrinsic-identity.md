# Intrinsic identity contract

Status: re-frozen after the `math` module gained stable identities for its 35
core numeric operations, without reassigning the existing registry.

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

13. The `math` module's 35 core numeric operations have stable identities
    `7001`-`7035` in family `MATH`. `stdlib/math/math.xr` declares the public
    surface and its bodies are the VM/reference semantics; the identity, not
    the module or member spelling, is what lets AOT emit the operation
    directly. Every operand is f64: the declared signature is the whole
    contract, and an integer argument converts rather than selecting a second,
    integer-preserving meaning. `math.random` and `math.randomInt` have no
    identity — they are private native leaves, not compiler-owned operations.
    The `FREESTANDING` flag records which of the 35 emit without libm and is
    the only authority the freestanding profile consults for a math member.

## Digest anchors

anchor-sha256: src/frontend/analyzer/xa_intrinsic_registry.def b6c6cb203d39b640ecf255ac06cf50dcd77e6c6ea91d88ca28e3961b64fbba43
anchor-sha256: src/ir/xi_method_sym.def 0ec1ca5390eb9be96b1a1fcfbf932787a39d6af810630f88c360538451359702
anchor-sha256: src/ir/xi_semantic_intrinsic.c 20a45ccefbdbda7fe02cae622bc6f64187e080ec761b288a77764473a8e86068
anchor-sha256: src/shared/xr_core_intrinsic.def d40802b53e3333eee9cd18fbbf9680e79c9ae5dd903770f799e0ef69c1805baa
anchor-sha256: contracts/capability-deletions.tsv 0ce3ca872d9dafa777f75f8540cc92244edb082615b9733534e418afe2d40449
anchor-sha256: scripts/check_branch_hint_surface_residue.py 1e1950f0e6bcd58b96d56b35bf8230e95b32ee0915cf62ad10ce5b70589b99aa
anchor-sha256: tests/regression/05_functions/0582_removed_builtin_names_reusable.xr 037941a5256838f24279cf536a83ccbbaa84af5dde8bc386343addc97b849c49