# Task 213 proof-carrying Xi baseline

Date: 2026-07-19

## Fingerprint

- Compiler HEAD: `29178fdc6678099562f7d695708f27be44e41eb0`
- Branch: `task/213-staged-xi`
- Host: `arm64-darwin` (`Apple M5 Pro`)
- Compiler: `Apple clang 21.0.0`
- CMake profile: `Debug`, tests enabled, full stdlib
- Build command: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j8`

## Focused gate

The following baseline command passed 5/5 tests in 3.32 seconds:

```bash
ctest --test-dir build --output-on-failure \
  -R '^(test_xi_stage|test_xi_pipeline|test_xi_intrinsic|test_xi_backend_plan_contract|zero_cost_plan_inventory)$'
```

The registered test names are `test_xi_stage`, `test_xi_pipeline`,
`test_xi_intrinsic`, `test_xi_backend_plan_contract`, and
`zero_cost_plan_inventory`. There is no test registered as `test_xi_verify`;
the current extended verifier target is `test_xi_verify_ext`.

## Structural baseline

- Stages: `RAW -> CANONICAL -> CLOSED -> OWNED -> CORO_LOWERED -> REPPED -> BACKEND`.
- `XiFunc.stage` is publicly mutable; 11 production assignments exist outside
  the structured pipeline error record.
- Range, TBAA, MemSSA, escape, and effect are mixed into the cumulative
  `XiInvariantMask`; 33 source references use those transient bits.
- `XiPassDesc` has only `requires_inv_mask` and `produces_inv_mask`; it has no
  revision, invalidation, or preservation contract.
- AOT calls `xi_simd_lower_bundle()` after bundle preparation and verification.
- SIMD identity uses one stdlib path suffix, six display type names, and 24
  method spellings.
- CGen contains 43 `_lanes` sites and nine pending-error emission sites. The
  broad text/analysis counts in `213-cgen-residue.tsv` are inventory numbers,
  not claims that every match is semantic inference; each must be classified
  before a zero target can be imposed.

## Root reproductions

The current architecture cannot express a revision-bound stale range, escape,
or effect proof as a first-class row, so the three corruption fixtures do not
yet exist as executable APIs. This absence is itself the P0 reproduction:
analyses are mutable value annotations or cumulative invariant bits. P3 must add
executable corruption tests before any backend is allowed to consume the new
evidence store.

The xxHash performance/shape baseline is owned by task 211 and stored in the
port branch commit `b797705`. Its ARM results are gating; the amd64 container
matrices on this ARM host are explicitly non-gating.
