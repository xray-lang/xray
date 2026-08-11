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

Higher-order array operations retain the existing native typed fast path for
pure uncaptured callbacks. Captured, dynamic, generic, cross-module, and tagged
forms follow the explicit matrix in `hof-shape-matrix.toml`; unknown target or
effect evidence cannot be guessed inside CGen.

## Digest anchors

anchor-sha256: contracts/semantic-owners.toml 887061580506cd04d7f8cc66f4bde64e66e7c3d20fbdaa36d14b568daa7d1572
anchor-sha256: contracts/semantic-owner-registry.json 7be9ccb6affbb2c0bf8e01f8fbe9f3c24bcd6c05f05c15d2d25dc63994a86aa7
anchor-sha256: contracts/hof-shape-matrix.toml e64c5c47454ee0ab56b28086cdded0dd7e962d89cc6bf72b37ba2677a715fbf7
anchor-sha256: contracts/shared-core-inventory.json ee6fa9bf48c9d14a16024df8bf82ba7944324b5db32880c209ba85ecf60564e9
anchor-sha256: src/shared/xr_semantic_owner_ids_gen.h 768661dce4cce2e2341ce2cc789c4b1b12d782f789fbe0009cc1b41bb445c21d
anchor-sha256: scripts/check_semantic_owners.py 6e18c609fd21bb0e2046a5bbbd76fa515af70a9ecc6b66eebb2581be28ae4786
