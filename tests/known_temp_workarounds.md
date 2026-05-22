# Known temporary engineering workarounds

> Tracking table for every `DEFENSIVE-TEMP[NNN]` tag in the source tree.
>
> Lives under `tests/` because `docs/` is intentionally untracked
> (`.git/info/exclude`); this file is a CI-enforced engineering
> contract, peer to `tests/known_failures.txt` and
> `tests/baseline_silent_fallback.txt`.
>
> **Hard rules** (enforced by `scripts/check_temp_workarounds.sh` in CI):
> - Every `DEFENSIVE-TEMP[NNN]` tag in source code MUST have a row here.
> - Every row here MUST have at least one matching tag in source code.
> - When a tag is removed, its row MUST be removed in the same commit.
> - Every row MUST fill all eight columns. A blank cell fails the check.
> - The `Tag` column matches the trailing identifier in the source tag,
>   e.g. `// DEFENSIVE-TEMP[082]: ... Tracking row "markobject-validate"`
>   maps to `markobject-validate`.

## Table

| Tag | Location | Introduced commit | Root cause | Removal criterion | Owner | Introduced on | Target removal |
|---|---|---|---|---|---|---|---|
| markobject-validate | `src/runtime/gc/xcoro_gc.c` `xr_coro_gc_markobject` | `2e1dacc` | Conservative stack scan reads stale `{tag=PTR, ptr}` from JIT-owned vm_ctx.stack slots; reallymarkobject would then overwrite the marked byte of an unrelated live header | Precise safepoint stack map for the JIT plus a uniform cross-tier pointer-trust contract; once neither path can hand a JIT-owned slot to the conservative scan, the four shape checks become unreachable and can be reverted to a `XR_DCHECK` shape assertion | xingleixu | 2026-05-17 | After JIT precise-stack-map and pointer-trust contract land |
| sweep-flush | `src/runtime/gc/xcoro_gc.c` `sweep_block` (entry) | `5f7a535` | JIT inline allocator bumps `immix.cursor` without writing `block->alloc_marks`; deferred-flush call sites were originally scattered across allocators and the sweep side was missed, so `sweep_block` could rebuild alloc_marks treating live lines as free | Replace the deferred alloc_marks mechanism with eager BTS at allocation (one store per allocation, no reconciliation phase). Removing the eager marks API also removes this flush call by definition | xingleixu | 2026-05-16 | After the eager-alloc-marks GC refactor lands |

## Adding a new entry

When you introduce a new `DEFENSIVE-TEMP[NNN]` tag:

1. Tag the code with the standard block format used by the existing
   three rows. The first line MUST be exactly:

   `// DEFENSIVE-TEMP[NNN]: <one-line summary>.`

   followed by   `Tracking row "<tag-id>" in tests/known_temp_workarounds.md.`
   line. Pick a stable `<tag-id>` you will use for years; do not use
   issue numbers or stage names.

2. Add a row to the table above with all eight columns filled.
   The reconciliation script keys on the `<tag-id>` string; rows whose
   identifier does not appear in any source file fail the check, and
   tags whose identifier does not appear here also fail.

3. Run `scripts/check_temp_workarounds.sh` locally before pushing.

## Removing an entry

When the removal criterion is met:

1. Delete the `DEFENSIVE-TEMP[NNN]` block in source code.
2. Delete the matching row in this file in the same commit.
3. The reconciliation script will pass on the next CI run; if it fails,
   one of the two artifacts is missing and the commit is incomplete.
