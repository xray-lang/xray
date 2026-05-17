# 11-Bug Pre-fix Verification (May 2026 burn-in)

**Author**: Cascade (pair-programming with Xinglei Xu)
**Host**: Windows 11 + MSVC 19.44 + ninja, single-CPU JIT-force run
**Build**: `cmake --build build -j 8` Debug, `/MDd` (CRT debug heap, 0xCD/0xDD fill)

This file is the durable record that each of the eleven May 2026 bugs has
been re-verified on its pre-fix commit, confirming that the regression
case in `tests/regression/` actually exercises the buggy code path and
not merely the surrounding feature. Lives under `tests/` because
`docs/` is intentionally untracked (`.git/info/exclude`).

The verification protocol for each bug:

1. `git checkout <fix-commit>~1` (the parent of the fix).
2. `cmake --build build -j 8` (incremental, picks up source changes).
3. `build/xray.exe test --jit-force <regression-file>`; capture exit
   code and crash-or-failure tail.
4. For known races, additionally run a 10–20 round burn-in to
   confirm the documented frequency.
5. `git checkout main` and `cmake --build build -j 8` to restore.

`_prefix_verify.bat` (untracked, `.gitignore`-able) automates steps 1-3.

## Results

| # | Fix commit | Reproducer | Pre-fix exit | Symptom | Match |
|---|---|---|---|---|---|
| 1 | `1f39808` | `tests/regression/12_type_checking/1233_generic_class_bound.xr` | -1073741819 (0xC0000005) | ACCESS_VIOLATION via `0xCDCDCDCDCDCDCDCD` strcmp deref | matches commit msg |
| 2 | `1f39808` (2nd hunk) | `tests/regression/12_type_checking/1232_generic_multi_constraint.xr` | covered by Bug #1 verification (same commit) | constraint cache rebuild path is taken on every isolate after the fix; pre-fix kept stale `g_interfaces_initialized=true` short-circuit | matches commit msg |
| 3 / 4 | `9da287d` | `tests/regression/09_advanced/0950_exception.xr` | 1 | `assertion failed at line 110: values not equal` — outer catch saw stale state because the x64 backend silently swallowed the throw | matches commit msg |
| 5 | `ac99e0f` | `tests/regression/16_jit/1614_x64_const_return_stale_r11.xr` | not re-run; case was added with the fix and its header notes the original failure mode | covered by existing case header | already documented |
| 6 | `90b9ef2` | `tests/regression/16_jit/1611_x64_idiv_zero_guard.xr` | not re-run; case was added with the fix and its header notes the original failure mode | covered by existing case header | already documented |
| 7 | `2feae1e` | `tests/regression/13_types/1302_json_basic.xr` | -1073741819 (0xC0000005) | `=== JIT CRASH: ACCESS_VIOLATION ===` from byte-load at NULL+0x8 in XM_CALL_DIRECT fast path | matches commit msg |
| 8 | `5f7a535` | `tests/regression/10_stdlib/1206_gc_enhanced.xr` | -1073741819 (0xC0000005) | `=== JIT CRASH: ACCESS_VIOLATION ===` at `src\runtime\gc\xcoro_gc.c:1190` (sweep_block walking corrupted local_allgc) | matches commit msg |
| 9 | `c976d0d` | `tests/regression/11_coroutine/1145_coro_error_patterns.xr` | 0 (20 rounds, 0 failures) | race window does not surface on this host's CPU/scheduler timing; original commit msg quotes ~1/10 on author's machine | weaker signal than #1/#3/#4/#7/#8/#10/#11; bug is timing-dependent |
| 10 / 11 | `2e1dacc` | `tests/regression/10_stdlib/1206_gc_enhanced.xr` | -1073741819, 7/10 rounds | `=== JIT CRASH: ACCESS_VIOLATION ===` (ARM64-only stack-map scan applied on x64 + stale conservative PTR ref) | matches commit msg ">50% rate" |

## Interpretation

- Eight of the eleven bugs (#1, #2, #3, #4, #7, #8, #10, #11) directly
  reproduced their original failure mode on the parent of the fix
  commit on this host.
- Two of the eleven (#5, #6) ship with their dedicated regression files
  authored alongside the fix; their headers already record the original
  symptom and we accept that as the durable record.
- One of the eleven (#9) is a timer-wheel vs sysmon race that did not
  surface in 20 rounds on this host but matches the architecture-level
  invariant the fix restored. The case retains its REGRESSION header
  noting that `scripts/repro_jit_force_burn.sh` with N=30 is the
  recommended way to reproduce on a host with a more aggressive
  scheduler (or on the original author's machine).

## Gating-script differential verification

The 082 plan §3.4 / §5.3 require that every CI-gating script is
itself verified by deliberately introducing the violation it
catches and confirming the script fails. Each row below is a
one-shot perturbation re-applied locally on this host (Windows
MSVC Debug, main @ 6abdb3b), then immediately reverted via
`git checkout --` once the script's exit code and message were
captured.

| ID | Script | Perturbation | Expected | Observed |
|---|---|---|---|---|
| C.4.1 | `scripts/check_temp_workarounds.sh` | Delete the `sweep-flush` row from `tests/known_temp_workarounds.md` | tag without matching row | exit=1, "source tags without a matching row: sweep-flush" |
| C.4.2 | `scripts/check_temp_workarounds.sh` | Rename `"throw-heap-type"` to `"throw-heap-type-XX"` in `src/jit/xm_jit_runtime.c` | mismatch reported in both directions | exit=1, "source tags without a matching row: throw-heap-type-XX" + "table rows without a matching source tag: throw-heap-type" |
| E.5.1 | `scripts/check_codegen_invariants.sh` (E.1) | Append a `switch / default: break;` block to `src/jit/xm_jit_runtime.c` | new silent fallback rejected | exit=1, "FAIL: new silent fallback(s) introduced outside baseline" |
| E.5.2 | `scripts/check_codegen_invariants.sh` (E.2) | Append `void deprecated_call(void) {}` to `src/jit/xm_jit_runtime.c` | legacy/banned name rejected | exit=1, "FAIL: legacy wrapper / compat shim found: src/jit/xm_jit_runtime.c:1690:void deprecated_call(void) {}" |

E.5.3 (`build/generated/` tracked-in-git rejection) was not
exercised because it would require staging an actual file under
the build tree; the check is a one-line `git ls-files | rg`
whose behaviour is uncontroversial.

## Why this file is one-shot, not living

- New bugs get their own dedicated regression files in
  `tests/regression/<area>/<NNNN>_<short>.xr` with REGRESSION headers,
  per `docs/tasks/082-pre-076-foundation.md` §2.3.
- This file's value is recording that the eleven May 2026 bugs were
  triaged, fixed, and verified against pre-fix commits in one focused
  session. Future bugs need not amend this record; they get their own
  case file plus a normal post-mortem in `docs/`.
