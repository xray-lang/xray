# Blocker: a module-qualified builtin class silently constructs a dead object

- **Lane**: A (standard-library self-hosting)
- **Status**: `BLOCKED`
- **Requested owner**: H (compiler / code generation)
- **Severity**: silent wrong answer. The caller receives an object that looks
  constructed and is inert, with no diagnostic at any stage.

## Exact source identity

| item | value |
|---|---|
| base commit | `f78ca940aeecd8d2512520a46d5e3391ec75b117` |
| worker branch | `work/a-stdlib-selfhost-r2-f78ca940a` |
| binary | `build/xray`, built from that base |

## Minimal case

The two programs differ only in how the class is named.

```xray
import { Semaphore } from sync
var s = Semaphore(2)
print(s.available)      // 2
print(s.tryAcquire())   // true
print(s.available)      // 1
```

```xray
import sync
var s = sync.Semaphore(2)
print(s.available)      // 0
print(s.tryAcquire())   // false
print(s.available)      // 0
```

Both compile and run without a warning. The second answers as though the
semaphore were constructed with zero permits, so every acquire fails forever.

`CountdownLatch` behaves the same way: `sync.CountdownLatch(3).remaining` is 0
where `CountdownLatch(3).remaining` is 3. Both are runtime object types. A
plain Xray class in the same module is unaffected: `sync.Mutex(5).value` and
`Mutex(5).value` both answer 5.

## Where it goes wrong

Instrumented at three points and rebuilt for each, then removed:

| probe | named import | module-qualified |
|---|---|---|
| `semaphore_construct` in `src/coro/xsemaphore_native.c` | fires, `nargs=1 arg0=2` | never fires |
| `OP_CALL` class branch in `src/vm/xvm_dispatch_call.inc.c` | fires, `klass=Semaphore nargs=1` | never fires |
| `vm_invoke_module` entry in `src/vm/xvm_props.c` | n/a | never fires |

The module-qualified form reaches neither VM path. Nothing calls the class's
static `call` constructor, which is where the permit count is read. What the
caller receives is an ordinary instance with its fields at their zero value,
which is why every getter answers 0 and every operation fails.

Since neither runtime dispatch path is entered, the decision is made before
the VM runs: the call is compiled as an ordinary class construction rather
than as an invocation of the registered native constructor. The analyzer does
have a dedicated path for this shape --
`xa_module_member_class_instance_type` in
`src/frontend/analyzer/xanalyzer_visitor_call.c` -- so the type is inferred;
it is the construction that is not routed.

## Why it matters to the standard library

`sync` exposes five runtime object types this way: `Semaphore`,
`CountdownLatch`, `EventCount`, `WorkQueue` and `ResultGroup`. They are the
module's whole native surface. Every one of them is reachable through the
module-qualified spelling, which is the form a reader would reach for after
`import sync`, and every one of them is wrong in that form.

This also blocks the migration itself. The five are listed as this module's
public native symbols, and deciding how they should be published -- re-exported
from Xray, or kept as runtime primitives with a working call path -- cannot be
settled while one of the two spellings silently produces a dead object.

## Requested capability

Compile a module-qualified construction of a runtime object type through the
same class-call rule as the unqualified form, so the registered native
constructor runs and receives the arguments. Failing that, refuse the shape at
compile time; answering with a zero-valued instance is the one outcome that
must not remain.

## Files deliberately not modified

```
src/frontend/analyzer/xanalyzer_visitor_call.c
src/vm/xvm_dispatch_call.inc.c
src/vm/xvm_props.c
src/coro/xsemaphore_native.c
```

All four were instrumented to locate the fault and restored; the working tree
carries no change to them. The standard-library lane can supply the case
corpus for all five types once the construction path is settled.
