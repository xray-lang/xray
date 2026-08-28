# Blocker: four cases the refusal baseline expects to build are now refused

- **Lane**: B (current census / refusal evidence)
- **Status**: `BLOCKED` on capability this lane must not implement
- **Requested owner**: H (compiler / unified target machine)
- **Severity**: ratchet regression. Three independent root causes, all in the AOT
  representation refinement layer. One of them was fixed and un-baselined six days before
  it broke again.

## Exact source identity

| item | value |
|---|---|
| base commit | `bb6eac777369c915ddbde8e4fe76e622ded64d28` |
| branch head at measurement | `5cc0daf3888836731a5baee09a0fe69f14fe6db1` |
| tree state | clean |
| manifest | `build/current-refusals-5cc0daf38888.json`, sha256 `7b941de9c27f01d6fd0a207e8d4f23b9387d9af1602a51ae7647b8022f419f3b` |

None of the four cases is listed in `tests/diff/known_failures_not_comparable.txt` or
`tests/diff/known_failures.txt`. Observed refusals 519 against 515 listed.

Reproduce any of them with:

```
build/xray build --native -c -o /tmp/probe.c <case>
```

Add `XRAY_COLLECT_ALL_REFUSALS=1` for the structured rows.

## Root cause 1: PHI join of reference-capable values (2 cases, 4 affected)

- `tests/diff/cases/semantics/optimizer/break_inside_match_arm.xr`
- `tests/diff/cases/semantics/optimizer/continue_inside_match_arm.xr`

Both carry a byte-identical first refusal:

```
owner=aot-representation-refinement family=refinement_definition_oracle
definer-opcode=180 use-opcode=197 value-machine=23 value-shape=0 value-flags=80
```

`definer-opcode=180` is `XI_PHI`, `use-opcode=197` is `XI_RELEASE`, and
`value-machine=23` is `XR_MACHINE_REP_COUNT`, meaning the target plan bound no
representation for the value at all. Diagnostic
`XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE`.

The language shape is a `break` or `continue` leaving a `match` arm inside a loop, which
leaves a join in the loop header whose incoming references do not converge to one type.

The same cluster key covers two further cases that are already baselined —
`tests/diff/cases/match/struct_destructure.xr` and
`tests/regression/11_coroutine/1130_linked_scope.xr` — so fixing it recovers four cases,
not two.

Suspected introduction: `e71237fc8` "Close tagged reference AOT transport" (2026-08-25),
which routed `XI_PHI` to a predicate requiring every incoming edge's type to equal the
PHI result type, structurally excluding the join family the earlier predicate existed to
cover. The two cases had been removed from the shrink-only refusal baseline on 2026-08-20,
which under that baseline's rules means they were building then. Not build-verified by
this lane.

## Root cause 2: a narrow use-site branch preempts the general fallback (1 case)

- `tests/diff/cases/semantics/types/union_as_conversion.xr`

```
owner=aot-representation-refinement family=refinement_use_site_oracle
selector=i64 definer-opcode=132 use-opcode=174 value-machine=19 value-shape=1 value-flags=80
```

Three refusal events, selectors `i64`, `string`, `i32`. `use-opcode=174` is `XI_AS`.

`oracle_use_storage` previously had no `XI_AS` case, so checked conversions from a union
were admitted by the general fallback whose comment states that a use site naming no
carrier consumes the operand in the storage it already occupies. `00a9621e1` "Publish
scalar parse failures through typed errors" (2026-08-23) added a `case XI_AS:` gated on a
NumberParseError identity predicate, which a union conversion cannot satisfy, so the new
narrow branch now rejects operands the general fallback used to accept.

This is the recurring shape where a newly added, more specific storage answer takes over
values that were previously handled correctly. The regression surface is likely wider
than this one case.

## Root cause 3: materialization verify with no structured evidence (1 case)

- `tests/diff/cases/semantics/stdlib/exact_scalar_parse_contract.xr`

```
Error: module representation materialization failed for '...':
XR_AOT_REFINEMENT_REPRESENTATION record=0 value=5 operation=6
```

`first_refusal` is null, `refusals` is empty, and the manifest classifies it as
`opaque-refusal-without-structured-diagnostic`. This case passes the authority build and
fails later in `xr_aot_representation_materialization_verify`, a stage that emits no
owner/family row, so nothing that fails there can be attributed.

Minimised: the trigger is `??` on a nullable, unrelated to `tryParse`. Single-line
reproduction:

```
print(i64.tryParse("42") ?? -1)
```

Introduction: evidence insufficient. The case was added by `e96c04acf` (2026-08-23)
without a baseline entry, but the `??` gap is long-standing — 23 baselined cases use `??`,
and 23 of the 24 materialization-stage failures are already listed. Both "added without
baselining" and "later regression" remain consistent with the evidence.

## Common finding

None of the suspected introducing commits (`e71237fc8`, `00a9621e1`) touched a baseline
file. Build refusals stopped counting as backend-diff divergences on 2026-08-16
(`b285df795`), so refusal-only regressions do not show red on the diff gate, and a
regression absorbed by rewritten unit tests shows nothing at all. The gate discipline
that let these through needs attention alongside the three fixes.

Root cause 3 also states a standing evidence gap: the materialization verify stage owes
an owner/family structured refusal, without which its failures cannot be clustered.

## Files deliberately not modified

`src/aot/refine/**`, `src/aot/xaot_driver.c`, every baseline and allowlist, and all
compiler semantics.
