# Public smoke gate

Run against the freshly built CLI:

```powershell
ctest --test-dir D:\xray-build\d-rel -R '^smoke$|^smoke_runner$' --output-on-failure
```

The equivalent standalone command is:

```powershell
python -X utf8 tests/smoke/run_smoke.py --binary D:\xray-build\d-rel\xray.exe --root E:\xray\wt\d --output D:\xray-build\d-tmp\smoke
```

`cases.json` records every demo, every Xray fence in both README files, and
every public `stdlib/<module>/<module>.xr` entry. Internal `_probe` and
`codegen` are excluded. A module can contain supporting files: `io` still has
one public entry. `json` has no `json.xr`, and prelude types have no module
entry. Network demos are checked without starting a server.

The runner discovers these inputs on every invocation and rejects missing or
duplicate manifest entries. The current README denominator is **6**, including
both languages and all three examples in each. The older homepage-only census
used 1. Each passing VM case must have a native companion with the same output
oracle. Source and expected-output paths are relative to the repository; README
paths use `#xray-N` to identify the Nth Xray fence.

## Verdicts and ratchet

- `pass`: exit zero, exact stdout bytes, no stderr. Oracles are reviewed against
  the example's semantics; they are not regenerated automatically. Expected
  files retain LF bytes on every checkout host.
- `gap`: the exact recorded exit-1 rejection, empty stdout, and the same first
  diagnostic. Source-root and temporary-source paths are normalized; diagnostic
  content and locations are preserved. Each entry names its F item.
- `fail`: a passing example regressed, a known gap unexpectedly succeeded,
  the rejection changed, a process timed out/crashed, or a required tool or
  output disappeared. A gap that succeeds must be reviewed, given an output
  oracle, and promoted to `pass` with its native companion in the same change.

The current five native companions exercise the actual direct-C CLI build path.
They record an exact exit-1 invocation-profile rejection, empty stdout and no
published executable on this Windows provider. They are executed `gap` cases;
native program execution has not been reached, and no native functional pass
is claimed. The original VM/native output oracles remain shared. Promote a
companion only after its real build and executable match that full oracle.

The base64 VM entry now passes its original `import base64; print(1)` oracle.
This checks module import, not the complete encoding/decoding property cases.
The crypto gap location follows the unchanged `HEX_DIGITS.slice` expression
after Builder removal; encoding advances past admitted `bytes()` to the
existing unsupported `runes()` contract. Both remain exact feature gaps,
with unchanged algorithms, outputs, exit codes and classification rules.

Separate fresh Windows observation on 2026-10-21 at `6856eb80a`: the existing
legacy `xray build` path built and ran hello, control_flow, and the time import
probe with matching output. This differs from the historical native failure
record. It is not evidence that the new direct-C path has been implemented.

The summary counts actual `pass` results, not accepted known failures:

```text
smoke: demos 3/23 stdlib 2/29 readme 0/6 native 0/5
smoke gate: pass=5 gap=58 fail=0 not_run=0
```

Each invocation writes a current `results.json` and a fresh log directory under
`--output`. A manifest failure replaces the previous summary with a failed
report, so stale evidence cannot be mistaken for the latest run. Build stdout
is separate from program stdout. The shared process runner kills timed-out
process trees; no failed case is retried into a pass.

## Gate purpose and cost

This L1 gate detects broken official examples and imports, output regressions,
unclassified failures, and forgotten promotions. It closes the missing product
coverage that allowed only 2 of 22 demos and 1 of 29 public modules to work while
internal tests passed. The fresh Windows Release CTest run measured 3.90 seconds
for `smoke` and 0.54 seconds for `smoke_runner` (4.44 seconds combined). Total
wall time was 102.89 seconds, including collection of the full test registry.
The gate is sequential (`PROCESSORS 1`) and tagged `smoke;fast`.
Each executed command has a 30-second timeout and CTest caps the gate at 300
seconds. This is a functional cost observation, not a performance benchmark.

`smoke_runner` tests inventory coverage, positive and negative verdicts,
timeouts/crashes, the pending-path boundary, and native output separation.
The required manual regression exercise uses a temporary copy of the manifest:
replace hello's stdout oracle (and its native companion's) with `one.txt`; the
real gate must return 1 with `FAIL demos/01-basics/hello.xr`. Do not modify the
official example or commit the intentionally incorrect manifest.
