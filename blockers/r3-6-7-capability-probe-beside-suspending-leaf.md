# A non-suspending leaf call beside a suspending one refuses the *importing* module

- **Lane**: 6-7 (http2 rewrite, round 3)
- **Base**: `34be0379c`
- **Branch**: `work/6-7-http2-xray-34be0379c`
- **Status**: worked around in `stdlib/http2/http2.xr`; the compiler defect stands

## What happens

Put a call to a non-yieldable stdlib leaf anywhere in a function that also
reaches a yieldable leaf, and the module compiles fine — but every module that
`import`s it is refused:

```
XR_SEM_0019: coroutine state count disagrees with grounded call authority
    function=23 operation=1300 opcode=116 selector= expected=0 actual=1 (func=<main>)
```

`opcode=116` is `XI_ISNULL`. The operation blamed is a null check in the
*importing* module's top level, which has nothing to do with either leaf.

## Minimal shape

In `stdlib/http2/http2.xr`, `__supported()` is a plain leaf and `__connect` /
`__send` / `__recv` are `vm_binding: "yieldable"`:

```xray
export fn request(...) -> (...) {
    _validateRequest(method, headerNames, headerValues)   // no leaf calls
    var target = _parseTarget(url)                        // no leaf calls
    if (!__supported()) { throw Http2Error.TlsUnavailable }   // <-- this line
    var handle = __connect(target.host, target.port, timeoutMs)
    ...
}
```

With that one line, `stdlib/http/http.xr` — which does `import http2` — fails
to compile with the message above. Delete the line and both modules compile.
`http2.xr` itself compiles either way.

## What it is not

Measured, each independently:

| hypothesis | test | result |
|---|---|---|
| the `try`/`catch` around the suspending call | removed it | still fails |
| the `try`/`catch` in `http.xr` | removed it | still fails |
| `http.xr` calling `http2.request` at all | replaced the call with a literal | still fails |
| the yieldable leaves themselves | replaced every leaf call with a constant | still fails |
| module size / the constant tables | added the tables alone | compiles |
| the `Exchange` class and its methods | added them alone | compiles |
| `_exchange`, the orchestration function | added it alone | compiles |
| the timeout guard | added it alone | compiles |
| calling the `supported()` wrapper instead of the leaf | swapped it | **still fails** |
| moving the probe after `__connect` fails | moved it | **still fails** |
| moving the probe into a function with no suspending call | moved it | **still fails** |

So it is not the leaf's position, not the wrapper, and not the shape of the
guard: it is the presence of a capability-leaf call anywhere reachable from a
function that also reaches a suspending leaf.

Note also that `net.xr` has suspending exports (`dial` reaches
`__connectFd`, which is yieldable) and `http.xr` imports it without trouble —
so "a module with a suspending export" is not by itself the trigger.

## Workaround in place

`http2.request` does not probe the capability. `http.xr` already asks
`http2.supported()` before calling it (`stdlib/http/http.xr`,
`_singleHttp2Request`), so the check is not lost; without a TLS provider the
connect simply fails and the answer is `ConnectionFailed` rather than
`TlsUnavailable`. `Http2Error.TlsUnavailable` stays in the error set and stays
mapped in `http.xr`, because the mapping is total and the variant becomes
reachable again as soon as the defect is fixed.

## Delete this file when

`stdlib/http2/http2.xr`'s `request` can call `supported()` (or `__supported()`)
before `__connect` and `stdlib/http/http.xr` still compiles. The check to
restore is the `if (!supported()) { throw Http2Error.TlsUnavailable }` line at
the top of `request`.

## Where to look

- `src/plan/semantic/xr_semantic_verify.c:4940-4995` raises it; `expected` comes
  from `operation_is_static_suspend()` plus the call-target kind, `actual` from
  `work.state_counts[operation]`.
- The `dependency_deferred` branch just above deliberately skips
  `XR_SEM_CALL_TARGET_SOURCE_EXPORT`, so a cross-module suspending export is
  meant to be tolerated here. The operation that is *not* tolerated is an
  `XI_ISNULL` in the importer's `<main>`, which suggests the state-count and the
  authority disagree about which operation the deferred fact belongs to.
