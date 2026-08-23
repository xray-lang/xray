# Backend-neutral semantic ownership contract

Every observable operation in the generated `semantic-owner-registry.json`
has exactly one canonical semantic owner, a stable 128-bit operation ID, a
stable 128-bit owner ID, and at least one declared production consumer. The
registry is generated from `xisa/xi/ops.def`; it is not a second planner or a
backend-owned name table. Unknown, unowned, or multiply owned operations,
duplicate stable IDs, missing consumer bindings, and stale fingerprints fail
closed during generation or contract verification.

Native and tagged implementations may use different representations, but
adapters may only marshal representation, ownership, errors, and ABI state.
They must not duplicate observable rules. VM, AOT, CGen, and runtime bindings
for a migrated shared owner consume its generated stable owner ID. CGen adapter
spelling is obtained by stable-ID lookup rather than reconstructed from an Xi
or source-language name.

Bounds semantics have one real operation family: canonical `xi.index.get` and
`xi.index.set`, whose generated VM and AOT consumers perform the access. The
retired `xi.bounds.check` row had no source-lowering producer and no executor
handler, so it is absent from the owner registry and target-machine inventory;
no alias, opcode hole, or alternate bounds owner remains.

The generated shared-core inventory records signatures, production callers,
test callers, representation class, and applicable profiles for every
`src/shared/xr_*_core.h`. A new or removed core, caller drift, a native kernel
that gains `XrValue`, loss of either production `Array.sort` consumer, or
revival of a retired private sort implementation fails the gate.

`shared.truthiness` is the first production owner migrated through this
registry. Its representation adapters classify null, bool, integer, float,
sized, and opaque-object values; the observable decision remains exclusively
in `xr_truthy_core_eval`. Removing a declared production binding, changing its
stable ID marker, or reviving a private truthiness decision fails the owner
ratchet.

`shared.assert-condition` owns canonical `xi.assert` after truthiness has been
classified. VM, hosted AOT, freestanding AOT, and CGen consume the same
expected-true/expected-false failure decision. Error construction, diagnostic
printing, abort, and trap transport remain backend adapters; `assert_eq`,
`assert_ne`, and `assert_throws` remain outside this owner.

`primitive.type-identity` owns canonical `xi.typeid`. VM, runtime, hosted AOT,
freestanding AOT, and CGen consume the generated owner ID and declared consumer
bit; representation adapters only classify local value tags into the shared
public TypeId core. CGen resolves `xrt_typeof_id` from the generated owner table,
and the legacy `typeOf` source-name dispatch has no remaining production path.
The generated registry, low-level owner header, SemanticPlan row, and
machine-readable fingerprint are all derived from the same `ops.def` row.
Restoring a source-name/literal adapter guess, borrowing another consumer bit,
or losing any of the six production bindings fails the owner ratchet.

`shared.range` owns the start, end, step, boundary mode, length, membership,
index, and display rules for `xi.range`. VM and AOT runtime code only allocate
and expose their local Range representation; CGen resolves
`xrt_range_semantics` by stable owner ID before calling that allocation
adapter. Negative indexing is rejected, inclusive and half-open boundaries are
decided in the shared core, and the INT64 edges are covered by the owner KAT.
Restoring backend-local Range formulas, raw construction helpers, or a literal
CGen adapter binding fails the owner ratchet.

`shared.bitwise-binary` owns `xi.band`, `xi.bor`, and `xi.bxor` for signed
64-bit patterns and arbitrary-precision BigInts. BigInt sign-magnitude storage
is converted according to unbounded two's-complement rules inside the shared
kernel; result sign and normalization are owner decisions. VM, optimizer,
hosted and freestanding AOT, CGen, and runtime code only adapt tags, allocation,
errors, and emitted representation. Per-operation BigInt helpers, operator
overload fallback, raw optimizer folds, or raw CGen `&`, `|`, and `^` emission
fail the owner ratchet.

`shared.shift` owns `xi.shl` and `xi.shr` for scalar and BigInt values. Scalar
counts use modulo 64, left shift wraps, and right shift is explicitly signed or
unsigned. BigInt counts are validated without modulo reduction and limb
planning/application remains in the same owner. VM, optimizer, hosted and
freestanding AOT, CGen, and runtime code may only adapt representation,
allocation, and errors; reviving private shift helpers or raw CGen shift
emission fails the owner ratchet.

`shared.numeric-neg` owns `xi.neg` across signed 64-bit integers, IEEE-754
floats, and normalized BigInts. Integer negation wraps at `INT64_MIN`; floating
negation toggles only the sign bit and therefore preserves signed zero and NaN
payloads; BigInt zero normalization and sign inversion come from the shared
plan. VM, optimizer, hosted and freestanding AOT, CGen, and runtime code may
only adapt value representation, allocation, and errors. Operator overloads,
raw C unary negation, private constant folding, and backend-local BigInt sign
rules are forbidden by the owner ratchet.

`shared.codegen-compiler-fence` owns the compiler-order edge of
`xi.codegen.compiler-fence`. The shared plan preserves program state, declares
no runtime memory effect, and requires a native provider to block memory
reordering across the boundary. VM bytecode projects this to a void value
because no native optimizer exists there; CGen emits the selected provider's
compiler fence. Neither executor may reinterpret the fence as a runtime memory
operation, drop the native ordering edge, or bypass the stable owner ID and
exact plan check.

Higher-order array operations retain the existing native typed fast path for
pure uncaptured callbacks. Captured, dynamic, generic, cross-module, and tagged
forms follow the explicit matrix in `hof-shape-matrix.toml`; unknown target or
effect evidence cannot be guessed inside CGen.

`shared.target-layout-query` owns `xi.target.sizeof` and `xi.target.alignof`.
The owner evaluates a frozen `XrTargetDataLayout` plus the native scalar tag;
VM only materializes the result and CGen emits the already-decided integer.
Generated C may not ask the host compiler to rediscover target layout through
`sizeof` or `_Alignof`, and invalid layout, query, or type inputs fail closed.

## Digest anchors

anchor-sha256: contracts/semantic-owners.toml da59d22dc919a7ba21a1b5370cf27a70b06ef88df4314480fa64b45edb40fba8
anchor-sha256: contracts/semantic-owner-registry.json 6bb58be4ddfd0fa5aa3f2aa7711b2e73965420c6932f1dc0b66a5da2b44d783d
anchor-sha256: contracts/hof-shape-matrix.toml e64c5c47454ee0ab56b28086cdded0dd7e962d89cc6bf72b37ba2677a715fbf7
anchor-sha256: contracts/shared-core-inventory.json 3b6619b7e186a5aa6c7a00a22e0cc1c0862b9d78e356c9911bc75236eb03c55f
anchor-sha256: src/shared/xr_semantic_owner_ids_gen.h f8b0f54d104bba0113daf72209377d43f4bb6ba1db9281227e063225bfdf9186
anchor-sha256: scripts/check_semantic_owners.py e4f615d91b5874e218059055190c689ae7831261005657dbd228a89b9d8e2ca3
