# Migrating the 31 remaining public native symbols into `.xr`

Read-only investigation in the worktree
`/Users/xuxinglei/workspace/xray-lang/worktrees/a-stdlib-selfhost-w0-inventory-bb6eac777369`,
using the pre-built `./build/xray` (`xray v0.9.2 (VM+AOT, TLS, arm64-darwin)`). Nothing in the
repository was modified; every experiment ran out of a scratch directory or a scratch copy of
`stdlib/`.

Machine-readable companion: `public_native_migration.json`.

> **Concurrency note.** This investigation started at `2d5bb8dec` and, mid-run, a concurrent session
> advanced the worktree to `40bc46213` (8 commits). I made no repository writes — `git status` is
> clean throughout. I re-verified afterwards that **none of the files I cite line numbers in were
> touched** by those commits: `stdlib/defs/core.def`, `stdlib/os/os.c`, `src/aot/xrt_os.h`,
> `src/aot/xi_cgen_dispatch_helpers.inc.c`, `stdlib/net/net.c`, `stdlib/cluster/cluster.c`,
> `src/frontend/analyzer/xanalyzer.c`, `src/base/xglobal_indices.h`, `tools/stdlibgen/stdlibgen.py`,
> `scripts/check_stdlib_boundary.py`, `scripts/check_binary_public_native_readiness.py` and
> `stdlib/stdlib_boundary.toml` are all unchanged. The one changed file I make a claim about,
> `scripts/stdlib_symbol_inventory.py`, was re-run against the new HEAD and the sync blind spot
> still holds (44 sync rows, none of them the five class names). See §5.1 for how the new
> `blockers/a-stdlib-vm-aot-leaf-semantic-divergence.md` packet relates to the defects below.

---

## 0. First: what `public_native` and `manual_public_native` actually mean

Both fields are *derived assertions*, not editorial lists. `scripts/check_stdlib_boundary.py:309-338`
computes

```
actual   = def_public_symbols(module)  |  manual_public_native
declared = public_native
assert declared == actual
```

where `def_public_symbols` (`scripts/stdlib_manifest.py:90-106`) walks every non-internal entry
parsed out of `stdlib/defs/core.def`.

So:

* **`public_native`** = "everything core.def publishes for this module, plus whatever the C factory
  publishes behind core.def's back".
* **`manual_public_native`** = exactly that second set. Each entry is additionally verified to appear
  as a quoted string literal inside the module's `factory_source` (checker lines 334-339).

**For `sync` they are the same five names because `sync` has no `module sync { … }` block in
core.def at all** (verified by parsing the file). `def_public_symbols("sync")` is empty, so
`public_native == manual_public_native` is the only value that satisfies the equality. This is an
artifact of the equality check — not two policies, and not redundancy that can be deleted.

### The `is_internal` asymmetry that decides how each symbol can leave the list

`tools/stdlibgen/stdlibgen.py` derives internality **differently per entry kind**:

| core.def entry kind | `is_internal` | consequence for migration |
| --- | --- | --- |
| `fn` (`StdlibMethodEntry`) | `visibility == "internal"`, defaulting to internal when the name starts with `__` (stdlibgen.py:593-594) | renaming `fn x` → `fn __x` is enough to drop it from `public_native` |
| `handle` / `object` / `enum` | `name.startswith("__")` | same — renaming works |
| **`const`** (`StdlibConstEntry`) | **property absent** (stdlibgen.py:125-143) | `getattr(..., "is_internal", False)` is always `False`; **every core.def const is unconditionally public**, even if named `__x`. Only deletion removes it. |
| **`native_class`** (stdlibgen.py:265-276) | **property absent** | renaming `NetConn` → `__NetConn` does **not** clear it |
| **`class_method`** (stdlibgen.py:293-304) | **property absent** | same |

`handle __ExecResult` in `module os` is the working precedent: it is already invisible to
`public_native` purely because of its name, and `os.xr` wraps it in `export final class ExecResult`.
That is the shape `net` would need — but `native_class` does not get the same treatment, so `net`
additionally needs a stdlibgen/checker change.

### Two independent pins on the os/net sets

Any change to `os` or `net` must edit **both**:

* `scripts/check_stdlib_boundary.py:224-241` — `check_l2_thinning`, `expected_native`
* `scripts/check_binary_public_native_readiness.py:26-40` — `L2_PUBLIC_NATIVE`

Both hard-code `"os": {"arch", "eol", "platform", "sep"}` and the full 11-name net set as *required*.
Today these checkers actively forbid the migration.

### A blind spot worth recording

`scripts/stdlib_symbol_inventory.py` reads core.def, so the five `sync` symbols **do not appear in
its per-symbol rows at all** (verified: 44 sync rows, none of them `Semaphore`/`CountdownLatch`/
`EventCount`/`WorkQueue`/`ResultGroup`). Any completion gate built on that inventory — including
`scripts/check_stdlib_full_xray_completion.py` — is blind to them.

---

## 1. The 31 symbols by category

| category | count | symbols |
| --- | --- | --- |
| `pure_constant` | **4** | `os.platform`, `os.arch`, `os.sep`, `os.eol` |
| `thin_abi` | **10** | `net.NetConn.{fd,close,isClosed,isTLS}`, `net.NetListener.{fd,port,close,isClosed}`, `cluster.self`, `cluster.nodes` |
| `has_policy` | **4** | `cluster.send`, `cluster.info`, `cluster.monitor`, `cluster.discover` |
| `runtime_object` | **13** | `sync.{Semaphore,CountdownLatch,EventCount,WorkQueue,ResultGroup}`, `net.{NetConn,NetListener,NetError}`, `cluster.{ClusterDelivery,ClusterNodeState,ClusterTlsStatus,ClusterNodeInfo,ClusterInfo}` |

`runtime_object` is the biggest bucket and the four given categories flatten a distinction that
turns out to decide everything, so it splits three ways:

| sub-kind | symbols | definable in `.xr` today? |
| --- | --- | --- |
| **VM builtin global** | the 5 `sync` classes | **No** — reserved global slots + IR name table + analyzer path gate |
| **native class** | `NetConn`, `NetListener` | **No** — name is a reserved builtin (`E0350`) |
| **generated descriptor (pure data)** | `NetError`, `ClusterDelivery`, `ClusterNodeState`, `ClusterTlsStatus`, `ClusterNodeInfo`, `ClusterInfo` | **Yes — experimentally confirmed** |

That last row is the surprise of this investigation: six of the thirteen "type" symbols are *not*
compiler-blocked at all. Their obstacle is that C constructs their values, not that Xray cannot
declare them.

Two `thin_abi` notes so the label is not read as "trivial":

* `NetConn.close` / `NetListener.close` are one-line delegates into `xr_net_conn_close`
  (`src/io/xnet_handle.c:137-151`), which frees the TLS connection, deregisters from netpoll and
  closes the socket. That teardown is not expressible in Xray and stays native — but it stays in the
  **already existing** `__close` leaf (`core.def:3501`), so nothing needs splitting.
* `cluster.nodes` walks a C linked list under `nodes_lock`. The loop is inherent to "enumerate the
  table", so the leaf is still `__nodes(): Array<string>` and the `.xr` side is a passthrough.

---

## 2. Migration slices, smallest first

Each slice is independently landable — it does not depend on any later slice.

### Slice 1 — `os` platform constants (4 symbols) · **smallest** · **recommended first**

New private leaves: **2**.

```
fn __platform  { signature: "(): string"  vm: "os_platform"  aot: "xrt_os_platform" }
fn __arch      { signature: "(): string"  vm: "os_arch"      aot: "xrt_os_arch"     }
```

`os.xr` gains:

```xray
fn _sepFor(p: string) -> string {
    if (p == "windows") { return "\\" }
    return "/"
}
fn _eolFor(p: string) -> string {
    if (p == "windows") { return "\r\n" }
    return "\n"
}

export const platform: string = __platform()
export const arch: string     = __arch()
export const sep: string      = _sepFor(platform)
export const eol: string      = _eolFor(platform)
```

`sep` and `eol` need **no leaf at all** — they are fully derivable from `platform`.

Files to change:

| file | change |
| --- | --- |
| `stdlib/defs/core.def` | delete the four `const` blocks at :1862-1901 (between `handle __ExecResult` and `fn __getenv` inside `module os {` at :1858); add two `fn __*` blocks |
| `stdlib/os/os.xr` | add the four exports and two helpers above |
| `stdlib/os/os.c:1100-1145` | `get_platform`/`get_arch` become the bodies behind the two new VM cfuncs; delete `get_sep`/`get_eol` |
| `src/aot/xrt_os.h:114-128` | delete `xrt_os_sep`/`xrt_os_eol`; keep `xrt_os_platform`/`xrt_os_arch` as the `aot:` targets |
| `src/aot/xi_cgen_dispatch_helpers.inc.c:13767-13777` | delete the hand-written `os` const-field branch |
| *regenerate* | `src/stdlib/xstdlib_defs_generated.h:403-406`, `src/stdlib/xstdlib_vm_bindings_generated.inc.c:264-267`, `src/aot/xstdlib_aot_methods_generated.inc.c:256-259` |
| `stdlib/stdlib_boundary.toml` | `os` → `public_native = []` |
| `scripts/check_stdlib_boundary.py:226` | expected `os` set → empty |
| `scripts/check_binary_public_native_readiness.py:28` | expected `os` set → empty |
| meta gates | `contract_freeze` CMakeLists.txt sha256 re-anchor + shrink-only residue inventory, per the repo's ratchet rules |

Consumers to re-verify (**no stdlib `.xr` module consumes any of the four** — verified by grep):
`tests/stdlib/contracts/os/probes/{current,legacy}.xr:3-4`,
`tests/diff/cases/semantics/stdlib/os_query_system_direct.xr`,
`tests/regression/10_stdlib/1160_os_basic.xr:11,17,23,29`, plus the `os.platform` readers in
`sys_dylib_direct.xr`, `io_chmod_shared_core.xr`, `1192_io_extended.xr`, `1161_os_extended.xr`.

**This slice fixes a live defect** — see §4.

### Slice 2 — `cluster` thin passthroughs (3 symbols) · **small**

`cluster.self`, `cluster.nodes`, `cluster.discover`. No new C is written: the existing C functions
keep their bodies, only the core.def entry names change to `__self` / `__nodes` / `__discover`, and
`cluster.xr` gains three one-line exports. Arguably even smaller than slice 1.

```xray
export fn self() -> string           { return __self() }
export fn nodes() -> Array<string>   { return __nodes() }
export fn discover() -> bool         { return __discover() }
```

Note `discover`'s declared type changes from `()` to `bool` — a latent-defect correction, see §4.
`self` and `nodes` are `aot_direct: false` today (`core.def:3780`, `:3792`), i.e. VM-only; migrating
does not fix that, but it makes the gap explicit at the `.xr` surface.

### Slice 3 — `cluster.send` + `cluster.monitor` + `ClusterDelivery` (3 symbols) · **medium**

Leaves: `__send` (must keep `ret: enum_i64`, `aot_enum: ClusterDelivery`, `aot_direct: true`,
`link_object: true`), and `monitor` splits into `__monitorNode` / `__monitorCoro` so the arity
dispatch — the only Xray-side policy — moves out of C (`stdlib/cluster/cluster.c:1516-1539`).

```xray
export enum ClusterDelivery { Accepted, InvalidTopic, InvalidEnvelope,
                              Unavailable, Overloaded, Disconnected }

export fn send(topic: string, envelope: move Buffer) -> ClusterDelivery {
    return __send(topic, envelope)
}

export fn monitor(name: string, coroName: string? = null) -> Channel? {
    if (coroName == null) { return __monitorNode(name) }
    return __monitorCoro(name, coroName!)
}
```

Risks: `ClusterDelivery`'s layout id `4282747530` is baked into
`src/aot/xstdlib_aot_methods_generated.inc.c:221`, so an `.xr`-declared enum must reproduce the same
nominal id or the row must be re-derived. And `monitor`'s return type must become `Channel?` — an
API correction, see §4.

`send`'s transport policy (topic-trie walk, per-target buffer copy, back-pressure classification,
hop-limited flood — `stdlib/cluster/cluster_topic.c:391-543`) stays native. This slice is also the
natural moment to decide the duplicate topic grammar: `cluster.xr:102-137` and
`stdlib/cluster/cluster_topic_core.h:25-92` implement the same rules independently today.

### Slice 4 — `net.NetError` (1 symbol) · **medium**

The enum declaration is free (`export enum NetError { … }` compiles today, see E8), and `net.xr`
**already owns the classification**: `_classify` at `stdlib/net/net.xr:31-41` maps the frozen
`_CODE_*` integers (`net.xr:15-23`) onto variants.

The work is entirely on the C side: `stdlib/net/net.c:1259-1320` (`NetErrorVariant`,
`net_error_variant_index`, `net_error_type`, `net_publish_error`) builds `NetError` values in C for
the yieldable primitives' error channel. Every such site must publish a portable i64 code instead
and let `_classify` do the mapping. Downstream: `stdlib/http/http.xr:21` and `stdlib/ws/ws.xr:13`
import `NetError`.

### Slice 5 — `cluster.info` + the four shapes (5 symbols) · **large**

Not compiler-blocked: a three-class nested shape with an `Array<ClusterNodeInfo>` field and a nested
`ClusterTlsStatus` compiles and runs in `.xr` today (E8). The cost is the snapshot ABI — `ClusterInfo`
carries 10 fields including an array of 16-field `ClusterNodeInfo`, so ~30 values must cross the
boundary, either through internal `handle` types or through indexed scalar accessors, while
preserving `cluster_info_fn`'s lock discipline. Also fixes three leak-on-error paths (§4).

### Slice 6 — `net` handles and their 8 methods (10 symbols) · **largest**, two variants

**6A — API-preserving.** Rename the native classes to `__NetConn`/`__NetListener`, declare
`export final class NetConn { _h: __NetConn … }` in `net.xr`, re-express the 8 methods over
`__fd`/`__close` plus two new leaves `__isTLS` and `__listenerPort`. Blocked twice over (see §3), and
even once unblocked it touches ARC/ownership certificates, coro analysis, `xvalue_typeid`, the AOT
value tags `XR_TAG_NET_CONN`/`XR_TAG_NET_LISTENER`, and the two independent NetConn implementations
(VM `XrNetConn` vs AOT `xrt_net_conn_object_t`). All 27 `net.xr` functions gain an unwrap, and the
handle types flowing through `http.xr`/`ws.xr` change.

**6B — API-reducing.** Delete the 8 `class_method` entries outright. `net.fd(handle)` and
`net.close(handle)` already exist in `net.xr` and are what all of stdlib actually uses: a grep for
`.fd()` / `.close()` / `.isClosed()` / `.isTLS()` / `.port()` across `stdlib/**/*.xr` finds only
`io.xr:127`, `io.xr:147` (file handles) and `ws.xr:1175` (a `WsConn`). **The 8 net methods have zero
stdlib callers.** Cheap, but it is a published-API removal needing an explicit product decision, and
it does not clear `NetConn`/`NetListener` themselves.

### Slice 7 — `sync` primitive classes (5 symbols) · **BLOCKED, recommend reclassification**

See §3. `stdlib/sync/sync.c` is a 54-line namespace re-export of runtime primitives that sit on the
same footing as `Array`/`Map`/`Set`/`Channel`/`Atomic`. The honest move is a manifest/checker edit —
record them as prelude-level runtime primitives outside the stdlib `public_native` ledger, or accept
them as a permanent per-symbol-approved native surface — not a migration.

---

## 3. What the compiler blocks, with the actual error text

### 3.1 The five `sync` classes — hard blocked

They are **VM builtin globals**, not module symbols:

```
src/base/xglobal_indices.h:46  #define XR_GLOBAL_VAR_WORKQUEUE      28
src/base/xglobal_indices.h:47  #define XR_GLOBAL_VAR_RESULTGROUP    29
src/base/xglobal_indices.h:49  #define XR_GLOBAL_VAR_COUNTDOWNLATCH 31
src/base/xglobal_indices.h:50  #define XR_GLOBAL_VAR_SEMAPHORE      32
src/base/xglobal_indices.h:51  #define XR_GLOBAL_VAR_EVENTCOUNT     33
```

with a hard-coded name→slot table at `src/ir/xi_lower_expr.c:118-122`, population at
`src/api/xisolate_full.c:214-227`, and — decisively — a **hard-coded file-path visibility gate**:

```c
/* src/frontend/analyzer/xanalyzer.c:1931-1939 */
static void xa_register_sync_native_class_symbols(XaAnalyzer *analyzer, const char *file,
                                                  XaScope *scope) {
    if (!xa_path_is_sync_stdlib_module(file))
        return;
    static const char *names[] = {"Semaphore", "CountdownLatch", "EventCount", "WorkQueue",
                                  "ResultGroup"};
    ...
}
```

These names resolve **only** inside `stdlib/sync/sync.xr`.

**Experiment E5** — using them from an ordinary program with no import:

```xray
const s = Semaphore(1)
const q: WorkQueue<i64> = WorkQueue<i64>(2, 2)
const r: ResultGroup = ResultGroup(1)
```

```
error[E0365]: undefined type 'WorkQueue'
error[E0365]: undefined type 'ResultGroup'
error[E0351]: Undeclared variable 'Semaphore'
error[E0351]: Undeclared variable 'CountdownLatch'
error[E0351]: Undeclared variable 'EventCount'
error[E0351]: Undeclared variable 'WorkQueue'
error[E0351]: Undeclared variable 'ResultGroup'
```

**Experiment E6** — declaring one in `.xr`:

```xray
export final class Semaphore {
    _n: i64
    constructor(n: i64) { this._n = n }
    tryAcquire() -> bool { return this._n > 0 }
}
var s = Semaphore(1)
```

```
[xcompiler] Xi IR pipeline failed at semantic-plan: XR_SEM_0002: builtin type identity is not exact (func=<main>)
```

Note this is a *pipeline abort*, not a diagnostic — the analyzer let it through. Compare E7 below,
which produces a clean error for the same class of mistake. That asymmetry is a defect in its own
right.

### 3.2 `NetConn` / `NetListener` — blocked on a reserved name, cleanly

**Experiment E7:**

```xray
export final class NetConn {
    _fd: i64
    constructor(fd: i64) { this._fd = fd }
    fd() -> i64 { return this._fd }
}
```

```
error[E0350]: class 'NetConn' redeclares the builtin 'NetConn' — builtin names are reserved, choose a different name
error[E0351]: Undeclared variable 'fd'
error[E0380]: type 'NetConn' has no member '_fd'
error[E0380]: type 'NetConn' has no member 'fd'
```

Same with `import net` present. The native class must be renamed before `.xr` can own the name — and
because `StdlibNativeClassEntry`/`StdlibClassMethodEntry` carry no `is_internal`, the rename alone
would not clear `public_native`; stdlibgen and the checker need name-based internality for those two
kinds too.

Secondary consequence: `.xr` cannot add a method to a native class, so **none of the 8 net class
methods can migrate independently of the class**. They move together or not at all (or they are
deleted — variant 6B).

### 3.3 The 8 net methods and the 6 data types — **not** blocked

**Experiment E9** — the wrapper shape itself works:

```xray
import net
export final class Conn {
    _h: NetConn
    constructor(h: NetConn) { this._h = h }
    fd() -> i64 { return net.fd(this._h) }
    close() { net.close(this._h) }
}
const l = net.listen(0)
print("listener port>0=" + (l.port() > 0).toString())
```
```
listener port>0=true
wrapper class over NetConn compiles=true
```

**Experiment E8** — the six generated-descriptor names are free:

```xray
import cluster
export enum ClusterDelivery { Accepted, InvalidTopic, InvalidEnvelope,
                              Unavailable, Overloaded, Disconnected }
```
```
delivery ok=true
```
```xray
import net
export enum NetError { Timeout, Closed, Reset, Refused, Dns, Tls, Io, Invalid, Cancelled, OutOfMemory }
```
```
neterror ok=true
```
```xray
import cluster
export final class ClusterTlsStatus { enabled: bool  … }
export final class ClusterNodeInfo  { name: string  phi: f64  … }
export final class ClusterInfo      { self: string  nodes: Array<ClusterNodeInfo>  tls: ClusterTlsStatus  … }
var i = ClusterInfo("a", [ClusterNodeInfo("n", 1.5)], ClusterTlsStatus(true))
```
```
info ok=a,1,true
```

So `NetError`, `ClusterDelivery`, `ClusterNodeState`, `ClusterTlsStatus`, `ClusterNodeInfo` and
`ClusterInfo` are **C-construction problems, not compiler-capability problems**.

### 3.4 AOT is already blocked at baseline for all four modules

**Experiment E1:**

```
$ ./build/xray build --native os_probe.xr
[xi-native] Building: os_probe.xr
[xi-native] 2 modules (topo order):
  [0] .../stdlib/os/os.xr
  [1] .../os_probe.xr (entry)
Error: product Program TargetPlan build failed: XR_TARGET_1000: product TargetPlan requires one canonical program authority
```

Independently confirmed by `tests/diff/known_failures_not_comparable.txt:426`, which already lists
`tests/diff/cases/semantics/stdlib/os_query_system_direct.xr`. No slice here can regress AOT for
these modules, because AOT does not work for them today.

---

## 4. Can `os` be the first slice that actually lands? — **Yes**

Explicit verdict: **yes, and it is the right first slice.** The evidence:

1. **No compiler capability is missing.** The exact shape works at runtime today.

   **Experiment E2** — a module exporting consts computed at module-init, consumed across a module
   boundary:

   ```xray
   // plat.xr
   import os
   fn _sepFor(p: string) -> string {
       if (p == "windows") { return "\\" }
       return "/"
   }
   export const PLATFORM: string = os.platform
   export const CWD: string      = os.getcwd()     // a native-backed call
   export const SEP: string      = _sepFor(PLATFORM)
   export const EOL: string      = _eolFor(PLATFORM)
   ```
   ```
   $ ./build/xray run c2/main.xr
   PLATFORM=darwin
   CWD_ok=true
   SEP=/
   EOL_len=1
   ```

2. **The analyzer accepts the shape inside `stdlib/os/os.xr` itself.** **Experiment E3** — a scratch
   copy of `stdlib/` with `export const probeCwd: string = __getcwd()` (a *same-module private
   native*) plus derived consts appended to `os.xr`, run under `XRAY_STDLIB_PATH`:

   ```
   compiled
   ```

   Negative control, appending `export const deliberateTypeError: i64 = __getcwd()`:

   ```
   .../stdlibcopy/os/os.xr:64:14: error: Type 'string' is not assignable to type 'i64'
   ```

   The negative control proves the analyzer really read the overridden copy.

3. **The name collision is a non-issue.** **Experiment E4** — appending `export const platform / arch
   / sep / eol` to the copied `os.xr` *while core.def still declares all four* compiles, and a
   consumer type-checks against the `.xr` declaration:

   ```
   $ XRAY_STDLIB_PATH=<copy> ./build/xray run c7.xr     # len(os.platform)>0, len(os.sep)>0, var s: string = os.sep …
   true / true / true / true / true
   $ XRAY_STDLIB_PATH=<copy> ./build/xray run c8.xr     # var n: i64 = os.sep
   error: Type 'string' is not assignable to type 'i64'
   ```

   `src/module/xmodule.c:890-893` documents this: script-layer exports are added to the module export
   table and *can override C module exports*. So the slice can even be staged `.xr`-first before the
   core.def deletion.

4. **No stdlib consumer.** Grep confirms no `stdlib/*.xr` reads `os.sep`/`os.eol`/`os.platform`/
   `os.arch`. `path.xr:18` has its own `export const sep = "/"` (POSIX canon, unrelated). Every
   consumer is a test or the language spec.

5. **AOT cannot regress** (§3.4).

6. **It fixes a real defect.** `os.arch` has two divergent C implementations:

   ```c
   /* stdlib/os/os.c:1117 — the VM path */
   static const char *get_arch(void) {
   #if defined(__aarch64__) || defined(_M_ARM64)  return "arm64";
   #elif defined(__x86_64__) || defined(_M_X64)   return "x64";
   #elif defined(__i386__)   || defined(_M_IX86)  return "x86";
   #elif defined(__arm__)    || defined(_M_ARM)   return "arm";
   #else                                          return "unknown";
   #endif
   }
   ```
   ```c
   /* src/aot/xrt_os.h:86 — the AOT path, using the project's XR_ARCH_* macros */
   ... additionally returns "ppc64", "loongarch64", "riscv64"
   ```

   `LANGUAGE_SPEC.md:6623` documents `"ppc64"` and `"riscv64"` as valid `os.arch` values, so **the VM
   is spec-noncompliant on those targets today**. Collapsing to one `.xr` const over one `__arch()`
   leaf removes the divergence by construction.

### The one thing not verified

`XRAY_STDLIB_PATH` steers the **analyzer** only; the VM still executes embedded bytecode
(`XR_STDLIB_FROM_FILE:BOOL=OFF` in `build/CMakeCache.txt`, and `src/module/xmodule.c:917-950` prefers
embedded bytecode over the file fallback). So **runtime execution of a stdlib module's top-level
native call was not observed** — that needs a rebuild, which this investigation did not perform.
E2 is the runtime evidence for the structurally equivalent shape in a user module, and
`src/module/xmodule.c:1023` (`xr_vm_execute_module(isolate, code)`) shows the stdlib script's full
top-level proto is executed at load; `_probe.xr:13` (`export const probeSeed = 148`) and
`xml.xr:192` already prove top-level const execution works for stdlib modules. Combining those, the
residual risk is low but **needs confirming with a build**.

Related unverified point for slices 3-4: whether an `.xr`-declared enum reproduces the nominal layout
id that the generated AOT rows bake in (`ClusterDelivery` 4282747530, `NetError` 2184710811).

---

## 5. Defects found in passing

Each is pre-existing and independent of the migration; several are *fixed by* the corresponding
slice.

| # | where | what |
| --- | --- | --- |
| 1 | `stdlib/os/os.c:1117` vs `src/aot/xrt_os.h:86` | `os.arch` has two divergent tables; VM cannot report `ppc64`/`loongarch64`/`riscv64` that `LANGUAGE_SPEC.md:6623` documents. *(fixed by slice 1)* |
| 2 | `stdlib/os/os.c:1100-1114` | `get_platform` tests `#elif defined(XR_OS_BSD)` **twice**; the `"openbsd"` branch is unreachable dead code. *(fixed by slice 1)* |
| 3 | `sync.Semaphore` namespace construction | `sync.Semaphore(n).tryAcquire()` is `false` for any `n`; the named-import form is correct. See E10 below. |
| 4 | analyzer + Xi plan | redeclaring a `sync` builtin name in `.xr` aborts at semantic-plan with `XR_SEM_0002` instead of the clean `E0350` that `NetConn` produces (E6 vs E7). |
| 5 | `core.def` `discover` vs `stdlib/cluster/cluster.c:1206-1215` | declared `(): ()` but the C returns `xr_bool(rc == 0)` — discovery start/failure is silently discarded. *(fixed by slice 2)* |
| 6 | `core.def:3799` `monitor` vs `stdlib/cluster/cluster.c:1516-1539` | declared return type is non-nullable `Channel`, but the C returns null on five failure paths. *(fixed by slice 3)* |
| 7 | `stdlib/cluster/cluster.c:1407-1408, 1462-1463, 1505-1506` | `cluster_info_fn` leaks `node_arr` and partially built instances on three early-return-under-lock paths. *(fixed by slice 5)* |
| 8 | `stdlib/cluster/cluster.c:1182-1203` | `cluster_nodes` comment says "connected node names" but the loop lists every node regardless of `node->state`; it also allocates VM objects inside `nodes_lock`. |
| 9 | `stdlib/net/net.c:2362` vs `src/io/xnet_handle.c:93` vs `src/aot/xrt_net.h:1389` | three spellings of `isClosed`; the VM class method omits the `fd < 0` check the other two have. |
| 10 | `src/aot/xrt_net.h:292-300` vs `stdlib/net/net.c:2352` | the AOT close path calls `shutdown(fd, SHUT_RDWR)` before closing; the VM path does not. |
| 11 | `src/aot/xi_cgen_dispatch_helpers.inc.c:13777-13793` | the cgen hard-codes `log.DEBUG/INFO/WARN/ERROR/FATAL` integer constants that **do not exist in `stdlib/log/log.xr` at all** (log.xr uses a `LogLevel` enum) — apparently dead. |
| 12 | `stdlib/cluster/cluster.xr:102-137` vs `stdlib/cluster/cluster_topic_core.h:25-92` | topic grammar validation and matching implemented twice, independently, in Xray and in C. |
| 13 | `scripts/stdlib_symbol_inventory.py` | the per-symbol inventory reads core.def and therefore cannot see the 5 `sync` symbols at all. |

### 5.1 Relation to the concurrent `blockers/a-stdlib-vm-aot-leaf-semantic-divergence.md` packet

That packet, committed into this worktree at `9f83cafa8` while this investigation was running,
records four VM/AOT leaf divergences: `io.exists`/`isFile`/`isDir` (`lstat` vs `stat`),
`os.username` fallback to `USER`/`LOGNAME` on AOT only, `net.copyBidirectional` declining TLS on
AOT, and `sys.processWait` blocking discipline. It also flags `_WIN32` gating in `src/aot/xrt_os.h`
where the repo rule is `XR_OS_*`.

**No overlap with the table above.** Defects 1, 2, 9 and 10 are additional VM/AOT divergences in the
same family that the packet does not list, and defect 2 is a VM-internal dead branch rather than a
divergence. Defect 1 is the one that matters most for this lane, because it sits inside slice 1.

Defect 1 also has a contract-rule twin of the packet's `_WIN32` note, on the stdlib side rather than
the compiler side: `src/base/xplatform.h:37-49` defines `XR_ARCH_X86_64`/`XR_ARCH_ARM64`/
`XR_ARCH_RISCV64`, `src/aot/xrt_os.h:86` uses them, and `stdlib/os/os.c:1118-1120` bypasses them for
raw `__aarch64__` / `__x86_64__` / `_M_ARM64`. That is exactly why the two `arch` tables drifted.

**Experiment E10** (defect 3), verbatim:

```xray
import sync
import { Semaphore, CountdownLatch } from sync
var a = Semaphore(2)
print("named a1=" + a.tryAcquire().toString())
print("named a2=" + a.tryAcquire().toString())
print("named a3=" + a.tryAcquire().toString())
var b = sync.Semaphore(2)
print("ns b1=" + b.tryAcquire().toString())
print("ns b2=" + b.tryAcquire().toString())
var l2 = sync.CountdownLatch(1)
l2.done()
print("ns latch wait=" + l2.wait().toString())
```
```
named a1=true
named a2=true
named a3=false
ns b1=false
ns b2=false
ns latch wait=true
```

The namespace-qualified form drops the permit-count argument and yields a 0-permit semaphore.
`CountdownLatch` behaves identically both ways, so this is specific to `Semaphore`.

---

## 6. Summary table

| module | symbols | verdict |
| --- | --- | --- |
| **os** | 4 | **Ready.** 2 new leaves, 0 compiler blockers, all evidence in §4. Land first. |
| **cluster** | 11 | 3 ready now (slice 2), 3 medium (slice 3), 5 large but unblocked (slice 5). No compiler blockers anywhere in cluster. |
| **net** | 11 | 1 medium (`NetError`, slice 4). 10 blocked on the reserved builtin name plus the missing `is_internal` on `native_class`/`class_method`; or resolvable cheaply by deleting 8 zero-caller methods (variant 6B, a breaking API decision). |
| **sync** | 5 | **Blocked and not migratable as stated.** Reclassify rather than migrate. |
