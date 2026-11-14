# Fixed managed-value layout experiment

This standalone C11 experiment is not linked into Xray, installed, or registered
with CMake/CTest. It does not define a product ABI or enable language features.
The production baseline is commit `320f43a18f83dd130958dbec73e9cf89c260aac2`.
The earlier canonical-access experiment remains separately archived at
`refs/archive/m2-canonical-array-experiment-833b943273`.

## Inputs and comparison boundary

| Command | Work | Exact stdout |
|---|---|---|
| `push` | Push 5,000,000 i64 values, then sum them | `12499997500000` |
| `push-prepared` | The complete same push input inside a trusted prepared window | `12499997500000` |
| `push-direct` | The complete same push input with scalar locals in the hot loop | `12499997500000` |
| `record` | Construct `{x:i,y:i+1}` 3,000,000 times and sum fields | `9000000000000` |
| `map` | Seed `{"seed":0}`, insert and look up 200,000 `"k${i}"` keys | `19999900000` |
| `map-take` | The complete same Map input, transferring each owned insertion key | `19999900000` |

These reproduce the operation counts and values of the original report. The
Map constructs and drops a new allocated string on **every** insert and lookup;
keys are not precomputed, interned, or replaced with integers. The raw-C control
uses the same decimal conversion, FNV hash, load factor, probing and growth.
Both array implementations start empty and double capacity from four with
`xr_realloc`; neither reserves the final size.

The original `map` mode and borrowed `xr_mvl_map_set` remain controls. The separate
`map-take` mode transfers the constructor's one owned reference and clears the
caller slot on success. A missing key receives that reference without a retain;
an equal key keeps its original storage and drops the incoming owner after
commit. Any aliases keep their real references, including negative shared-counter
references. Failure leaves the binding, key owner and net domain charge intact.
Both modes allocate all 400,001 Strings including seed and lookup temporaries.
No SSO, interning, key cache, or new Source/LIR ownership inference is involved.

The take API requires a valid caller-owned slot; its pointer type and RC do not
prove that precondition. A null slot is rejected, but arbitrary external pointers
or borrowed references cannot be certified by this prototype. Product lowering
would need its separately verified ownership plan. State checks include actual
aliases, equal distinct/same-pointer keys, both counter bands, every discovered
allocation failure in growth and COW preparation, and partial child-retain failure
at both signed RC limits. Negative-band fixtures expose no graph to a thread.

`push-prepared` peels the first real write after its RHS. Only then does the
shared Array kernel check physical uniqueness, grow or COW, and return a writable
data/length/capacity descriptor. The remaining loop uses that descriptor and
commits the canonical length after every element store. Cold growth accepts and
returns descriptor values. It never receives or caches the caller's binding
address. Ordinary and prepared Array writes use the same COW/growth kernel and
the same 32-byte object; the descriptor owns no reference or second payload.

This is a trusted caller-owned, alias-free window, not permission inferred from
RC==1. Retain, alias observation, rebinding and observing calls close the window
before executing. Close by discarding the descriptor, evaluate RHS effects, read
the current binding, and prepare again at the actual write. The internal cold
allocator/COW operation belongs to the window and returns refreshed pointers.
Its successful COW result leaves the old caller reference unconsumed: immediately
publish the returned owner, then drop that old reference. An explicit abort
drops the fresh replacement instead. Do not insert a fallible call between
preparation and publication; stable Root cleanup across unwind remains absent.

State checks cover empty ranges without pre-COW, real RC without a pin, explicit
abort/refund, all COW allocation failures, failed growth, refreshed pointers,
RHS rebinding, bounds before allocation, and invalidated descriptors before
retain/alias/call/rebind. These are native C protocol tests, not a language loan,
module effect proof, Root/longjmp test, or support for class/for-ref.

`push-direct` retains the original `push-prepared` as an ABI-cost control. Its
hot iterations hold owner/data/length/capacity as ordinary C locals and store
elements plus canonical length directly. Only cold preparation constructs and
receives descriptors; no large result is built at each fast iteration. The
shared range helper moves one real reference in and returns the current owner
even after failure. Zero iterations do not prepare a borrow. State checks run
that same helper with a committed prefix, local/shared aliases, zero count and
every measured allocation failure. The fixed CLI workload still appends all
5,000,000 values. No volatile, forced inlining, compiler flag or allocator change
is used, and this introduces no product access authority or new runtime carrier.

`record` is an inline two-i64 value. Ordinary scalar replacement and loop
folding are allowed in both implementations. A folded loop or startup-limited
result cannot establish an improvement in heap allocation. `record-escape` is
a separate control: an opaque consumer in another translation unit retains the
record beyond its producing call, aliases it, then retires the previous owner.
It adds observable ownership work and is not a replacement for the original
record input. Do not use volatile or compiler barriers to handicap the original.

`layout` prints sizeof, alignment and field offsets for the actual provider.
`verify` checks COW snapshots, real references, bounds-before-allocation, failed
preparation leaving bindings unchanged, key lifetime, and final domain balance.
It is a state check, not a timing sample. No timing result exists yet.

## Layout and reusable algorithms

Heap objects begin with one fixed 32-byte header: signed atomic RC, flags, allocation extent,
allocator domain, and a stable `TypeOps` pointer. `TypeOps` are static for this
executable and therefore outlive every object. They are used at final drop, not
looked up on every get/set. There is no XIR value, type arena, owner hash table,
tagged value, VM frame, or per-element admission traversal.

* Array<i64>: header, `i64 *data`, `size_t length`, `size_t capacity`.
* String: header, byte length, cached byte hash, trailing UTF-8 bytes and NUL.
  The stored length, rather than the NUL, defines equality.
* Record: inline `{i64 x,y}` payload; escaping storage adds a header around that
  payload. Copying the payload never copies an RC header.
* Map<string,i64>: header, length/capacity, and a slot array containing cached
  hash, owned String pointer and inline i64. Empty slots have a null key.
  Deletion/tombstones and ordering are outside these three inputs.

Allocation goes through `src/base/xmalloc.h`; FNV comes directly from
`src/base/xhash.h`. Decimal formatting and capacity arithmetic are shared pure
helpers in `algorithms.h`. The raw-C control shares these algorithms, not a
managed-value runtime. Domain accounting is constant-time on allocation/free;
ordinary element access does not consult it.

Counter-only primitives come from `src/base/xsigned_rc.h`, shared with the
unchanged 72-byte production XIR object header. Positive counts use local relaxed
loads/stores; negative counts use atomic CAS. Unique `-1` becomes local `+1` with
acquire ordering. Counter code has no XIR dependency. The former 40-byte local
header and its complete measurements remain a separate fixed baseline.

The intentional representation differences are managed heap headers/domain
accounting, real RC on copied/retained objects, and COW when an object is shared.
The raw control manually transfers unique key storage and has no RC or COW.
Growth/rehash/hash/equality/key bytes are identical; the managed slow helper also
reloads/reprobes its current binding at a growth boundary. Neither difference is
a claim that the compiler has proved hoisting or escape analysis.

## COW, borrow, cleanup and future execution integration

Each logical heap copy owns a real RC reference. The fixed workloads use local
objects. Counter operations support the production local/shared bands and unique
handoff, but complete graph publication and cross-execution APIs are not
implemented. A negative counter alone is not shared-binding mutation authority.
Array SET/PUSH and Map SET receive already evaluated RHS/key/value arguments,
then resolve the current binding. Slow paths prepare all fallible allocation
and key references before publishing. Failure preserves the old pointer,
contents and net domain charge. Successful push commits length immediately.
No zero-iteration pre-COW or additional pin reference is introduced.

A borrowed data/span does not increase RC and cannot outlive its real owner.
Grow/rebind/retain/call/alias observation invalidates a cached uniqueness or data
certificate unless the checked access plan proves otherwise. The prototype does
not implement language loans, hoist proof, or cross-function summaries. A future
checked plan must describe owner access/retain/escape, writes, returned origins
and invalidation in a composable module interface, with link-time closure.

VM operations can hold these same typed object pointers in the VM's existing
register-owner slots; generated C can use them in stable native activation-owner
slots. They share payload and drop semantics, not an execution frame. Cleanup
slots must remain discoverable across allocation and longjmp. A slow transaction
must register prepared owners before a possible trap, commit the actual binding
without failure, or abort on unwind. No automatic-stack address or relocatable
owner-table bucket may be cached. The prototype uses explicit return cleanup;
integration with Root, scopes and longjmp is **not implemented**.

The only current input boundary is a constructor copying bounded caller bytes.
Future external import must validate an explicitly owned handle/packet and its
extent, layout, allocator domain, type/code lease, children and RC state before
creating trusted pointers. Reading a header through an arbitrary pointer is not
validation. Dynamic TypeOps/code must remain leased until final object drop,
even after Program retirement. This is a design obligation, not a tested API.

## Building and evidence

Compile the two standalone executables without LTO, using the same provider,
optimization flags and allocator backend for both:

```
cl /std:c11 /experimental:c11atomics /O2 /DNDEBUG /DXR_OS_WINDOWS /I <source>/src main.c layout.c verify.c escape.c /Fe:managed.exe
cl /std:c11 /O2 /DNDEBUG /DXR_OS_WINDOWS /I <source>/src raw.c raw_keys.c /Fe:raw.exe
```

The Windows evidence commands also use `/W4 /WX /MT /utf-8` and the existing
`_CRT_SECURE_NO_WARNINGS` definition required by the shared compatibility header.
MSVC alone uses `/wd4200` for its diagnostic on standard C11 flexible array
members; other warnings remain errors. The first command missing these settings
is preserved as a failed attempt. Domain instances and TypeOps in this prototype
have static storage duration; they outlive escaped records.

Use separate output directories and record actual commands, provider, source
hashes, executable hashes, stdout/stderr and failures. There is no timer in the
executables. Run `managed.exe verify` before requesting a coordinated measurement
window. Full workloads and ratios require a fixed-input, alternating-order plan
and must retain all samples; this directory does not create a new gate or change
existing performance thresholds.

Production integration, general carriers, shared graph publication, class identity, for-ref,
external imports, Root/unwind integration, SAN, cross-platform compilation,
module-summary serialization and CI performance blocking remain **OPEN**. No
micro-VM was implemented; sharing the layout with product execution is a design
proposal pending actual integration and validation.
