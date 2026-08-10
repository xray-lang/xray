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

anchor-sha256: contracts/semantic-owners.toml 0576437ed30a7fee2456c2982b205ff2b04257d8e426bbc96a97be563fe1ebc2
anchor-sha256: contracts/hof-shape-matrix.toml e64c5c47454ee0ab56b28086cdded0dd7e962d89cc6bf72b37ba2677a715fbf7
anchor-sha256: contracts/shared-core-inventory.json 29835f7b6558c0def4528bc22c0e1b3f16e1d5abc01e26842722b48c082ee743
anchor-sha256: scripts/check_semantic_owners.py a6d4cc8f79ef15ea0d4155f6493c35b564607903455245d92815c2bcae56c185
