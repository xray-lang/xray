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

## 0. What moved

| component gate | base | after |
|---|---|---|
| `stdlib_full_xray_source_coverage` | FAIL, 6 modules | FAIL, **1** (math) |
| `stdlib_no_whole_module_native_policy` | FAIL, 6 modules | FAIL, **1** (math) |
| `stdlib_no_public_native_surface` | FAIL, 171 symbols | FAIL, **128** |
| `stdlib_native_leaf_allowlist` | FAIL, 127 leaves | **PASS** |
| `stdlib_no_handwritten_c_semantic_owner` | FAIL, 892 owners | FAIL, **844** |
| `stdlib_no_module_specific_c_loader` | FAIL, 30 loaders | FAIL, 30 |
| `stdlib_generated_c_reproducibility` | UNRUN | UNRUN |
| `stdlib_unified_target_plan_coverage` | UNRUN | UNRUN |

The two `UNRUN` gates need backend probe evidence, and the probe cannot run
while AOT refuses these modules — see §0.1.

## 0.1 The one measurement that reframes all six

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

| module | public native symbols | verdict |
|---|---|---|
| `http2` | 2 -> 0 | **migrated**, pure forward |
| `compress` | 10 -> 0 | **migrated**, four functions written in Xray |
| `mem` | 38 -> 17 | **partly migrated**; the rest is compiler-owned |
| `regex` | 26 -> 13 | **migrated surface**; the automata stay native |
| `math` | 51 | **blocked**, see `blockers/r3-6-stdlib-math-public-surface-is-int-preserving-polymorphic.md` |
| `prelude` | 0 | the gate asked it an unanswerable question; see §6 |

### 1.1 `http2`

Both entries become `__supported` / `__request` and `stdlib/http2/http2.xr`
forwards. The binding layer still applies six admission rules
(`stdlib/http2/http2_binding.c:91-163, 233-254`). None of them moved up,
because each one is either a precondition of the C code's own memory safety --
`h2_typed_headers` indexes `values` by `names->length` -- or a rule
`stdlib/http/http.xr` already owns: the 28-header cap at `http.xr:1898`, the
forbidden-header list at `http.xr:1899-1910`, the header character classes at
`http.xr:1303-1311`. Restating one of them here would give a single rule two
owners with two different failure channels, since `http.xr:2192` turns a null
into `HttpError.ConnectionFailed` while its own checks raise
`HttpError.ProtocolError`. An honest forward is worth more than a policy move
that manufactures a second owner.

### 1.2 `compress`

Four of the ten are now written in Xray, not forwarded: `crc32`, `adler32`,
`isGzip` and `isZlib`. They are byte arithmetic and two-byte header
judgements, and their C bindings are deleted in the same change, so each has
exactly one owner afterwards.

The other six hand the whole buffer to a leaf, and the reason is a language
limit rather than an algorithmic one: this module publishes compressed bytes
as `string`, and Xray builds a string from bytes only through
`string.fromUtf8` / `fromUtf8Lossy` (`src/frontend/analyzer/xnative_type_defs.inc.c:41`)
and cuts one only through `sliceBytes`, which requires scalar boundaries
(`src/shared/xr_string_core.h:151-160`). A gzip payload satisfies neither.
Measured: `fromUtf8` on a gzip payload raises `Utf8Error.InvalidUtf8`,
`fromUtf8Lossy` changes both length and content, and `gz.sliceBytes(10, n - 8)`
raises `StringSliceError.InvalidByteRange`. Already-migrated modules avoid the
same wall by not using `string` for bytes at all: `stdlib/base64/base64.xr:204`
answers `Array<u8>` and `stdlib/crypto/crypto.xr:748` answers hex.

The `.xr` body does own one thing the `.def` used to state twice: the default
compression level. Each compressor carried two `.def` entries, one per arity,
selecting two AOT entry points; a default argument says it once. The level
clamp and the container header check are **not** restated here -- both already
have an owner inside the leaf (`xr_compress_core_level_or_default` in
`src/shared/xr_compress_core.h:33`, and the `xr_is_gzip` call at
`stdlib/compress/compress.c:853`), and a second copy is a copy that can drift.

### 1.3 `mem`

Everything whose meaning Xray can state moved: the four `PROT_*` constants
(xray's own abstract bits from `src/os/os_mem.h:44-47`, translated per platform
by `prot_to_posix` / `prot_to_win`, so they are the same number everywhere) and
15 forwards.

Three groups keep their `.def` rows, and the reason is expressibility rather
than layer:

1. The layout and pointer intrinsics -- `sizeOf`, `alignOf`, `offsetOf`, `ptr`,
   `mutPtr`, `load`, `store`, `slice`, `withSliceMut`, `assumeInitialized`.
   The analyzer computes their result type from an explicit type argument and
   lowering encodes the selected `T`, the endianness and the owner evidence
   onto the backend op. Their VM entry points are deliberately unreachable
   stubs. Xray cannot spell "the caller's `T` decides my return type".
2. `Buffer` and its four methods. `StdlibNativeClassEntry`,
   `StdlibClassMethodEntry` and `StdlibTypeMethodEntry`
   (`tools/stdlibgen/stdlibgen.py:315, 329, 358`) carry no `is_internal`
   property, and `scripts/stdlib_manifest.py:117` reads it with
   `getattr(entry, "is_internal", False)`, so those kinds are unconditionally
   public and renaming cannot change that.
3. `pageAlloc`, whose two arities select two different AOT entry points
   (`xrt_mem_page_alloc_default` and `xrt_mem_page_alloc`).
   `tests/aot/filetests/link/freestanding_mem_page_hook.expect` requires both
   spellings in the generated C, and Xray has no function overloading, so
   collapsing the pair into a default argument would erase one of them.

`addr` also stays native, for a reason worth recording because it is easy to
hit again: `mem.addr` accepts a `Ptr<T>` for any `T` only because
`xa_call_is_mem_addr` (`src/frontend/analyzer/xanalyzer_visitor_call.c:670-682`)
recognises the member name `addr` and `xanalyzer_visitor_call.c:7920` then skips
the ordinary parameter-type check for slot 0. The leaf itself is declared
`(ptr: Ptr<u8>)`, so an `.xr` body calling `__addr(ptr)` with a `Ptr<T>` would
be refused. The special case survives for callers writing `mem.addr(...)`; it
does not survive the rename.

### 1.4 `regex`

Ten of the 13 module-level functions move: the `.def` entries become private
leaves and `stdlib/regex/regex.xr` publishes the names over them, with the
`compile` / `split` overload pairs collapsed into one default argument each
(the defaults are the values the short-arity C entry points already pass:
`""` at `stdlib/regex/xregex_binding.c:229`, `-1` via
`xr_regex_core_limit_arg` at `src/shared/xr_regex_core.h:102-104`).

`find`, `fullFind` and `findAll` stay native, and the reason is naming rather
than logic: they answer `RegexMatch`, and an `.xr` body cannot write that name.
Type annotations resolve through `resolve_known_named`
(`src/frontend/analyzer/xtype_ref_resolve.c:1132`) into
`xr_prelude_lookup_type`, and `stdlib/prelude/builtin_symbols.def:115` lists
`Regex` but nothing lists `RegexMatch` — it is declared only by the compiler
input `stdlib/types/regex.xr` and registered from core.def. Measured: a body
naming it fails the build with `error[E0365]: undefined type 'RegexMatch'`,
and the module-qualified spelling `regex.RegexMatch` fails the same way.

A one-line `builtin_symbols.def` entry would fix it, but that makes
`RegexMatch` visible in every `.xr` in the language, which is a public-surface
decision rather than a standard-library one, so it is left for whoever owns
that file.

`Regex` and `RegexMatch` themselves stay in `public_native` for the same
metadata reason as `Buffer`. Note that the `sys`/`net` precedent for naming a
native class inside an `.xr` body rests on `xa_register_sync_native_class_symbols`
(`src/frontend/analyzer/xanalyzer.c:1931-1939`), which is hardcoded for `sync`
alone — it is not a general mechanism.

This does not reopen task-256. That decision is about where the automata live,
and they do not move: `private_native_reason` still reads "Mature regex automata
implementation with bounded native data structures", and
`governance.regex_reassessment_triggers` is untouched. `sys` and `net` are the
precedent -- both are `xray_semantic` while keeping `native_class` rows in
core.def (`core.def:896-928` and `3084-3091`).

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

## 2.1 The one KAT these migrations move, and why every SemanticPlan KAT moves with them

`tests/unit/ir/test_xi_cgen.c:2715` froze the SemanticPlan fingerprint of a
fixture that imports nothing:

```xray
fn label(value: u64) -> string { return "value=${value}" }
print(label(7))
```

It still moved, from `9a99849f192ca8108c6ba9502a8dcc43f03f6d93251e03551d19f1df2155a02b`
to `fedfc7c88a77cdbee8134c13448dc6ad5fd571af159636eecf22df7a48eba1b4`, and the
reason is mechanical rather than incidental: `xr_semantic_plan.c:214-215` hashes
`plan->stdlib_registry_fingerprint`, which
`xr_stdlib_metadata_registry_fingerprint` derives from the whole `.def` registry.
**Any** change to `stdlib/defs/core.def` moves **every** SemanticPlan
fingerprint in the tree, whether or not the program touches the standard
library.

That is worth knowing before the next migration: a KAT that moves here is not
evidence that the program's meaning changed. It is the registry fingerprint
doing its job. `test_xi_cgen` reaches `PASS 42` before and after once this one
value is re-anchored, and it is the only 64-hex constant in that file.

The same fingerprint reaches two more places, which is why seven values in
`tests/unit/plan` moved as well:

- `xr_semantic_verify.c:5437` spells a dependency's canonical key as
  `"dependency-v1:...:semantic=<fingerprint>"` — the depended-on module's
  SemanticPlan fingerprint, written into the key — and
  `xr_semantic_verify.c:4175` then embeds that stable id in
  `"call-target-v4:...:dependency=<id>:..."`. So two **32-hex stable ids** move
  with the registry too.
- `xr_target_plan.c:840` and `:801` hash `plan->semantic_fingerprint` first, so
  the TargetPlan fingerprint and every call fingerprint follow.

The chain stops at exports: an export's key carries no fingerprint, so
`export_id` does not move. That is the direct evidence for why these cannot be
replaced blind — of the 15 frozen values in `test_semantic_plan.c`, exactly 3
moved and 12 did not.

## 2.2 The native-leaf allowlist gate

`stdlib_native_leaf_allowlist` failed with all 127 leaves unclassified, and the
inventory said why in its own docstring: `leaf_class_proposal` deliberately did
not write into the authoritative column because "no such record exists in the
current manifest schema". The missing thing was the record, not the judgement.

`stdlib/native_leaf_allowlist.toml` is that record — one entry per leaf naming
the ABI it reaches, the ownership and effect it declares (checked against the
`.def` entry rather than restated freely), the provider that implements it, and
a decidable condition under which it can be deleted. The gate now passes.

Three properties keep it from becoming a rubber stamp: a record whose leaf no
longer exists is reported as a defect, a leaf with no record stays
`unclassified` and keeps the gate red, and a class outside the closed set fails
rather than passing by not being one of the two known unapproved spellings.

## 3. The AOT link expectations these migrations invalidate, and why they are not edited here

`tests/aot/filetests/link/*.expect` pins, per case, which runtime symbols the
generated C calls. A module that gains an Xray body stops calling some of them,
so several lines in `core_compress.expect` and `core_regex.expect` state a
property the source no longer intends:

| file | lines | why they stop holding |
|---|---|---|
| `core_compress.expect` | `c_contains=xrt_compress_crc32(`, `..._adler32(`, `..._is_gzip(`, `..._is_zlib(` | those four functions are now written in Xray, so nothing calls the runtime entry point |
| `core_compress.expect` | `c_contains=xrt_compress_gzip_default(`, `..._deflate_default(`, `..._zlib_compress_default(` | the short-arity `.def` entries are gone; the default level is a default argument |
| `core_regex.expect` | `c_contains=xrt_regex_compile_default(`, `..._split(` short arity | same collapse |
| both | the `stdlib_objects` / `stdlib_symbols` lines | the leaf spellings gain a `__` prefix and the Xray-owned names appear instead |

**None of them are edited in this lane, on purpose.** Every one of these cases
already fails to build on this base — `core_compress.xr` refuses at
`XR_TARGET_1003`, and so does `core_path.xr`, which involves no module this
lane touches — so the true post-migration values cannot be derived. Writing
guessed values into a frozen expectation is the failure mode the round-3
briefing §4.3 warns about, and it would be worse than leaving a line that is
visibly stale.

There is precedent for the risk, and it is a defect rather than a licence. The
`time` migration (`d3fc9ad07`) replaced all seven `xrt_time_*` entry points with
four differently-named leaves: `src/aot/xrt_time.h` declared `xrt_time_now`,
`_clock`, `_monotonic`, `_micros`, `_nanos`, `_local_offset` and
`_local_offset_at` before it, and declares `xrt_time_realtime_nanos`,
`_monotonic_nanos`, `_cpu_nanos` and `_utc_offset_at` after it, with no name in
common. Three link expectations still call for a deleted spelling by name:

| file | line | requires |
|---|---|---|
| `core_datetime.expect` | 14 | `c_contains=xrt_time_local_offset_at(` |
| `system_datetime_offset.expect` | 11 | `c_contains=xrt_time_local_offset(` |
| `runtime_time_symbol.expect` | 9 | `c_contains=xrt_time_now(` |

None of the three can be repaired by substituting a surviving leaf, because the
migration also changed how a leaf is reached: the four leaves route through the
generated stdlib method table instead of a recognized-name direct call, so the
post-migration C may name no `xrt_time_*` function at these sites at all and
assert an `xrt_method_N(` instead. The `stdlib_objects` and `stdlib_symbols`
lines are in the same position rather than merely suspect.
`core_datetime.expect:4` names `time.localOffsetAt`, which is now an `export fn`
in `stdlib/time/time.xr:44` forwarding to the `time.__utcOffsetAt` leaf; whether
the plan records the export, the leaf, or both is a question for the dump and
not for reading the source.

Nor can these cases be built to find out, and the refusal is not the
`XR_TARGET_1003` this section opens with. `core_datetime.xr` refuses at
`XR_TARGET_1000`: `import datetime` draws in `stdlib/datetime/datetime.xr`,
which imports `time`, so the entry is a three-module graph, and
`src/aot/xaot_driver.c:2554` admits a multi-module product only when a source
program closure was published. Both publishers are scalar-only, so a graph
carrying `DateTime` does not qualify.

None of the three are listed in `tests/aot/filetests_known_failures.txt`, and
they are deliberately not added there either. Measured on this base with
`build-nofp/xray` (`-DXRAY_STDLIB_VM_FASTPATHS=OFF -DXR_STDLIB_FROM_FILE=OFF`),
the whole `link` mode reports `8 passed, 244 failed, 0 skipped`, and 124 of
those failures are the same `dump command failed` as `core_datetime`, against an
empty ratchet baseline that leaves all 244 untracked. A line for one case would
not make the suite green, and would file a branch-wide fail-closed state as a
per-case allowance; the baseline's own rules say the list may only shrink and
that a line may never be added to make an unrelated change go green.

`contracts/stdlib-symbol-inventory.json` carries the same drift for the same
reason: its `time` entries still bind `localOffset` and `localOffsetAt` to
`xr_time_local_offset` and `xrt_time_local_offset_at`, neither of which exists
any more, and name `stdlib/time/time.c` as the handwritten body of both. That
file does still exist, but now holds only the four leaves and `sleep`.

Whoever regains the ability to build these cases should re-derive every asserted
line of all five files together from `--dump-xaot-plan --dump-link-manifest`
output rather than by hand.

## 3.1 What the before/after test comparison actually says

The full 494-test suite could not be run to a summary line on this machine:
five lanes were running `ctest -j3/-j4` concurrently, load average peaked above
140, and both `backend_diff` (134.7s at the documented baseline) and
`backend_diff_embedded` (219.1s) hit their 900s timeouts. Two attempts were
killed by the host.

What was obtainable is a matched pair of runs over the 107 tests this change
can plausibly touch -- every `stdlib`, `surface`, `boundary`, `manifest`,
`contract`, `inventory`, `native`, `xi_cgen`, `semantic_plan` and `target_plan`
test -- one against the base tree and one against a tree carrying all four
migrations. Same filter, same `-j 2`, same machine, overlapping in time so both
saw the same contention.

```
before: 26 failed out of 107
after:  33 failed out of 104   (three tests were outside the second run's filter)
```

Set difference by test name, new failures only:

| test | verdict |
|---|---|
| `test_semantic_plan` | SemanticPlan fingerprint drift; re-anchored |
| `test_target_plan` | same |
| `test_target_plan_fingerprint_channel_close` | same |
| `test_target_plan_fingerprint_direct_local_call` | same |
| `test_target_plan_fingerprint_snapshot_determinism` | same |
| `test_target_plan_fingerprint_tail_coroutine_chain` | same |
| `stdlib_migration_contracts` | not a regression -- the comparison tree was built with `git archive` and has no `.git`, so every module's `legacy_commit is not available in this repository`, including modules this change never touched |
| `stdlib_self_hosting_consistency` | same artefact |

Nothing else moved. The second run's filter missed three cases, which were run
separately against the same tree: `test_type_identity_core` and
`test_xrt_type_identity_freestanding` pass on both sides, and `aot_filetests`
fails on both (40.8s after, 64.0s before). `aot_link_command_manifest` also
fails on the base, and `query_surface_residue`, `string_surface_residue`,
`aot_program_graph_native_execution`, `test_param_contract_aot` and
`legacy_product_residue_inventory` are load-induced timeouts on both sides.

## 4. Reproducibility note for anyone re-measuring

Four other lanes were running `ctest -j4` on this machine concurrently. Any
timeout observed under that load has to be re-run serially before it is
attributed to a change; see the round-3 briefing §4.3.

## 5. What landed, and what is left

Ten commits on `work/6-stdlib-selfhost-00f665c5c`:

```
Record why math cannot be self-hosted yet and how the other five stand
Publish the http2 surface from an Xray module body
Write the compress checksums and header judgements in Xray
Move the mem capability surface into Xray, leaving what Xray cannot say
Publish ten regex module functions from an Xray body
Re-anchor the SemanticPlan KAT the stdlib registry fingerprint moved
Record why each native leaf is an accepted C boundary
Stop asking the prelude for an Xray source it cannot have
Record what the six remaining modules turned out to be
Regenerate the analyzer and LSP stdlib tables for the four migrations
Re-anchor the plan fingerprints and stable ids the stdlib registry moved
```

Left for other owners:

- **math** — the blocker packet names the three coupled obstacles and the route
  through `xa_intrinsic_registry.def` that `simd` and `codegen` already take.
- **`RegexMatch` in `.xr`** — one line in `stdlib/prelude/builtin_symbols.def`
  would let `find` / `fullFind` / `findAll` move too, at the cost of making the
  name visible in every `.xr`. That is a language-surface call.
- **`Buffer` and the other native classes** — `StdlibNativeClassEntry`,
  `StdlibClassMethodEntry` and `StdlibTypeMethodEntry` need an `is_internal`
  property before any native class can leave `public_native`. The 16 mem
  symbols, the 13 regex ones and `net`'s 11 all wait on the same change.
- **The two UNRUN gates** — `stdlib_generated_c_reproducibility` and
  `stdlib_unified_target_plan_coverage` want backend probe evidence, which
  cannot be produced while AOT refuses these modules at `XR_TARGET_1000`.
- **The AOT link expectations** listed in §3, once their cases build again.
