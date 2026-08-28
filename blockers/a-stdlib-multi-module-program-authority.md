# Blocker: AOT refuses every module graph that contains an Xray standard-library module

- **Lane**: A (standard-library self-hosting)
- **Status**: `PARTIALLY LIFTED` — the program-authority guard is gone; what it
  was masking is now measurable. See "2026-08-28 update" below.
- **Requested owner**: H (compiler / unified target machine)
- **Severity**: blocks the AOT and generated-C half of every standard-library
  migration slice, not one module.

## 2026-08-28 update: the guard is lifted

Round-3 lane 4 removed the refusal this packet is about. Two commits on
`work/4-multi-module-authority-00f665c5c`:

- `2eb6c863e` splits the canonical program authority from the executable slice.
  The complete reachable source-module graph is now published as the program
  authority for every module count, so `nmodules != 1` no longer refuses.
- `dc6a37e00` lets a TargetPlan carry module partitions without claiming a
  cross-module call edge, and builds those partitions for any module count.

**This packet's own prediction held exactly.** It said:

> Lifting the program-authority guard therefore does not deliver 19 working
> modules. It converts a masked refusal into a measurable one.

That is what happened. No module newly reaches generated C; every refusal that
used to read `XR_TARGET_1000 program authority` now reads as a specific target
or semantic refusal naming what is actually missing. The measurement below
replaces the masked column with the real distribution.

The remaining wall is documented as an architecture diagnosis in
`analysis/multi-module-program-authority.md` section 8.2: the ordinary
TargetPlan verify path interprets one merged table through the entry
SemanticPlan alone, and the two-module graph family reaches past that by
opening a parallel verify path built for its own narrow shape rather than by
fixing the assumption. Making multi-module plans verifiable means making that
verifier partition-aware, which is a separate piece of work.

## Exact source identity

| item | value |
|---|---|
| base commit | `bb6eac777369c915ddbde8e4fe76e622ded64d28` |
| worker branch | `work/a-stdlib-selfhost-w0-inventory-bb6eac777369` |
| tree state at measurement | clean at base |
| binary | `build/xray`, sha256 `7ef04e3249c4c250cf07dc830764cdf5e1f82ae26b4ca5762425f2bcc1e0513c` |
| build configuration | `cmake --preset default -DXRAY_STDLIB_VM_FASTPATHS=OFF -DXR_STDLIB_FROM_FILE=OFF` |
| version banner | `xray v0.9.2 (VM+AOT, TLS, arm64-darwin)` |

## Minimal case

The two programs differ by one `import` line.

Refused:

```xray
import base64

fn main() {
    print("probe")
}
```

Accepted:

```xray
fn main() {
    print("probe")
}
```

## Reproduce

```
build/xray build --native -c -o /tmp/probe.c /tmp/probe.xr
```

## Expected and actual

Expected: a program importing a standard-library module reaches C emission, so
a migration slice can compare VM and AOT on one verified program plan.

Actual: the AOT driver refuses before the program TargetPlan is built.

```
[xi-native] 2 modules (topo order):
  [0] stdlib/base64/base64.xr
  [1] /tmp/probe.xr (entry)
Error: product Program TargetPlan build failed: XR_TARGET_1000: product TargetPlan requires one canonical program authority
```

## First refusal

```
owner=target-plan-builder
family=program_authority
XR_TARGET_1000: product TargetPlan requires one canonical program authority
```

The blocking fact is the guard in `src/aot/xaot_driver.c`:

```c
if (!source_program_closure && nmodules != 1) {
```

`source_program_closure` comes from
`xa_program_semantic_closure_publish_scalar_module_graph` in
`src/frontend/analyzer/xa_program_semantic_closure.c`. That publisher admits
one bounded graph family and returns `UNSUPPORTED` for everything else:

- exactly two module specs, entry plus one dependency;
- the entry module's syntax is exactly two statements, one import and one
  function declaration;
- the dependency module's syntax is exactly one statement, an exported
  function declaration;
- the import carries exactly one member and no alias;
- the entry function body contains exactly one call and the dependency
  function body contains none;
- that call takes exactly one argument, no default arguments and no type
  arguments;
- the participating types are scalar.

No standard-library module can satisfy that shape: every `.xr` module declares
many exports, classes and enums. Once the publisher returns `UNSUPPORTED`,
`source_program_closure` stays NULL and the `nmodules != 1` guard refuses.

## Measured VM and AOT difference

Two measurements are reported separately because they answer different
questions and mixing them overstates what works.

### Import without using the module

An import alone shows the graph-size effect in isolation:

| program | modules in graph | VM | AOT |
|---|---:|---|---|
| no import | 1 | passes | passes |
| `import time` (no `.xr` source) | 1 | passes | passes |
| `import math`, `mem`, `runtime`, `regex`, `crypto`, `compress`, `http2` | 1 | passes | passes |
| `import base64` (`.xr` source) | 2 | passes | XR_TARGET_1000 |
| `import sys` (`.xr` source) | 2 | passes | XR_TARGET_1000 |

A module with no `.xr` source never enters the module graph, so the program
stays single-module. A module with an `.xr` source enters the graph and the
program becomes multi-module.

### Full contract probe for all 33 modules

Running each module's own contract probe, which actually calls the module's
symbols, gives the real number. Measured with
`scripts/probe_stdlib_backends.py` against the binary recorded above; the
no-import baseline control passed, so these outcomes are attributable to the
modules and not to the toolchain.

| AOT outcome | modules | count |
|---|---|---:|
| generated C | `time` | 1 |
| `XR_TARGET_1000` program authority | cluster, codegen, csv, datetime, encoding, http, http2, io, log, os, parallel, path, simd, sys, text, toml, url, ws, yaml | 19 |
| `XR_TARGET_1003` call target authority | compress, crypto, math, mem, prelude, regex, runtime | 7 |
| semantic refusal before the target layer | _probe, base64, net, sync, test_yield, xml | 6 |

VM passes for 26 of 33; the 7 VM failures are defects in the probe corpus and
in `parallel`, reported separately, not consequences of this blocker.

So one standard-library module out of 33 can reach generated C today.

### The refusal is masking, not the whole gap

The 19 modules refused at `XR_TARGET_1000` carry no information about which
symbols the target layer actually lacks: the product-authority guard fires
before any per-symbol target authority is consulted. The 7 single-module
probes that get past it land on the next layer instead, with a specific
`XR_TARGET_1003: call-shaped operation has no exact target authority` naming
the selector it cannot bind (`abs`, `allocZeroed`, `slice`, `compile`,
`liveBytes`, `md5`).

Lifting the program-authority guard therefore does not deliver 19 working
modules. It converts a masked refusal into a measurable one, which is what the
standard library needs in order to report a real per-symbol gap. The size of
the layer behind it is unmeasured, and this packet does not claim otherwise.

## The cohort gate cannot currently fail

`scripts/stdlib_migration.py verify <module>` is the per-slice backend
convergence gate. Measured on this tree:

```
$ python3 scripts/stdlib_migration.py verify io --root . --xray build/xray
=== Results: 0 passed, 0 failed, 2 refused, 0 skipped ===
== stdlib backend convergence: io (legacy_oracle=executable) ==
OK: 1 stdlib contract(s) are consistent; 1 have executable legacy oracles
```

Every case was refused and the gate reported OK, exit status 0. `text` behaves
the same way.

The differential runner is not at fault: it classifies a refusal as answering
nothing about agreement and leaves it to the coverage ratchet, which is a
deliberate separation. The consequence is what matters here. While AOT refuses
every module graph containing an Xray standard-library module, this gate is
green for reasons unrelated to convergence, so a passing run is not evidence
that a slice's backends agree, and a slice cannot be called READY on it.

Any accounting of standard-library progress that leans on this gate is
measuring the refusal, not the module.

## Consequence for the migration

Migrating a module from C to Xray currently removes what AOT capability it
had rather than adding to it: `time` is the one module whose contract probe
compiles to native today, and it is a whole-module native primitive with no
`.xr` source at all. Giving it an Xray body makes every importing program
multi-module and moves it into the refused set.

The completion criteria require every slice to produce same-plan VM and AOT
observations plus real generated C, so this refusal gates the evidence half of
all remaining standard-library work, including slices that are otherwise
ready. The VM half is unaffected and stays measurable.

## Requested generic capability

A program semantic closure that publishes an arbitrary acyclic module graph:
any module count, any export count, class, enum and generic exports, module
state, and calls with default and type arguments. The standard library needs
the general capability, not an extension of the bounded scalar family, because
no widening of that family reaches a real module.

The standard-library side can supply consumer fixtures for pure modules,
modules with private native leaves, module state, cross-module calls, generic,
class and enum exports, and yieldable leaves.

## Files deliberately not modified

```
src/aot/xaot_driver.c
src/frontend/analyzer/xa_program_semantic_closure.c
src/plan/**
xisa/**
```

The refusal is inside compiler-owned code. The standard-library lane read it
to identify the blocking fact and changed nothing.

## Evidence logs

| log | sha256 |
|---|---|
| `import base64` AOT | `20f2e5e6c7afc61c5e8a16fb6f7d0e11875239d96af2bc850dc3e78540262d78` |
| `import sys` AOT | `88eae96b68e38f74d8935f58ec5f5b0615a35d1cb1a8052b988a96e8622216f0` |

Logs are reproducible from the commands above against the recorded binary.
