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

## Formatting

There is no pre-commit hook. One used to run `clang-format -i` over staged
files; it was deleted because it rewrote files after the commit had been
composed, which staled contract anchors immediately and handed every branch a
reformat of lines it never wrote. Formatting is checked, not applied.

Two checks, because the tree carries real debt. The hook only ever formatted
files somebody had staged, so a file nobody staged since it landed was never
formatted at all -- 335 of the 1478 non-generated sources when this was
written.

```sh
# Your changed lines must satisfy .clang-format. This is the rule for new code.
scripts/check_format.py --mode changed --base origin/main

# ...and to fix them without reformatting anything you did not write:
scripts/check_format.py --mode changed --base origin/main --fix
```

```sh
# The whole tree against tests/format_debt.txt, which only shrinks. A file that
# is unformatted and unlisted fails; a listed file that is now formatted also
# fails, so a fix deletes its line in the same commit.
ctest --test-dir build --output-on-failure -R format_debt_ratchet
```

The tree formats with clang-format 22 and only 22: two releases do not always
agree, so a check that takes whichever binary `PATH` surfaces first reports
drift that exists only between the two binaries. Install it with
`brew install clang-format`, `apt install clang-format-22`, or
`pip install 'clang-format~=22.1'`.

To clear a debt entry, run `clang-format -i` on the file and delete its line,
in a commit that changes nothing else so review can skip it.

## Test discipline

Two suppression channels exist, and both expire. Adding to either is a decision
that gets a name attached to it.

- `tests/known_failures.txt` -- individual disabled tests. Every active line
  carries `ISSUE=`, `ADDED=YYYY-MM-DD` and `OWNER=`, and fails CI once `ADDED`
  is more than 30 days old.
- `tests/ci_exclusions.txt` -- ctest lanes CI skips entirely. Same fields plus
  `LANE=` and `REASON=`, same 30-day window. `.github/workflows/ci.yml` derives
  its `--exclude-regex` from this file, so the manifest cannot be bypassed by
  editing the workflow.

```sh
ctest --test-dir build --output-on-failure -R 'ci_exclusion_discipline|known_failures_freshness'
```

The only-shrink ratchets are a third thing and work differently: no expiry, no
owner, and both directions gate. A failure outside the baseline fails the run,
and a baselined case that starts passing fails it too, so a fix deletes its
line in the change that fixed it. They live next to the suites they gate:
`tests/regression/baseline_failures.txt`, `tests/diff/known_failures*.txt`,
`tests/aot/filetests_known_failures.txt`, `tests/vm/parallel_vm_known_failures.txt`,
`tests/format_debt.txt`, `tests/unreachable_case_allowlist.txt`.
