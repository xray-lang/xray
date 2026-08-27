# Blocker: the program-authority guard masks the real frontier and reports zero unlock

- **Lane**: B (current census / refusal evidence)
- **Status**: `BLOCKED` on a capability this lane must not implement
- **Requested owner**: H (compiler / unified target machine)
- **Severity**: the largest cluster in the census unlocks nothing; the same guard also
  prevents the default build configuration from producing a compiler at all.

Directly relevant to `work/h-c0a-general-source-graph`, which is generalizing the source
module graph closure. That is the right direction; this packet measures what will be
behind it.

## Exact source identity

| item | value |
|---|---|
| base commit | `bb6eac777369c915ddbde8e4fe76e622ded64d28` |
| branch head at measurement | `5cc0daf3888836731a5baee09a0fe69f14fe6db1` |
| worker branch | `work/b-current-census-evidence` |
| tree state | clean before and after; the experiment build was reverted |
| manifest | `build/current-refusals-5cc0daf38888.json`, sha256 `7b941de9c27f01d6fd0a207e8d4f23b9387d9af1602a51ae7647b8022f419f3b` |

## The guard

`src/aot/xaot_driver.c:2561`, introduced by `38176c749` (2026-08-26):

```c
if (!source_program_closure && nmodules != 1) {
    aot_bundle.error_msg =
        "XR_TARGET_1000: product TargetPlan requires one canonical program authority";
    xaot_survey_target_plan_refusal("program_authority", aot_bundle.error_msg);
    ...
    goto fail_free_ir;
}
```

It fires before `xaot_build_program_target_plan`, so no target-plan layer runs. The only
closure publisher, `xa_program_semantic_closure_publish_scalar_module_graph`, accepts a
two-module shape with one import member, one exported unary i64 function, and exactly one
call. No real program matches.

The same commit rewrote two driver tests from asserting build success to asserting
refusal (`test_driver_auto_discovers_package_summary_payloads` became
`test_driver_rejects_package_summary_graph_without_program_authority`), so the gates
stayed green across the change.

## Coverage

Of 519 refused cases, 131 are genuinely multi-source-module and 101 of those are stopped
by this guard. Single-module cases hitting it: 0, consistent with `nmodules != 1`.

Multi-source-module cases among the 142 comparable cases: **0**. The twelve passing cases
that spell `import` all resolve to a single source module — the import names a native or
extern declaration. Verified by compiling each and checking for the
`[xi-native] N modules (topo order)` line, printed only when `nmodules > 1`
(`src/aot/xaot_driver.c:2258`).

So multi-module AOT availability is zero, not partial.

## Measured effect of lifting the guard

Controlled experiment: delete the seven guard lines, rebuild, re-run the same 101 cases,
restore. The restored `src/aot/xaot_driver.c` is byte-identical to the base
(sha256 `d92954960de17cc4bdab92fc6510e0512b1852c03c59a4f194598fcb7fd1c2bf`), the tree is
clean, and the guard's behaviour was confirmed to return.

| | guard present | guard removed |
|---|---|---|
| cases that build | 0 | **0 of 101** |
| refusal events | 101, exactly one per case | **1533**, mean 15.2 per case, deepest 154 |

New first refusals:

| cases | family | code |
|---:|---|---|
| 68 | `calls_and_adapters` | XR_TARGET_1003 |
| 16 | `source_namespace_storage` | XR_TARGET_1001 |
| 10 | no structured refusal | none |
| 5 | `program_build` | XR_TARGET_1003 |
| 1 | `direct_local_go_callee_storage` | XR_TARGET_1001 |
| 1 | `source_class_object_storage` | XR_TARGET_1002 |

`source_namespace_storage` appears nowhere in the census. It is a whole family that is
invisible while the guard stands; its cases are concentrated in `semantics/modules/` and
`stdlib/sys_*`.

## What this means for scheduling

The census scores this cluster lead = solo = member = 101, which reads as the single
largest target. It is not a target at all in unlock terms: it is a gate that compresses a
roughly 15x deeper refusal stack into one line. Lifting it converts unmeasured depth into
measurable work; it does not by itself make any case comparable.

`program_authority` and `calls_and_adapters + program_build` are consecutive segments of
one path, not competing candidates.

## Boundary of this measurement

The experiment removes the guard, so `source_program_closure` stays null and the
closure-gated PSC verification block right after it is skipped. Generalizing the closure
instead — the `h-c0a-general-source-graph` approach — routes through that verification
first, so some of these 101 cases may stop there rather than in the target-plan layer.
The distribution above is therefore the target-plan frontier reached by bypassing the
closure, and is a close but not exact prediction of the frontier reached by publishing
one.

## Reproduce

```
build/xray build --native -c -o /tmp/probe.c tests/diff/cases/basic/strings.xr
```

Expected today: `XR_TARGET_1000: product TargetPlan requires one canonical program
authority`, one refusal event. With the guard removed the same case reports
`XR_TARGET_1003: source-export call authority is incomplete` under
`calls_and_adapters` and `program_build`.

Set `XRAY_COLLECT_ALL_REFUSALS=1` for the structured rows.

## Also blocked by this guard

`cmake --preset default` without `-DXRAY_STDLIB_VM_FASTPATHS=OFF` cannot produce a
compiler. `XRAY_STDLIB_VM_FASTPATHS` defaults to `ON`, and the generated fastpath harness
`main.xr` imports 25 modules, so generating it hits this guard and the build stops with
no `xray` executable.

## Files deliberately not modified

`src/aot/xaot_driver.c`, `src/frontend/analyzer/xa_program_semantic_closure.c`, every
baseline and allowlist, and all compiler semantics. The experiment produced no commit.
