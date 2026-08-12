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

Higher-order array operations retain the existing native typed fast path for
pure uncaptured callbacks. Captured, dynamic, generic, cross-module, and tagged
forms follow the explicit matrix in `hof-shape-matrix.toml`; unknown target or
effect evidence cannot be guessed inside CGen.

## Digest anchors

anchor-sha256: contracts/semantic-owners.toml e01f67078d6b0f7e856972edb66839f6511c9169cea27c18bda7d1497f8911c0
anchor-sha256: contracts/semantic-owner-registry.json bc186ca20bab6a32b6b910ca3c58724a525a8e2ceeb5eb41de64ae06eb975217
anchor-sha256: contracts/hof-shape-matrix.toml e64c5c47454ee0ab56b28086cdded0dd7e962d89cc6bf72b37ba2677a715fbf7
anchor-sha256: contracts/shared-core-inventory.json 079cffccae81c6161cdf3f6eadb974b32366e6a866ca961d7e19530c9c9a660a
anchor-sha256: src/shared/xr_semantic_owner_ids_gen.h 591ba3da92b0e036ac441d33c6cd01e38eb54e6ecbe2c1297d03c649cf546482
anchor-sha256: scripts/check_semantic_owners.py c10d76ef2c6506830a2f4567644a9e59f2de67fd5c7d37ab2d865ba528ebee24
