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

## Design decisions

Frozen language and implementation decisions live in the sibling `xray-docs`
repository and are enforced by ordinary tests. A change that alters such a
decision updates the owning document and the tests that carry it together, and
its commit message states which differential cases, KATs, generated-shape gates
and ports were rerun or retired. Commit messages carry no process trailers.

## Formatting

There is no pre-commit hook. One used to run `clang-format -i` over staged
files; it was deleted because it rewrote files after the commit had been
composed and handed every branch a reformat of lines it never wrote.

New and changed lines follow `.clang-format`. Many older files predate that
rule and are not reformatted piecemeal; reformatting the whole tree at once is
a separate decision. Format only the lines you touch, for example with
`git clang-format origin/main`, and never run `clang-format -i` over whole
files you did not write.

The tree formats with clang-format 22 and only 22: two releases do not always
agree, so whichever binary `PATH` surfaces first can report drift that exists
only between the two binaries. Install it with `brew install clang-format`,
`apt install clang-format-22`, or `pip install 'clang-format~=22.1'`.

## Test discipline

Disabling a test is a decision that gets a name attached to it.
`tests/known_failures.txt` lists individual disabled tests. Every active line
carries `ISSUE=`, `ADDED=YYYY-MM-DD` and `OWNER=`, and the check fails once
`ADDED` is more than 30 days old.

```sh
ctest --test-dir build --output-on-failure -R known_failures_freshness
```

The only-shrink ratchets work differently: no expiry, no
owner, and both directions gate. A failure outside the baseline fails the run,
and a baselined case that starts passing fails it too, so a fix deletes its
line in the change that fixed it. They live next to the suites they gate:
`tests/regression/baseline_failures.txt`, `tests/diff/known_failures*.txt`,
`tests/aot/filetests_known_failures.txt`, `tests/vm/parallel_vm_known_failures.txt`,
`tests/unreachable_case_allowlist.txt`.
