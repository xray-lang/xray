# Blocker: an enum keeps a different identity depending on which source declares it

- **Lane**: A (standard-library self-hosting)
- **Status**: `BLOCKED`
- **Requested owner**: H (compiler / semantic identity)
- **Severity**: blocks moving any standard-library enum or object shape from
  the shared definition file to its module's Xray source, and the failure mode
  is silent.

## Exact source identity

| item | value |
|---|---|
| base commit | `bb6eac777369c915ddbde8e4fe76e622ded64d28` |
| worker branch | `work/a-stdlib-selfhost-w0-inventory-bb6eac777369` |

## The two identities

A nominal enum's layout id is computed by one algorithm over a `nominal_owner`
string. The two declaration sites supply different strings for the same enum.

- A `.def`-declared enum takes the module name. `xa_builtin_enum_nominal_owner`
  in `src/frontend/analyzer/xanalyzer_builtins.c` looks the owner up in the
  builtin module table, so `net.NetError` is owned by `"net"`.
- An `.xr`-declared enum in a standard-library module takes the module's full
  canonical identity. `xa_analyzer_nominal_owner_for_file` in
  `src/frontend/analyzer/xanalyzer.c` returns `analyzer->current_module_identity`,
  which is the framed form
  `stdlib-module-v1:module=3:net:path=10:net/net.xr`.

Measured layout ids for `NetError` under the two owners:

| owner | layout id |
|---|---:|
| `net` | 2184710811 |
| framed canonical identity | 2448215453 |

The compiled bytecode confirms both spellings are live: the four enums
declared in `csv.xr` carry the 48-byte framed identity, while `NetError` in
the `net` bytecode carries `"net"`.

## Why the move fails silently

The AOT backend emits an `IS`/`AS` test as a comparison against a
compile-time layout-id constant. A value boxed by C carries the `.def` id; a
`catch (e: NetError)` compiled against an `.xr` declaration expects the framed
id. The comparison is false, so the handler does not run. Nothing reports a
mismatch: the program simply stops catching an error it used to catch.

Variant ordinals are unaffected -- the variant order carries over unchanged --
which makes the failure harder to spot, because the enum looks correct
everywhere it is inspected by name.

## The C side cannot construct an Xray-declared enum at all

There is one path for C to build a standard-library enum value:
`xr_stdlib_enum_type_get` in `src/module/xstdlib_runtime_cache.c`, which consults only the
table generated from the definition file. An enum declared in `.xr` has no
entry there, so C could not produce a value even if the identity matched.

Removing the definition also breaks the generator outright: it exits with
`net.__copyBidirectional: unknown aot_enum net.NetError`, because the AOT
method emitter requires an `aot_enum` for a paired-result return.

## A second defect found on the same path

`net_publish_error` in `stdlib/net/net.c:1303` opens with:

```c
XrEnumType *type = net_error_type(X);
if (!type)
    return;
```

When the enum type is unavailable the function returns having published
nothing, and its caller at `net.c:1629` then sets the result to null. A typed
error degrades into a null value with no diagnostic at any stage. This is
fail-open on a path whose whole purpose is to publish a typed failure, and it
is what would turn the identity mismatch above into a null dereference rather
than a reported error.

## Consequence for the migration

Every remaining public native entry in `cluster` is a type shape
(`ClusterDelivery`, `ClusterInfo`, `ClusterNodeInfo`, `ClusterNodeState`,
`ClusterTlsStatus`), and `net.NetError` is the same case. None can move while
an enum's identity depends on which file declares it. The callable surface of
both modules has already moved; this is what remains.

## Requested capability

One nominal identity for a standard-library enum regardless of declaration
site, so a module may declare its own error type in Xray and C leaves may
still construct it. Either the `.xr` declaration adopts the module-name owner,
or the definition-file path adopts the canonical identity, but the two have to
agree before any type shape can move.

`net.NetError` additionally needs its producer converted first: today only
`__copyBidirectional` publishes it from C. Returning a code and classifying it
in `net.xr` -- the shape `__connectFd` and `__lastConnectCode` already use --
would remove the C producer and is worth doing on its own, since it also
deletes the hand-copied `NetErrorVariant` table at `net.c:1259`.

## Files deliberately not modified

```
src/frontend/analyzer/xanalyzer.c
src/frontend/analyzer/xanalyzer_builtins.c
src/aot/xi_cgen_dispatch_helpers.inc.c
tools/stdlibgen/stdlibgen.py
```
