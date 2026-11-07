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

**Validation follows the direct-cutover verification policy.** Use the current
numbered roadmap and `rules/verification-policy.md` as the gate boundary:

- L0: incremental builds and affected `.xr` positive and negative cases. Hot-path
  changes immediately run affected performance benchmarks after rebuild.
- L1: Windows and macOS builds, smoke `run` and `build-run`, valid LIR and
  register-bytecode tests, and W1-W4 with real host C compilers before main.
- L2: affected ASan, UBSan and `XR_XIR_PARANOID` coverage, plus an independent
  runtime ownership review when runtime, RC or code generation changes.
- L3: the full `.xr` corpus, performance gates, Linux and complete sanitizers
  in the nightly lane.

Run the affected coverage for the change. Reuse passed results only when the
source, effective dependencies, configuration and products are unchanged.
Do not require a full sanitizer matrix or an independent review for every
ordinary leaf edit. A failed or unavailable gate remains FAIL or NOT_RUN;
removing an obsolete target does not turn its old result into a pass.

**Compiler memory safety remains mandatory.** Strings and metadata crossing
AST, HIR, XIR, LIR, plan or generated-C boundaries must be copied into the
receiving owner or transferred explicitly. Never retain a dynamic-array
member pointer across an operation that can grow the array. Preserve the
allocation and owned-lifetime checks in the affected metadata/runtime tests.
External input still receives complete validation. Same-process revalidation
moves to the paranoid build at the execution cutover; do not silently remove
checks ahead of that change. The generated-C W1-W4 verifier remains enabled:
never add a bypass, downgrade its failure, or send malformed C to a compiler.
Keep `.xr` language inputs and their oracles when retiring an old C harness.
The smoke ratchet, exact first diagnostics and each product command's
30-second limit remain in force.

**Generated C is portable C11 first.** The AOT backend and its runtime headers
must compile with supported MSVC, Clang, GCC, and Zig C providers for the
selected target.

- Never emit GNU statement expressions (`({ ... })`) or another
  provider-specific language extension on an unconditional generated-C path.
- Express multi-statement value production with ordinary scoped statements or
  a typed runtime helper. Provider intrinsics are allowed only behind an owned,
  capability-gated runtime boundary with a portable implementation where the
  feature is part of the supported language surface.
- Automatic native compilation currently supports host MSVC and Clang on
  Windows, and host Clang and GCC on Unix. Cross-target and Zig automatic
  compilation are temporarily unavailable. On Windows, discovery prefers a
  ready MSVC, then Clang. A fallback records a failed capability probe.
- A generated-C portability change needs a regression gate that compiles real
  generated output with MSVC and rejects newly introduced GNU statement
  expressions; a synthetic SDK probe alone is insufficient evidence.

Document any platform-only LeakSanitizer suppression in `scripts/lsan.supp`; the suppression budget may shrink but may not grow without an explicit contract review.

## Coding and commit rules

The full coding standard lives in the sibling `xray-docs` repository, checked
out next to this repo in the umbrella directory (`../xray-docs` from the
primary checkout; from a worktree under `.claude/worktrees/`, resolve it via
the primary tree). Read the file that matches the work:

- `xray-docs/rules/c-coding-standards.md` — memory, visibility, assertion
  density, naming, size caps, the file-header template, and the comment rules.
- `xray-docs/rules/architecture.md` — the L0→L8 include DAG, `XR_OS_*`
  platform macros, native types via the prelude table, and the task-218/219
  ownership and RC contracts.
- `xray-docs/rules/dev-workflow.md` — build/test/debug entry points, Windows
  `scripts/win_pd_test.sh` discipline, and the shared-worktree git rules
  (no `git stash`, `reset --hard`, or `checkout -- <file>` in a shared tree;
  commit with pathspecs).
- `xray-docs/rules/design-principles.md` and
  `xray-docs/rules/concurrency-ownership-surface.md` — the language's type and
  sharing model. These win over any older doc that still shows `shared`
  declarations (removed) or `any` (removed).

(`.devin/rules/` in this repo is the legacy Windsurf-era digest of the same
rules — its process content is mostly still right, but its `xray-lang.md`
predates the `shared` removal; on any conflict the `xray-docs/rules/` files
above are authoritative.)

The rules agents break most often:

- **All C comments and all commit messages are in English and self-contained.**
  Never reference a `.md` document path from a source comment or a commit
  message, and never use phase talk (`Phase A/B/C`, `Step 2`, `P0/P1`,
  `Round 2`, 本次重构) — docs move and phases expire, but code history must
  stand alone. State the fact and the reason; long-lived design intent belongs
  in the doc comment of the owning function or type.
- **Commits carry no AI attribution or process-specific trailers.** No
  `Co-Authored-By` trailer naming Claude or any other tool, no tool as author
  or committer, and no `CONTRACT-CHANGE:` trailer. Keep design rationale in
  the ordinary self-contained subject/body.
- **Allocate only through `xr_malloc`/`xr_free`**, check for NULL, and pass
  `xr_realloc` results through a temporary pointer. Non-`static` functions
  carry `XRAY_API`/`XR_FUNC`; preprocessor OS checks use `XR_OS_*`, never
  `_WIN32`/`__APPLE__`/`__linux__`.
- **Bugs are zero-tolerance.** Fix the root cause with a regression test now,
  or report immediately and record it in `xray-docs/known_bugs.md`. Never
  skip-and-continue, and never mask a bug with catch-and-ignore or a skipped
  test.

## Design decisions and verification

Frozen language and implementation decisions are recorded in the sibling
`xray-docs` repository (numbered task documents and design records) and are
enforced by ordinary tests; there is no separate contract registry, digest
anchor or contract gate. A change that alters a frozen decision updates the
owning document and the tests that carry it in the same batch. Never refresh a
runtime, schema, provider, ABI, or artifact fingerprint merely to silence a
failure.
