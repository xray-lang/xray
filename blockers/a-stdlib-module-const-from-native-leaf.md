# Blocker: a module-level constant cannot be initialised from a native leaf

- **Lane**: A (standard-library self-hosting)
- **Status**: `BLOCKED`, worked around
- **Requested owner**: H (compiler / module loader)
- **Severity**: forces every migrated platform constant to become a function,
  which changes the public shape of the symbol and may cost an allocation per
  read.

## Exact source identity

| item | value |
|---|---|
| base commit | `bb6eac777369c915ddbde8e4fe76e622ded64d28` |
| worker branch | `work/a-stdlib-selfhost-w0-inventory-bb6eac777369` |
| binary | `build/xray` built from that base |

## Minimal case

In `stdlib/os/os.xr`, with `__platform` declared as a private leaf:

```xray
export const platform = __platform()
```

## Actual

```
[WARNING] [module] E0504: circular dependency: os -> os (xmodule.c:1287)
[Uncaught Panic] E0502: import: failed to load module 'os'
[WARNING] [module] failed to execute embedded stdlib bytecode from
  'stdlib/os/os.xr' for module 'os' (xmodule.c:1046)
Error: Module 'os' not found
```

The same leaf called from an exported function in the same file works:

```xray
export fn platform() -> string { return __platform() }
```

So the leaf binding is fine. What fails is evaluating a module-level constant's
initialiser during that module's own load, which re-enters the loader for the
module already in flight and is reported as a self-referential dependency.

## Why it matters to the migration

A platform constant is the natural shape for `os.platform`, `os.arch`,
`os.sep` and `os.eol`: the value cannot change for the life of the process,
and the AOT side already treats them as constants with a helper-evaluated
value. Migrating them to Xray had to convert all four to functions, and the
25 call sites in the test corpus moved with them.

The repository has a link filetest, `no_alloc_generated_string_const_ok.xr`,
whose whole purpose is to hold `os.platform` to a no-allocation contract. It
cannot measure the change: it already fails on the unmodified base commit,
along with 530 of the 566 AOT filetests. So the allocation cost of the
conversion is unmeasured, and this packet does not claim it is zero.

Every remaining module with platform or capability constants will hit the same
wall.

## Requested capability

Evaluate a module-level constant initialiser without re-entering the module
loader for the module being loaded, so a constant may call a private leaf its
own module declares.

## Files deliberately not modified

```
src/module/xmodule.c
src/module/xmodule_graph.c
```

The refusal is in the module loader. It was read to identify the blocking
fact and changed in no way: the standard-library lane took the function form
and moved on rather than modifying the loader for one symbol shape.
