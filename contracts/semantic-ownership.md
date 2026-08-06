# Backend-neutral semantic ownership contract

Every observable operation named in `semantic-owners.toml` has exactly one
canonical semantic owner. Native and tagged implementations may use different
representations, but adapters may only marshal representation, ownership,
errors, and ABI state. They must not duplicate observable rules.

The generated shared-core inventory records signatures, production callers,
test callers, representation class, and applicable profiles for every
`src/shared/xr_*_core.h`. A new or removed core, caller drift, a native kernel
that gains `XrValue`, loss of either production `Array.sort` consumer, or
revival of a retired private sort implementation fails the gate.

Higher-order array operations retain the existing native typed fast path for
pure uncaptured callbacks. Captured, dynamic, generic, cross-module, and tagged
forms follow the explicit matrix in `hof-shape-matrix.toml`; unknown target or
effect evidence cannot be guessed inside CGen.

## Digest anchors

anchor-sha256: contracts/semantic-owners.toml 09e062fa263e635d3c90b55173a74ff18f8f62458a50a1e6dd4a49582168f930
anchor-sha256: contracts/hof-shape-matrix.toml e64c5c47454ee0ab56b28086cdded0dd7e962d89cc6bf72b37ba2677a715fbf7
anchor-sha256: contracts/shared-core-inventory.json e1074b75d714c46700d67ef0a2f143f6d0e3e57eb2028873b233fcfe81ef1744
anchor-sha256: scripts/check_semantic_owners.py b6857bde8312047cee53fccfa52b22920d9cc39bf95da4634aadb68c8f3336cd
