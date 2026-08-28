# The six standard-library modules without an Xray semantic source

- **Lane**: 6 (standard-library self-hosting, round 3)
- **Base**: `00f665c5c`, tree `afe293b71`
- **Branch**: `work/6-stdlib-selfhost-00f665c5c`
- **Build**: `-DXRAY_BUILD_TYPE=Release -DXRAY_STDLIB_VM_FASTPATHS=OFF`

The completion gate names six production modules whose semantic source is still
a declaration file rather than an `.xr` body:

```
$ python3 scripts/check_stdlib_full_xray_completion.py --verbose
FAIL stdlib_full_xray_source_coverage: 6 production modules without a real .xr semantic source remain
    - compress  (policy native_library,   semantic source stdlib/defs/core.def)
    - http2     (policy native_library,   semantic source stdlib/defs/core.def)
    - math      (policy native_primitive, semantic source stdlib/defs/core.def)
    - mem       (policy native_primitive, semantic source stdlib/defs/core.def)
    - prelude   (policy native_primitive, semantic source stdlib/prelude/builtin_symbols.def)
    - regex     (policy native_library,   semantic source stdlib/defs/core.def)
```

The round-3 briefing listed `test_yield` in place of `regex`. Both are real
items, but they are not the same kind of item: `test_yield` is
`public = false`, so it never enters `production` and the source-coverage gate
does not count it. It does appear in `stdlib_no_module_specific_c_loader`.

## 0. The one measurement that reframes all six

AOT is fail-closed on this base for every one of these modules, migrated or
not. Measured with the exact command the AOT filetest runner uses
(`tests/aot/run_aot_filetests.py:121-123`):

| probe | result |
|---|---|
| `import math; print(math.sqrt(81))` | `XR_TARGET_1003` selector=`sqrt` |
| `import compress; print(compress.crc32("abc"))` | `XR_TARGET_1003` selector=`crc32` |
| `import mem; print(mem.PROT_READ)` | `XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE` |
| `import time; print(time.now())` (already migrated) | `XR_TARGET_1000` |
| `import os` (already migrated) | `XR_TARGET_1000` |
| `tests/aot/filetests/link/core_compress.xr` | `XR_TARGET_1003` |
| `fn pair() -> (i64, i64)` — no stdlib at all | `XR_TARGET_1003` |
| `names.push("a")` on `Array<string>` — no stdlib at all | `XR_TARGET_1003` |
| `fn add(a: i64, b: i64)` — pure scalars | builds |

So "does AOT still work after the migration" is not a question this lane can
answer on this base, and an AOT failure after a migration is not evidence the
migration caused it. What a migration does change is *which* refusal appears:
a module that gains an `.xr` body enters the module graph and moves from
`XR_TARGET_1003` to `XR_TARGET_1000`, the multi-module program authority
blocker recorded in `blockers/a-stdlib-multi-module-program-authority.md` and
owned by lane 4.

## 1. Per-module verdict

| module | verdict | why |
|---|---|---|
| `http2` | **migrated** | 2 public symbols, both thin typed leaves. No AOT link expectation names it (`grep -rn http2 tests/aot/` is empty). |
| `compress` | see §3 | 10 public symbols; `core_compress.expect` pins direct `xrt_compress_*` emission. |
| `math` | **blocked** | public surface is int-preserving polymorphic and three `src/` tables key on the public names. See `blockers/r3-6-stdlib-math-public-surface-is-int-preserving-polymorphic.md`. |
| `mem` | see §4 | |
| `regex` | see §5 | |
| `prelude` | see §6 | |

## 2. The migration shape, restated from the modules that already did it

A module `M` is migrated when four things hold together:

1. `stdlib/M/M.xr` exists and exports the module's whole public surface. The
   path is a convention, not a registration: `CMakeLists.txt:1182` globs
   `stdlib/*/*.xr` and `scripts/generate_stdlib_embedded.py:53-58` accepts only
   `stdlib/<dir>/<dir>.xr`.
2. `stdlib/defs/core.def`'s `module M { … }` block publishes nothing. `fn x`
   becomes `fn __x` — a leading `__` is what makes an entry internal
   (`tools/stdlibgen/stdlibgen.py:593-594`) — while `vm:`, `aot:`, `argc`,
   `arg_spec`, `link_object`, `layer` and `return_ownership` stay byte-identical,
   because they name the C side. A `const` cannot be made internal by renaming
   and can only be deleted.
3. `stdlib/stdlib_boundary.toml` records `policy = "xray_semantic"`,
   `semantic_source = "stdlib/M/M.xr"` and `public_native = []`.
4. `tools/stdlibgen/stdlibgen.py` is re-run. No `*_generated*` file is edited
   by hand.

Two pins that look like they forbid step 2 but do not:

- `scripts/check_stdlib_boundary.py:139-152` requires `cluster`, `http2`,
  `compress` and `crypto` to keep a `module X {` block in core.def. Renaming
  the entries satisfies it; deleting the block does not.
- `scripts/check_binary_public_native_readiness.py:26-42` pins public-native
  sets for `io`, `os` and `net` only — all L2. None of these six is pinned there.

## 3. Reproducibility note for anyone re-measuring

Four other lanes were running `ctest -j4` on this machine concurrently. Any
timeout observed under that load has to be re-run serially before it is
attributed to a change; see the round-3 briefing §4.3.
