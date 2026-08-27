# Blocker: a loaded machine turns toolchain probe timeouts into fabricated refusals

- **Lane**: B (current census / refusal evidence)
- **Status**: `FIXED` in this branch
- **Owner of the changed file**: H (build and gate configuration) — see the ownership
  note at the end; this fix was made on explicit instruction, outside lane B's normal
  file ownership.
- **Severity**: evidence integrity. Under load, whole batches of cases were reported
  `REFUSED (aot did not build)` for a reason that has nothing to do with the compiler.

## What happens

`backend_diff_deterministic` fails intermittently. Every case in the run reports:

```
tests/diff/cases/semantics/coro/await_array.xr    REFUSED (aot did not build)
      [RUN_PROBE_FAILED] native-run: probe stage timed out after 10000 ms
```

The native-run probe stage exceeds its 10 s budget, the toolchain selection is treated as
unusable, and every case in the lane is recorded as a build refusal. Nothing about the
compiler or the cases changed.

This matters beyond one flaky lane: a refusal produced this way is indistinguishable, in
the diff runner's output, from a genuine capability refusal. Any evidence gathered on a
loaded machine can silently contain fabricated refusals.

## Reproduction

Deterministic given the two conditions together — a cold cache and concurrent ctest load:

```
rm -rf .cache/xray-test/deterministic
ctest --test-dir build -j 2 -R "^(contract_freeze|contract_freeze_injection|target_machine_inventory|target_machine_inventory_self_test|backend_diff_deterministic|target_machine_baseline_runner_self_test|target_machine_matrix_evidence_self_test|target_machine_matrix_row_self_test)$"
```

Observed on an 18-core macos-arm64 host at `5cc0daf38888`:

| condition | result |
|---|---|
| warm cache, serial (3 runs) | pass, ~15 s |
| warm cache, `-j 2` | pass, 3.3 s |
| cold cache, serial | pass, 43.6 s |
| **cold cache, `-j 2`** | **fail, 160 s** |
| cold cache, `-j 2`, `XRAY_TOOLCHAIN_PROBE_SCALE=6` | pass, 27.7 s |

Neither condition alone reproduces it. The failure is not a CTest timeout: the lane's
`TIMEOUT` is 900 s and the failing run took 160 s.

## Cause

`XTC_PROBE_RUN_TIMEOUT_MS` is 10 s, applied at `src/app/toolchain/xtc_probe.c:524` through
`xtc_probe_timeout()` (`:138`). That helper already reads
`XRAY_TOOLCHAIN_PROBE_SCALE` and multiplies the base budget by it, with a 10 minute
ceiling — the escape hatch exists. No `backend_diff*` lane in `CMakeLists.txt` sets it,
so on a loaded machine the probe competes for cores with the other lanes and overruns a
budget sized for an idle one.

Scaling the budget also makes the run *faster* when it would otherwise fail (27.7 s versus
160 s), because the lane no longer spends 10 s per case timing out before failing.

## Fix

All four `backend_diff*` lanes now set `XRAY_TOOLCHAIN_PROBE_SCALE=4` in their `add_test`
environment, beside the other environment those lanes already pin.

The scale value follows the existing precedent in
`tests/target_machine/comparison/run_comparison.py:741`, which already defaults the same
variable to 4 for the same reason. A scale of 6 was tried first and also works; 4 was
measured sufficient under the reproduction conditions above and matches what the tree
already chose, so it is not a new magic number.

Verified in the exact failing conditions — cold cache, `-j 2`, with no
`XRAY_TOOLCHAIN_PROBE_SCALE` in the caller's environment — where the lane now passes in
15.7 s instead of failing in 160 s.

`CMakeLists.txt` is anchored by five contracts (`ownership-audit-foundation.md`,
`program-semantic-closure.md`, `runtime-target-plan-load.md`,
`typed-target-plan-debug.md`, `typed-target-plan-execution.md`); all five anchors were
refreshed in the same commit and `contract_freeze` passes.

This does not close the underlying question. Scaling the budget makes the lanes stop
fabricating refusals, but the probe still competes for cores with whatever else CTest is
running. Whether the durable answer is a scaled budget, lower lane concurrency, or
`PROCESSORS` accounting that keeps these lanes from overlapping is still H's call.

## This lane's own evidence is unaffected

The 664-case census was generated with `--jobs 8` on the same host, so it had to be
checked rather than assumed. It is clean:

- 0 of the 519 retained refusal logs contain `RUN_PROBE_FAILED` or `probe stage timed out`
- 0 occurrences in `build/current-refusals-5cc0daf38888.json`

`tests/diff/survey_refusals.py` probes the toolchain once up front through
`toolchain_identity()` with a 180 s budget and fails closed if that probe does not prove a
ready provider, rather than re-probing per case. That is why the census is immune to the
failure mode described here, and it is worth preserving if the diff runner's probing is
reworked.

## Ownership note

`CMakeLists.txt` is outside lane B's declared file ownership, and lane B is otherwise
forbidden from touching timeouts. This change was made on explicit instruction rather than
by this lane's own judgement, and is called out here so the reviewer sees the boundary
that was crossed rather than discovering it in the diff.

What the change does *not* do is relax any measurement threshold: the probe budget governs
how long a toolchain probe may take before the toolchain is declared unusable, not how long
a case may take to build or how many cases may fail. No baseline, allowlist, coverage
ratchet, or case timeout was touched.

`src/app/toolchain/xtc_probe.c` was not modified; the fix uses the scaling hook that helper
already provides.
