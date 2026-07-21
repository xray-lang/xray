# Contributing to Xray

## Compiler metadata ownership

The compiler is implemented in C, so cross-stage ownership is part of correctness rather than an implementation detail:

- R-OWN-1: names reachable from Xi, AOT bundles, plans, or global evidence are owned by the receiving arena, pool, or symbol interner; they never borrow AST/session/stack storage.
- R-OWN-2: code does not retain an element pointer across an append/grow/realloc of the same dynamic array; use an index or a value snapshot.
- R-OWN-3: aggregates crossing a compiler stage are deep-copied or ownership-transferred, never backed by shared mutable storage.

The fail-closed check is:

```sh
ctest --test-dir build --output-on-failure -R meta_ownership_inventory
```

Changes to `src/ir`, `src/aot`, or `src/analysis` also require the focused sanitizer lane:

```sh
ctest --test-dir build --output-on-failure -R asan_focused
```

Generated C is checked by the always-on W1-W4 well-formedness verifier before it is written or passed to a toolchain. A verifier failure is an internal compiler error and must be fixed at its source; there is no bypass switch.
