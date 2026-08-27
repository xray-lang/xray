# Blocker: AOT refuses every module graph that contains an Xray standard-library module

- **Lane**: A (standard-library self-hosting)
- **Status**: `BLOCKED`
- **Requested owner**: H (compiler / unified target machine)
- **Severity**: blocks the AOT and generated-C half of every standard-library
  migration slice, not one module.

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

| program | modules in graph | VM | AOT |
|---|---:|---|---|
| no import | 1 | passes | passes |
| `import time` (no `.xr` source) | 1 | passes | passes |
| `import math` (no `.xr` source) | 1 | passes | passes |
| `import mem` (no `.xr` source) | 1 | passes | passes |
| `import runtime` (no `.xr` source) | 1 | passes | passes |
| `import regex` (no `.xr` source) | 1 | passes | passes |
| `import crypto` (no `.xr` source) | 1 | passes | passes |
| `import compress` (no `.xr` source) | 1 | passes | passes |
| `import http2` (no `.xr` source) | 1 | passes | passes |
| `import base64` (`.xr` source) | 2 | passes | XR_TARGET_1000 |
| `import sys` (`.xr` source) | 2 | passes | XR_TARGET_1000 |
| io contract probe (`.xr` sources) | 4 | not measured | XR_TARGET_1000 |

A module with no `.xr` source never enters the module graph, so the program
stays single-module and AOT accepts it. A module with an `.xr` source enters
the graph and the program becomes multi-module.

## Consequence for the migration

Migrating a module from C to Xray currently removes that module's AOT
capability rather than adding to it: a program that compiles to native today
because `time` is a whole-module native primitive stops compiling the moment
`time` gains an `.xr` semantic source. The completion criteria require every
slice to produce same-plan VM and AOT observations plus real generated C, so
this refusal gates the evidence half of all remaining standard-library work,
including slices that are otherwise ready.

The VM half is unaffected and stays measurable.

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
