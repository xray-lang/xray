# Blocker: VM and AOT implement different semantics for the same standard-library leaves

- **Lane**: A (standard-library self-hosting)
- **Status**: `BLOCKED`
- **Requested owner**: H (compiler / unified target machine)
- **Severity**: breaks the premise that the two backends consume one verified
  plan, and the divergences cannot currently be caught by differential
  testing.

## Exact source identity

| item | value |
|---|---|
| base commit | `bb6eac777369c915ddbde8e4fe76e622ded64d28` |
| worker branch | `work/a-stdlib-selfhost-w0-inventory-bb6eac777369` |
| binary | `build/xray`, built from that base with the `io.exists` debug write removed |
| platform | arm64-darwin |

## Why this is not caught today

A standard-library module with an `.xr` source cannot reach AOT execution at
all: the program becomes multi-module and refuses at `XR_TARGET_1000`, which
is the subject of the sibling packet in this directory. So every divergence
below sits behind a refusal that stops the comparison before it runs. They
were found by reading the two implementations, not by a failing test, and no
existing gate would report them.

That is the compounding risk: the standard-library lane is being asked to
produce same-plan VM and AOT evidence for each slice, on a pair of backends
that already disagree in ways nothing measures.

## Divergence 1: `io.exists`, `io.isFile`, `io.isDir` follow symlinks on one backend only

Confirmed by reading both sides and by an executed observation.

- VM: `src/os/unix/fs_unix.c:63` — `xr_fs_stat` calls **`lstat`**, and
  `xr_fs_exists`, `xr_fs_is_file`, `xr_fs_is_dir` are all built on it.
- AOT: `src/aot/xrt_io.h:133` and `:143` — `xrt_io_exists` and
  `xrt_io_is_file` call **`stat`**.

For a symlink whose target does not exist:

```xray
import io
import { Path } from path

var p = Path("<path to a dangling symlink>")
print(io.exists(p))
```

Measured on the VM: `true`. The AOT helper calls `stat`, which fails with
`ENOENT` for that path, so the same program answers `false` once it can be
compiled. Confirmed against the same file with `os.stat` and `os.lstat`:
`stat()` raises `FileNotFoundError`, `lstat()` succeeds.

`io.isFile` answers `false` on both backends here, but for different reasons,
and diverges in the opposite direction for a symlink that points at a real
file: VM reports `false` because `lstat` sees a link, AOT reports `true`
because `stat` sees the target.

## Divergence 2: `os.username` has different fallback behaviour

Confirmed by reading both sides; not executed, because `os` cannot reach AOT
execution.

- VM: returns null when `getpwuid` fails.
- AOT: `src/aot/xrt_os.h:318` — on `getpwuid` failure it falls back to `USER`,
  then `LOGNAME`.

The AOT value is non-null in environments where the VM value is null, so a
program branching on the null case takes different paths on the two backends.

## Divergence 3: `net.copyBidirectional` is a different program on each backend

Confirmed by reading both sides; not executed.

- VM: coroutine-based, and supports TLS connections.
- AOT: `src/aot/xrt_net.h:917` — `xrt_net_copy_bidirectional` returns
  `XRT_NETERR_TLS` for any connection whose `conn_kind` is `XRT_NETCONN_TLS`,
  before doing any work.

A TLS proxy that works on the VM returns a typed error under AOT. This is not
a performance difference; it is a capability the AOT path declines.

## Divergence 4: `sys.processWait` blocking discipline

Reported from a reading of the two implementations, not independently
confirmed here: the VM polls and yields, the AOT helper blocks in `waitpid`.
Under AOT this occupies the worker thread for the lifetime of the child.
Listed so it is not lost, and flagged as the one item in this packet that has
not been verified twice.

## Related contract violation found in the same files

`src/aot/xrt_os.h` gates on `_WIN32` in at least five places (lines 19, 22,
30, 64, 179), including the `xrt_os_username` divergence above. The repository
rule is that preprocessor OS checks use `XR_OS_*`. The same file uses
`XR_OS_WINDOWS` elsewhere, so the two spellings coexist and can drift apart
under a build that defines one and not the other.

## Requested action

Two decisions belong to the compiler owner, not to the standard library:

1. Which behaviour is canonical for each divergence, so that the leaf has one
   meaning rather than one per backend. The standard library can then write
   its `.xr` body against a single stated contract.
2. A differential gate that compares leaf behaviour across backends. It cannot
   be built while AOT refuses every module graph containing an Xray
   standard-library module, so it depends on the program-authority blocker.

The standard-library lane can supply the case corpus for each leaf once the
canonical answer is chosen.

## Files deliberately not modified

```
src/aot/xrt_io.h
src/aot/xrt_os.h
src/aot/xrt_net.h
src/os/unix/fs_unix.c
```

All four are compiler-owned. They were read to establish the facts above and
changed in no way.
