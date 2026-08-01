# Repository agent gates

## Build and test rules

**Ninja is the only build generator, on every platform.** There is no Makefiles
or Visual Studio path, and no fallback to one — a missing `ninja` is an error
with an install hint, never a silent switch to another generator.

- Configure locally with `cmake --preset default` (Ninja + Release in `build/`).
  Release is deliberate: this is a compute-heavy compiler, so optimized test runs
  dominate the edit/build/test loop far more than the slightly longer compile.
  Use the sanitizer presets (Debug + assertions) for correctness passes.
- Ninja is single-config. A build tree has exactly one binary location —
  `build/xray` (`build/xray.exe` on Windows). Never add a `Release/` or `Debug/`
  subdirectory probe, and never pass `--config` to `cmake --build`/`cmake --install`.
- On Windows, Ninja needs `cl.exe` on PATH; CI does this with
  `ilammy/msvc-dev-cmd`. Every `cmake -B` in `.github/workflows/` passes `-G Ninja`.

**Debug info follows the build type.** `Release`/`MinSizeRel` compile without
`-g`; `RelWithDebInfo` is the "optimized with symbols" build. Do not add `-g`
back to Release — it slows compile and link, inflates `libxray_core.a` roughly
4x (which is what makes the per-test static link expensive), and lowers the
ccache hit rate. Debug a release-only crash by rebuilding with `RelWithDebInfo`.

**Diagnostics colour themselves only for a terminal.** `xr_diag_use_color()` in
`src/frontend/xdiag_fmt.h` gates on `isatty(stderr)` plus the `NO_COLOR`
convention. Piped or redirected output is therefore plain text, and test
harnesses must not strip ANSI escapes — if you find yourself adding a `sed`
to remove colour, the gate is what needs fixing.

**Test runners are parallel and order-stable.** A corpus runner fans work out
with `xargs -P` and writes one self-contained result file per case, then reads
them back in sorted order, so the report and the tallies are identical to a
serial run. Keep them free of per-case forks on the hot path (`sed`, `grep`,
`head`, `basename` per case cost more than the work itself); bash parameter
expansion does the same job. Portable bash only — no `mapfile` (macOS ships
bash 3.2) — so the same script runs under Windows Git-Bash.

**Heavy ctest lanes declare their cost.** Any lane over ~100s carries a `COST`
matching its measured wall time so a cold run (no `CTestCostData.txt`) schedules
it first instead of stranding it at the tail. Keep `PROCESSORS` honest about the
cores a lane actually consumes.

Changes under `src/ir/`, `src/aot/`, or `src/analysis/` must preserve the Task 218 compiler-memory-safety defenses:

- Run `ctest --test-dir build -R meta_ownership_inventory` after compiler metadata changes.
- Run `ctest --test-dir build -R asan_focused` before handing off a completed change in those directories.
- Strings and metadata crossing AST, analyzer, IR, plan, evidence, or CGen stage boundaries must be copied into the receiving arena/pool or transferred explicitly. Do not retain dynamic-array element pointers across operations that can grow the array.
- The generated-C W1-W4 verifier is always on. Do not add a bypass, downgrade its ICE behavior, or hand malformed output to the host C compiler.

Document any platform-only LeakSanitizer suppression in `scripts/lsan.supp`; the suppression budget may shrink but may not grow without an explicit contract review.

## Semantic contract freeze

The machine-checked semantic contracts live in `contracts/`. Before changing a
listed anchor, read the owning contract and decide whether existing diff cases,
KATs, shape gates, or ports evidence must be migrated.

- Refresh every affected `anchor-sha256` record in the same commit.
- Add `CONTRACT-CHANGE: contracts/<file>.md <one-line reason>` to the commit
  message for each affected contract.
- Record how affected evidence was rerun, regenerated, or retired. Retirement
  must use the governed tombstone inventory; never silently delete evidence.
- Run `ctest --test-dir build --output-on-failure -R contract_freeze` after the
  commit so trailer enforcement runs against the final commit message.
