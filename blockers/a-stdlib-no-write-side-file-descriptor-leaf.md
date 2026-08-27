# Blocker: the io module has no write-side file-descriptor leaf

- **Lane**: A (standard-library self-hosting)
- **Status**: `BLOCKED`
- **Requested owner**: H (compiler / runtime)
- **Severity**: stops the remaining io migrations. Every one of them is a
  drain loop, and a drain loop cannot be written in Xray without a handle to
  write through.

## Exact source identity

| item | value |
|---|---|
| base commit | `f78ca940aeecd8d2512520a46d5e3391ec75b117` |
| worker branch | `work/a-stdlib-selfhost-r2-f78ca940a` |

## What is missing

The io module declares a read-side handle triple:

```
__fileOpen(path: Path): i64
__fileRead(handle: i64, maxBytes: i64): Array<u8>?
__fileClose(handle: i64): bool
```

There is no write-side equivalent. The full leaf list carries no
`__fileOpenWrite`, no `__fileWrite` and no way to append through a handle.

## What that blocks

Four public symbols are still implemented in C purely because the loop that
retries a short write has nowhere else to live:

| symbol | C shape | why it cannot move |
|---|---|---|
| `io.appendFile` | `fopen("ab")` + drain loop + checked `fclose` | no handle to write through |
| `io.writeFile` | same, plus truncate | same |
| `io.writeFileBytes` | same, over bytes | same |
| `io.writeStdout` / `writeStderr` | drain loop + `fflush` | the stream has no handle form |

Each is the same shape: open, write until the buffer is consumed or an error
stops it, close and report whether the close also succeeded. The loop and the
"a failed close means a failed write" rule are policy and belong in the module
body; only the individual write is a host call.

The read side shows this works. `io.readLines` needs no leaf at all now,
because `BufReader` and `LineIterator` are built on the read-side triple in
Xray; its C implementation was deleted in this branch as dead code.

## Requested capability

A write-side handle triple with the same shape as the read side:

```
__fileOpenWrite(path: Path, append: bool): i64
__fileWrite(handle: i64, data: Slice<u8>, offset: i64): i64
__fileClose(handle: i64): bool
```

`__fileClose` already exists and can be shared. The write leaf should report
the number of bytes accepted so the module body can decide whether to continue,
which is what the shared C loop does today.

One constraint worth stating: the current `readFile`/`writeFile` leaves take an
io_uring path on Linux and park the coroutine until completion. A write-side
leaf that cannot suspend would move those calls back to blocking the worker.
The leaf therefore needs the same yieldable effect the read side already
carries, or the migration trades a policy improvement for a concurrency
regression.

## Measured cost of the alternative

A pure-Xray drain over repeated appends was measured on an 8 MB stream during
the io leaf survey recorded in this repository: the C loop takes 0.01s, an
Xray loop appending element by element takes 0.17s, and building the result by
concatenation is quadratic at 2.75s. Reading and writing whole files is not a
place to accept a 17x cost, which is why this is filed rather than worked
around.

## Files deliberately not modified

```
src/coro/**
src/runtime/**
stdlib/defs/core.def (the write-side declarations)
```

Adding a yieldable leaf touches the coroutine and runtime boundary, which is
compiler-owned. The standard-library lane can supply the module-side signature,
the Xray bodies and the case corpus once the capability exists.

## What the lane did instead

Moved on to the migrations that need no new capability: the recursive listing,
the intermediate-directory creation, the recursive removal, the timestamp
fallback and the temporary-path root, all of which are in this branch.
