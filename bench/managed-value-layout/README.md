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
| `record` | Construct `{x:i,y:i+1}` 3,000,000 times and sum fields | `9000000000000` |
| `map` | Seed `{"seed":0}`, insert and look up 200,000 `"k${i}"` keys | `19999900000` |

These reproduce the operation counts and values of the original report. The
Map constructs and drops a new allocated string on **every** insert and lookup;
keys are not precomputed, interned, or replaced with integers. The raw-C control
uses the same decimal conversion, FNV hash, load factor, probing and growth.
Both array implementations start empty and double capacity from four with
`xr_realloc`; neither reserves the final size.

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

Heap objects begin with one fixed header: local RC/state, allocation extent,
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

## COW, borrow, cleanup and future execution integration

Each logical heap copy owns a real RC reference. This experiment supports local,
single-threaded references only. Shared atomic RC and conversion to local RC are
not implemented and must follow the existing language rules before promotion.
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
cl /std:c11 /O2 /DNDEBUG /DXR_OS_WINDOWS /I <source>/src main.c layout.c verify.c escape.c /Fe:managed.exe
cl /std:c11 /O2 /DNDEBUG /DXR_OS_WINDOWS /I <source>/src raw.c raw_keys.c /Fe:raw.exe
```

Use separate output directories and record actual commands, provider, source
hashes, executable hashes, stdout/stderr and failures. There is no timer in the
executables. Run `managed.exe verify` before requesting a coordinated measurement
window. Full workloads and ratios require a fixed-input, alternating-order plan
and must retain all samples; this directory does not create a new gate or change
existing performance thresholds.

Production integration, general carriers, shared RC, class identity, for-ref,
external imports, Root/unwind integration, SAN, cross-platform compilation,
module-summary serialization and CI performance blocking remain **OPEN**. No
micro-VM was implemented; sharing the layout with product execution is a design
proposal pending actual integration and validation.
