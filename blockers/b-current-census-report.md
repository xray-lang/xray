# Lane B report: current census and refusal evidence

- **Lane**: B (current census / refusal evidence)
- **Status**: `READY`
- **Scope**: this lane began as measurement only and stayed that way through the census.
  It later fixed compiler defects the census surfaced, on explicit instruction, so the
  branch is no longer a pure measurement lane — see "Changes beyond measurement". No
  baseline, allowlist, or acceptance threshold was modified at any point.

## Exact source identity

| item | value |
|---|---|
| documented base | `bb6eac777369c915ddbde8e4fe76e622ded64d28` |
| worker branch | `work/b-current-census-evidence` |
| branch head at measurement | `b92abcc59c12` |
| tree state | clean |
| binary | `build/xray`, commit `b92abcc59c12`, `dirty=false`, Release, Ninja |
| build configuration | `cmake --preset default -DXRAY_STDLIB_VM_FASTPATHS=OFF` |
| provider | apple-clang 21.0.0, `ready=true`, `fallbackUsed=false`, all capabilities ok |

The branch head is ahead of the documented base. The census was regenerated at the
current head after the compiler fixes landed, so the manifest below matches the binary
that produced it.

## Manifest

| item | value |
|---|---|
| path | `build/current-refusals-b92abcc59c12.json` |
| sha256 | `1021265064cd4f381f8790ceb8742fad47e79b30b0cff410b2eea5591da48e04` |
| generator exit code | 1 |
| retained logs | 519 refused build logs alongside the manifest |

Command:

```
python3 tests/diff/survey_refusals.py --build build \
  --output build/current-refusals-b92abcc59c12.json --jobs 8 --timeout 180
```

Exit code 1 is investigation evidence, not a qualification certificate. It is caused by
four new refusals against the governed baseline plus seventy evidence gaps. No baseline
was edited to make it green.

## Outcome census

| outcome | count |
|---|---|
| comparable (pass) | 142 |
| refused | 519 |
| expected rejection | 2 |
| skipped | 1 |
| differential failure | 0 |
| **total** | **664** |

Denominator conserved: 142 + 519 + 2 + 1 + 0 = 664, matching both `summary.case_count`
and `inputs.case_count`.

**These figures describe the unoptimized pipeline only.** The generator measures with
`aot_opt="0"` and `xi_opt=""`. Re-running the 142 comparable cases with Xi optimization
enabled refuses 4 of them, all tail-call cases, so the honest reading is 138 comparable
both unoptimized and optimized. One of the four is not merely refused under optimization
but silently miscompiled. See `b-xi-optimization-tail-call-defects.md`.

Structured refusals 449, missing refusal evidence 70, refusal events 2775, distinct
root causes 487.

Against the historical `4b6908b68` sample of the same size (134 comparable, 519 refused,
8 provider failure, 2 expected rejection, 1 skip): comparable rose by exactly the eight
cases that were provider failures then, because this host has a complete native provider.
The refused count is unchanged.

## First-refusal clustering

Cluster key is `owner + family + blocking_fact`, as emitted by the source. 147 distinct
first-refusal clusters over 449 structured refusals.

By owner:

| owner | cases |
|---|---|
| target-plan-builder | 355 |
| aot-representation-refinement | 71 |
| semantic-plan-verifier | 23 |

By family:

| cases | owner / family |
|---:|---|
| 204 | target-plan-builder / calls_and_adapters |
| 101 | target-plan-builder / program_authority |
| 47 | aot-representation-refinement / refinement_use_site_oracle |
| 38 | target-plan-builder / program_build |
| 23 | aot-representation-refinement / refinement_definition_oracle |
| 17 | semantic-plan-verifier / semantic_coroutine_state_count |
| 6 | target-plan-builder / source_class_object_storage |
| 5 | semantic-plan-verifier / semantic_source_export_call_argument |
| 4 | target-plan-builder / channel_receive_storage |
| 1 each | semantic_ownership_certificate, refinement_return_storage_oracle, direct_local_callee_storage, scalars |

Rigid coupling: all 204 `calls_and_adapters` cases also carry `program_build`. The two
are one capability unit, not two candidates.

## Measurement limit that must be read with the numbers

Cross-owner cases: **0 of 449**. Every case's refusals fall inside a single owner. This
is a product of serial fail-closed layering, not evidence that a case is one layer from
building. A cluster's `lead`/`solo` count is therefore an upper bound on unlock, never a
lower bound.

This was measured directly for the largest cluster; see
`b-program-authority-guard-masks-frontier.md`. Removing that gate unlocked **zero** of
its 101 cases and multiplied their refusal events by 15.2x.

## Provider classification

| classification | count |
|---|---|
| provider-unavailable | 0 |
| provider-probe-failed | 0 |
| provider-compile-failed | 0 |
| provider-link-failed | 0 |
| compiler-capability-refused | 449 |
| refused without structured evidence (also compiler-side) | 70 |

None of the 519 refused build logs contains a host-toolchain compile or link error
signature. Nothing in this census is attributable to a missing or failing provider.

## Evidence gaps

70 refused cases carry no source-emitted structured refusal row.

| cases | classification | diagnostic |
|---:|---|---|
| 53 | opaque-refusal-without-structured-diagnostic | none |
| 12 | diagnostic-without-structured-refusal | XR_SEM_0019 |
| 3 | diagnostic-without-structured-refusal | XR_OWN_3002 |
| 2 | diagnostic-without-structured-refusal | XR_CORO_4003 |

Owners that should close them: the semantic plan verifier for `XR_SEM_0019`, the
ownership checker for `XR_OWN_3002`, the coroutine verifier for `XR_CORO_4003`. The 53
uncoded ones are dominated by representation-materialization and coroutine-lowering
failures that reach the user with no stable diagnostic at all — including one generated-C
verifier ICE (`W4_FORWARD_REF`, `tests/diff/cases/limits/struct_literal_over_32.xr`) that
the refusal baseline currently absorbs as an expected refusal.

`XR_AOT_REFINEMENT_*` names appearing in these logs are C enumerator names, not
registered diagnostic codes, so cases naming one still owe a stable diagnostic.

## Ratchet delta

| item | value |
|---|---|
| listed refusals in governed baseline | 515 |
| observed refusals | 519 |
| new refusals | 4 |
| listed refusals now building | 0 |

The four new refusals are three independent root causes, all in the AOT representation
refinement layer; see `b-new-refusal-regressions.md`.

## Next capability ranking

Ranked after the guard experiment, which changed the ordering: `program_authority` and
`calls_and_adapters + program_build` are not alternatives, they are consecutive segments
of one path. 68% of the cases behind the first land directly on the second.

| rank | capability | current cases | unlock | conflict | note |
|---|---|---:|---|---|---|
| 1 | general source module graph authority (`program_authority`) | 101 | **0 directly** | driver + analyzer | prerequisite for seeing real work, and for the default build producing a compiler at all |
| 2 | `calls_and_adapters` + `program_build` | 204 + 38, plus 73 revealed by the experiment | high | target-plan builder hot spot | rigidly coupled, must be one slice |
| 3 | `refinement_use_site_oracle` | 47 | medium | refinement hot spot | co-occurs with rank 4 in 38 cases |
| 4 | `refinement_definition_oracle` | 23 | medium | refinement hot spot | |
| 5 | `source_namespace_storage` | 16 | unknown | target-plan builder | invisible in this census; only the experiment revealed it |
| 6 | `semantic_coroutine_state_count` | 17 | closes within one family | semantic layer | |
| 7 | `source_class_object_storage` | 6 | 0 alone | target-plan builder | never closes a case by itself |
| 8 | `semantic_source_export_call_argument` | 5 | closes within one family | semantic layer | |
| 9 | `channel_receive_storage` | 4 | 0 alone | target-plan builder | |
| 10 | the 53 uncoded refusals | 53 | not rankable yet | — | must emit stable diagnostics before they can be targeted |

Ranks 1 and 2 are the same campaign. Ranks 3 and 4 are the same campaign. Rank 5 cannot
be scheduled from this census alone because its size is only known from the experiment.

## Verification

Passed: both evidence-runner self-tests; `contract_freeze`; `target_machine_inventory`;
`backend_diff_deterministic`; the three target-machine self-test lanes; single-case
replay agreeing with the manifest in both directions; binary sha256 identical to the
manifest record; denominator conservation; the independent checker replaying all 519
logs with zero reconstruction mismatches.

Not run: any other platform (this is macos-arm64 only), the sanitizer lanes, the full
CTest suite, Windows or MSVC, and any production qualification. This lane supplies
current decision facts only and does not represent completion of the target-machine work.

One gate was intermittently red for a reason unrelated to this lane's measurements:
`backend_diff_deterministic` fabricated refusals when a cold cache coincided with
concurrent ctest load. Reproduced, root-caused, and fixed in
`b-probe-timeout-fabricates-refusals.md`. The census evidence was checked against that
failure mode and is clean.

## Baseline defects observed at the documented base

Both predate this lane and are unmodified by it.

- The default CMake configuration cannot produce a compiler. `XRAY_STDLIB_VM_FASTPATHS`
  defaults to `ON`, and generating the fastpath harness compiles a 25-import module graph,
  which the program-authority guard refuses. `build-fastpaths-on/xray` is never produced.
- `backend_diff_optimized` is red. As measured it reported 44 passed, 0 differential
  failures, 13 refused, of which three were not baselined. Confirmed pre-existing by
  re-running the lane with this branch's `CMakeLists.txt` reverted — it failed identically
  either way. The tail-call fixes in `b-xi-optimization-tail-call-defects.md` close one of
  the three, taking the lane to 45 passed and 12 refused; the two that remain are the PHI
  regressions in `b-new-refusal-regressions.md`, which this lane did not attempt.
- With fastpaths off, 2 of 620 build targets fail: the generated fixtures
  `tests/unit/generated/leaf_product_native.c` and `i64_overflow_native.c`. Their
  generator is the registered CTest `test_xi_program_semantic`, two of whose subtests are
  red (`XR_TARGET_1003: leaf product program target projection is invalid`, predicate
  `verify_product_program_target` at `src/plan/target/xr_target_verify.c:8270`). The
  compiler itself links and is complete.

## Changes beyond measurement

The census is the deliverable this lane was chartered for, and it was produced before any
of the changes below. They are listed so a reviewer sees the whole branch rather than
discovering half of it in the diff.

**Evidence tooling and diagnostic governance.** The refusal generator and its independent
checker decided what a diagnostic code was by matching an `XR_`-shaped token, which both
accepted internal enumerator names and rejected registered codes over punctuation. Both
now look the code up in the governed registry. Seven emitted codes that were never
registered are now registered, and the registry's `unknown_code = "error"` policy has an
enforcer for the first time.

**Gate configuration.** The differential lanes now scale the native-run probe budget, so
a cold cache under concurrent load stops reporting every case as a build refusal.

**Compiler fixes.** Four defects, all the same omission at different layers: a promoted
tail call was not modelled by the inliner, by representation selection, by ARC's
callee resolution, or by the AOT definition-site oracle. Details, evidence and the one
fix that was tried and reverted are in `b-xi-optimization-tail-call-defects.md`.

These touch `src/ir/` and `src/aot/`, which lane B's charter lists as read-only, and the
probe change touches a timeout the charter forbids modifying. Both were done on explicit
instruction rather than this lane's own judgement.

**Effect on the census: none.** The generator measures at `aot_opt="0"` with no Xi
optimization, and every compiler fix is on the optimized path. Re-running the full census
at the fixed head reproduces the earlier numbers exactly — 664 = 142 + 519 + 2 + 1, 449
structured refusals, 70 evidence gaps, 2775 refusal events, 4 new refusals. The fixes are
visible only with `--xi-opt` enabled, where the four affected cases go from 0 passed / 4
refused to 4 passed on both backends.

## A gate that cannot pass at this base

`asan_focused` fails on this branch, and would fail on any branch cut from this base. It
builds the whole tree, and the tree does not build: the generated fixtures
`tests/unit/generated/leaf_product_native.c` and `i64_overflow_native.c` fail exactly as
they do at `bb6eac777`, with `XR_TARGET_1003: leaf product program target projection is
invalid` from `test_xi_program_semantic.c:2536` and `:2766`.

No sanitizer reported anything; the failure is in the build phase.

**This is already fixed upstream.** The same fixtures were diagnosed as an architecture
predicate wrongly refusing the leaf/product projection on arm64 hosts, and the fix is
integrated at `afa2df8bf`, which is the integration branch's current head. This branch is
based on `bb6eac777` and is 71 commits behind it, so it does not carry that fix. The
statement to take from this section is therefore about this branch's base only, not about
the integration branch: rebasing onto the current head is expected to make the tree build
and let `asan_focused` actually run its sanitizers.

A separate entry in the bug ledger records that once the tree does build, `asan_focused`
has one genuine sanitizer failure of its own in the LSP incremental refresh path. That is
unrelated to this lane and was not reached here.
