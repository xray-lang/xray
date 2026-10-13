# Legacy consumer preparation

This directory preserves migration obligations and independent expectations before a
published product baseline is available. It does not run Xray, claim a capability,
change a product interface, or authorize deletion of an old test.

- `responsibilities.json` maps the 27 historical capability families, eight retained
  goals, 14 qualification families and 18 root-effect requirements to their owning
  tasks and packages. Historical names come from a fixed Git object; retired
  contracts governance, layer states and exclusions are not reinstated.
- `consumer_routes.json` assigns an explicit primary family and producer packages
  to all 108 original Source responsibilities. It preserves 26 other files and
  all 160 indexed function bodies, including seven helper files with no counted
  test functions. Native harness success exits stay separate from language results;
  all replacement qualification and new-entry mappings remain OPEN.
- `oracles.json` records seven independent output/result/rejection expectations.
  Inputs under `fixtures/` retain complete original source literals byte for byte.
  They have no invented new entry adapters. The StringBuilder input still requires
  a reviewed semantic migration to its replacement; it is not an API to restore.
- `audit.py snapshot` calls the existing runners' actual discovery and expectation
  parsers and checks the entire existing source-consumer inventory. It preserves
  missing expectations, absent historical anchors and unmapped case responsibilities
  explicitly. Counts overlap and are never a completion percentage.
- `compare_required` checks a single observation against the independent expected
  fields. It is preparation infrastructure, not a product execution driver. The
  tests include identical-but-wrong backends, absent versus empty output, exact
  cleanup order, missing requirements and a runtime failure posing as a compile
  rejection. Printed cleanup does not prove physical resource reclamation.

The standalone CMake project uses Ninja and `LANGUAGES NONE`; it validates preparation
artifacts only. It does not compile a product binary or replace production CTest
registration, budgets, sanitizer lanes, or P20's mixed-execution tests.

Run from the owning code worktree:

```powershell
cmake -S tests/unit/xir/legacy_consumers -B build-p19-preparation -G Ninja
ctest --test-dir build-p19-preparation --output-on-failure -j 2
python -X utf8 -B tests/unit/xir/legacy_consumers/audit.py snapshot --output .tmp_p19/inventory.json
```

The inventory distinguishes the legacy per-file `run_regression_tests.py` entry
selection from the registered `regression_corpus` gate. The latter uses directory
level `xray test` and the exact named-test ratchet in `xir_passing.txt`; its runtime
test discovery has not run here. The default backend-diff net invokes `run`, so a
matching source file does not establish that regression `@test` bodies executed.

After fixed dependencies arrive, review their documented interfaces and accepted
ancestor SHAs, re-inventory changed source inputs without erasing the seed evidence,
complete case-level mappings and independent missing oracles, and connect the real
consumer driver and production registration. Fresh VM and real generated-C native
observations must each satisfy the oracles, with separate full applicable ownership,
FI, resource, cancellation, physical cleanup and safety evidence. Until then every
product result remains `NOT_RUN` and every capability remains `OPEN`.
