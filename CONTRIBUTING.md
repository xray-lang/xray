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

## Semantic contract changes

Language syntax and API shape may be replaced directly, but the semantic layer
in `contracts/` is versioned by git and guarded by anchor digests. A commit that
changes a contract or one of its listed anchors must:

1. update the affected contract text and `anchor-sha256` records;
2. include one trailer per contract:

   ```text
   CONTRACT-CHANGE: contracts/<file>.md <one-line reason>
   ```

3. state which differential cases, KATs, generated-shape gates, and ports were
   rerun, regenerated, or intentionally retired; and
4. run `ctest --test-dir build --output-on-failure -R contract_freeze` after
   committing, plus the affected semantic gates.

Implementation-only edits outside registered anchors do not need a trailer.
The gate deliberately defers trailer validation in a dirty working tree because
the final commit message does not exist yet; clean post-commit and CI runs are
fail-closed.
