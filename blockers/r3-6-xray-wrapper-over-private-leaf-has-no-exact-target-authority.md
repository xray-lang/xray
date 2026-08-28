# Blocker: a call from an Xray module body into its own private native leaf has no exact target authority

- **Lane**: 6 (standard-library self-hosting, round 3)
- **Status**: `BLOCKED`
- **Requested owner**: whoever owns the exact-target-authority family in
  `src/plan/target/` — lane 5's scope. Not lane 6: `src/plan/` is outside its
  file boundary and the failure cannot even be reproduced on its branch.
- **Severity**: this is the tax on every future standard-library migration, not
  a defect in one module. It grows each time a module gains an Xray body.

## What was measured, and by whom

Lane 10 measured it on the integration branch (`8499a67cf`, all ten lanes
merged), running `tests/diff/run_backend_diff.py` directly:

| | `00f665c5c` | integration |
|---|---|---|
| passed | 161 | 150 |
| refused | 514 | 525 |
| cases that stopped building | 4 | **15** |

All eleven new entries `import mem`. Two of them — `semantics/ffi/rawptr_static_null.xr`
and `semantics/stdlib/mem_fence_shared_core.xr`, which share no other property —
give a byte-identical first refusal:

```
XR_TARGET_1003: call-shaped operation has no exact target authority
  operation=44 function=1 opcode=116 selector= intrinsic=0
  receiver-type=type-v3:13:...:fn:1:1:0:0:1;p0:...;ret:type-v3:17:...
```

`operation=44 function=1` being identical across two unrelated entry programs
means the failure is inside one shared compiled function, not in either entry.

## Which function

`stdlib/mem/mem.xr` has exactly one export taking one parameter and returning
nothing:

```xray
export fn fence(ordering: i64) { __fence(ordering) }
```

That matches the reported receiver type — a function type with one parameter
and a unit result — and `function=1` in a module whose first slot is its
initializer.

So the shape that has no exact target authority is: **an exported Xray function
in a standard-library module body calling one of that module's own private
native leaves, with no result value.** `mem.xr` has nine of that shape
(`fence`, `prefetch`, `cacheFlush`, `cacheInvalidate`, `nontemporalStore`,
`copy`, `move`, `set`, `volatileStore`).

## Why lane 6 cannot investigate further

`work/6-...-34be0379c` carries **none** of lane 4's program-authority work
(`git log 00f665c5c..HEAD -- src/frontend/analyzer/xa_program_semantic_closure.c
src/aot/xaot_driver.c` is empty). On that branch both cases stop one wall
earlier, at `XR_TARGET_1000`, so the 1003 is unreachable there. Only a tree
containing lane 4's changes can reproduce it. Lane 10's tree is currently the
only one.

## Why this is structural rather than a mem defect

Before the migration `import mem` named a whole-module native binding, the
program stayed single-module, and none of `mem`'s bodies ever reached the AOT
target layer. After it, `mem.xr` is a real module, the program is a two-module
graph, and seventeen Xray wrappers get compiled. The eleven cases did not
change; what changed is that code which previously bypassed AOT now goes
through it.

Lane 1 predicted exactly this earlier in the round: every module that gains an
Xray body enlarges this family, and the part it grows is not on any list.

**It will get much worse before it gets better.** The 6-1..6-10 decomposition
gives `regex`, `compress`, `http2` and `cluster` substantial Xray bodies, and
those four modules carry far more differential coverage than `mem` does. A
known-failures list that absorbs eleven lines per migration is the mechanism by
which the differential net went 78% hollow in the first place.

## What is needed

Exact target authority for a call whose callee is a private native leaf reached
from its own module's Xray body. `os`, `time`, `crypto` and `net` all already
have bodies of this shape; they simply have fewer differential cases pointed at
them, so the gap stayed invisible.

## Interim handling agreed with lane 10

The eleven entries go into `tests/diff/known_failures_not_comparable.txt` with
the first refusal and the attribution in the line, per the format lane 1
established this round, so that the reason and the deletion trigger are both
readable from the list itself. They are not comparable today because AOT cannot
build them — but the entry has to say why, and point here.
