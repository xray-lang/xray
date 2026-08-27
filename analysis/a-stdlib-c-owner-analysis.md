# stdlib handwritten-C semantic owners: what the 947 functions actually do

Read-only survey of the working tree at
`/Users/xuxinglei/workspace/xray-lang/worktrees/a-stdlib-selfhost-w0-inventory-bb6eac777369`.

Base data: `python3 scripts/stdlib_symbol_inventory.py --root . --json /tmp/inv_for_analysis.json`
(generator not modified). Owner filter, matching `is_semantic_c_owner` in the script plus its
production-module restriction:

```
handwritten_c_body != '' AND xray_body == false AND kind != 'module-factory'
AND native_leaf == false AND module.audience == 'production'   ->  947 rows
```

Those 947 rows span **34 files and 17,178 lines of C** (line counts measured per owner function,
platform `#else` twins counted once).


> **Snapshot warning — the worktree is shared and the generator changed mid-investigation.**
> This analysis is pinned to the **947-row** snapshot produced by
> `scripts/stdlib_symbol_inventory.py` as it stood when this survey started. While the survey ran,
> another session committed to the same branch (`cab6b8865` 23:50, `2d5bb8dec` 23:54, `b99475032`
> 00:00 *"Count C owners by audience rather than manifest membership"*, `f4b5cdb29` / `79edb9e42` /
> `00eff6d82` 00:01, `55d7cdd5a` 00:04) and left further uncommitted edits to
> `scripts/stdlib_symbol_inventory.py` (mtime 00:06:27) and `stdlib/io/io.c` (mtime 00:06:46).
> **None of those changes were made by this investigation**, which was strictly read-only and wrote
> only to `/tmp/inv_for_analysis.json` and the scratchpad.
> The generator now reports **965** owners. The delta is purely additive — **18 added, 0 removed,
> 0 reclassified** — and all 18 are `vanishes_with_owner`. See the addendum at the end.

## Method and evidence honesty

- **380 of 947 function bodies were read verbatim** in this
  investigation (`evidence: "read_body"` in the JSON). The exact list is in
  `scratchpad/evidence.py`.
- The remaining **567** are `evidence: "extrapolated"`: classified from the
  containing file's structure plus at least one read sibling in the same family.
- **Every one of the 34 owner files had at least one function read.** Fully read files:
  `stdlib/net/io.c` (19/19), `stdlib/net/tls.c` (33/33), `stdlib/time/time.c` (11/11),
  `stdlib/runtime/runtime.c` (6/6), `stdlib/cluster/cluster_monitor.c` (9/9),
  `stdlib/cluster/cluster_auth.c` (2/2), `stdlib/net/xneterror.c` (1/1), `stdlib/sync/sync.c` (1/1).
- Weakest coverage, i.e. where I would double-check before acting: `stdlib/regex/xregex_parse.c`
  (0/35 bodies read — only `parse_atom`/`ast_*` signatures and one sibling in `xregex_compile.c`),
  `stdlib/regex/xregex_api` (`xregex.c` 0/32), `stdlib/regex/xregex_dfa.c` (0/14),
  `stdlib/crypto` AES block (0/13), `stdlib/cluster/cluster_node.c` (2/36),
  `stdlib/http2/http2_binding.c` (0/14). In every one of those the function names are
  self-describing and the file headers state the algorithm, so the *category* is safe; the
  *cohort sizing* is what could shift.

## Classification totals

| category | count | % | disposition |
|---|---:|---:|---|
| `parser_codec` | 78 | 8.2% | must migrate |
| `protocol_state_machine` | 116 | 12.2% | must migrate |
| `algorithm` | 168 | 17.7% | must migrate |
| `policy` | 122 | 12.9% | must migrate |
| `binding_glue` | 234 | 24.7% | vanishes |
| `internal_helper` | 59 | 6.2% | vanishes |
| `host_abi` | 110 | 11.6% | leaf candidate |
| `runtime_primitive` | 60 | 6.3% | leaf candidate |
| `unclear` | 0 | 0.0% | - |
| **total** | **947** | | |

`unclear` is 0. Every row got an explicit rule; nothing fell through to a default bucket.

## 1. How many must actually move to Xray?

| bucket | functions | C lines | share |
|---|---:|---:|---:|
| **Must migrate** (parser_codec + protocol_state_machine + algorithm + policy) | 481 | 11789 | 50.8% |
| **Vanishes with the owner** (binding_glue + internal_helper) | 291 | 3139 | 30.7% |
| **Possibly legitimate leaf** (host_abi + runtime_primitive) | 161 | 1840 | 17.0% |
| **Dead code, just delete** (compress_zlib.c) | 14 | 410 | 1.5% |
| total | 947 | 17178 | 100% |

Breakdown of the 481 that must migrate:

- `algorithm` 168, by module: regex 85 (compiler + NFA + DFA + API), crypto 33 (hash cores + AES),
  math 16 (numeric semantics), cluster 15 (topic trie, phi accrual, node registry), compress 12
  (LZ77 + Huffman), http2 6 (stream hash table), sys 1 (`sys_barrier_wait`).
- `policy` 122, by module: net 62 (retry/deadline/partial-progress/error classification/copy),
  cluster 27 (outq backpressure, health, monitor delivery, AOT queueing), sys 9 (process + pipe),
  http2 9 (connection pool + short-read/short-write loops), io 8 (buffering + io_uring),
  os 6 (exec pipe poll-drain), compress 1.
- `protocol_state_machine` 116, by module: cluster 73 (handshake, reader/writer, discovery,
  monitor, topic transport, AOT), http2 39 (frames + HPACK + header validation), net 3
  (TLS handshake driver + ALPN selection), compress 1 (RFC 7692 trailer strip, dead).
- `parser_codec` 78, by module: regex 37 (pattern parser + UTF-8 decode + replace templates),
  cluster 18 (frame codec + discovery announce + AOT address parse), compress 17 (bit reader/writer,
  gzip/zlib containers), crypto 3 (hex), os 2 (Windows command-line quoting), net 1 (IP literal).

Two caveats that change what "481" costs:

- **91 of the 947 are CPS scaffolding** (`*_continue` / `*_step` / `*_drive` / `*_entry` /
  `*_complete` / `*_wait` / `*_done` / `*_finish` / `*_context_destroy` / `*_ctx_free` around a
  `xr_yield_for_io` or `xr_yield_for_timeout`), of which 80 are in the must-migrate set. In Xray
  each family collapses into one `while` loop with `await`. Example: `net.__readInto` is 8 C
  functions and 172 lines (`net_read_into_continue|value|done|progress|error|complete|wait|step`,
  `stdlib/net/net.c:699-870`) that become a single loop. Distribution: cluster 40, net 29, sys 14,
  io 4, http2 2, time 1, os 1.
- **291 "vanishes" is optimistic for math.** 21 of them (`math_sqrt`, `math_pow`, `math_sin`, …)
  are `unpack -> libm -> repack`. They do not disappear; they shrink into ~21 libm leaf
  declarations that math.xr still needs.

### Where the semantics actually live

A third of the "vanishes" bucket is thin because the rule it used to hold was moved into a
header-only kernel under `src/shared/` that the VM, the AOT runtime and stdlib all include:

| shared kernel | stdlib consumer | also consumed by |
|---|---|---|
| `src/shared/xr_crypto_core.h` | `crypto.c` bindings (12 of 17 glue rows) | `src/aot/xrt_crypto.h`, `src/module/xlockfile.c`, `xnative_package.c`, `xpkg_client.c`, `src/app/toolchain/xtc_probe.c`, `xtc_runtime_manifest.c` |
| `src/shared/xr_compress_core.h` | `compress.c` checksums + bindings | `src/aot/xrt_compress.h`, `src/module/xpkg_client.c` |
| `src/shared/xr_io_core.h` | `io.c` (read-all-stream, mkdirp, remove-all, read-dir) | `src/aot/xrt_io.h` |
| `src/shared/xr_os_core.h` | `os.c`, `io.c` | `src/aot/xrt_os.h`, `src/aot/xrt_io.h` |
| `src/shared/xr_math_core.h` | `math.c` random/int-arg | `src/aot/xrt_math.h` |
| `src/shared/xr_regex_core.h` | `xregex_binding.c`, `xregex.c` (flag parsing, match slots) | `src/vm/xvm.c`, `src/aot/xrt_regex.h` |
| `src/shared/xr_sync_core.h` | `mem.c` fences | `src/aot/xi_cgen.c`, `src/aot/xrt_mem.h` |

Consequence: for `io` and `os` the stdlib C file is mostly syscall callbacks with almost no
stdlib-owned semantics left to move, and for `crypto`/`compress`/`regex` deleting the stdlib C
requires re-pointing the shared kernel's other consumers first (finding F3).

## 2. Suggested cohorts and order

A cohort = one independently acceptance-testable slice: it has its own inputs/outputs, its own
test surface, and can be flipped to Xray without touching a sibling cohort.

| wave | cohort | fns | C lines | must-migrate | files |
|---:|---|---:|---:|---:|---|
| 0 | **compress-zlib-DEAD** | 14 | 410 | 0 | `compress_zlib.c` |
| 1 | **crypto-hex** | 3 | 31 | 3 | `crypto.c` |
| 1 | **compress-checksum** | 4 | 12 | 0 | `compress.c` |
| 1 | **compress-bitstream** | 9 | 94 | 9 | `compress.c` |
| 1 | **compress-container** | 7 | 206 | 7 | `compress.c` |
| 1 | **compress-deflate** | 12 | 476 | 12 | `compress.c` |
| 1 | **math-numeric-semantics** | 16 | 225 | 16 | `math.c` |
| 2 | **regex-parser** | 35 | 889 | 33 | `xregex_parse.c` |
| 2 | **regex-compiler** | 39 | 1149 | 37 | `xregex_compile.c` |
| 2 | **regex-engine-nfa** | 14 | 858 | 14 | `xregex_nfa.c` |
| 2 | **regex-engine-dfa** | 14 | 392 | 13 | `xregex_dfa.c` |
| 2 | **regex-api** | 32 | 660 | 25 | `xregex.c` |
| 3 | **crypto-primitives** | 21 | 348 | 20 | `crypto.c` |
| 3 | **crypto-aes** | 13 | 200 | 13 | `crypto.c` |
| 3 | **cluster-auth** | 2 | 16 | 2 | `cluster_auth.c` |
| 4 | **net-error** | 10 | 91 | 10 | `net.c`, `xneterror.c` |
| 4 | **net-deadline** | 3 | 25 | 3 | `net.c` |
| 4 | **net-dns** | 1 | 5 | 1 | `net.c` |
| 4 | **net-coro-io** | 19 | 453 | 18 | `net.c` |
| 4 | **net-copy** | 13 | 288 | 13 | `net.c` |
| 4 | **net-udp** | 4 | 79 | 4 | `net.c` |
| 4 | **net-tls-handshake** | 4 | 36 | 4 | `net.c` |
| 4 | **net-blocking-io** | 19 | 432 | 11 | `io.c` |
| 5 | **io-buffering** | 3 | 34 | 3 | `io.c` |
| 5 | **io-uring** | 5 | 97 | 5 | `io.c` |
| 5 | **os-exec** | 9 | 155 | 8 | `os.c` |
| 5 | **sys-process** | 8 | 106 | 4 | `sys.c` |
| 5 | **sys-pipe** | 10 | 120 | 5 | `sys.c` |
| 6 | **http2-hpack** | 13 | 457 | 13 | `http2.c` |
| 6 | **http2-frames** | 17 | 594 | 15 | `http2.c` |
| 6 | **http2-validation** | 7 | 81 | 6 | `http2_binding.c` |
| 6 | **http2-conn** | 5 | 86 | 5 | `http2.c` |
| 6 | **http2-streams** | 6 | 86 | 6 | `http2.c` |
| 6 | **http2-transport** | 2 | 33 | 2 | `http2.c` |
| 6 | **http2-pool** | 11 | 353 | 7 | `http2_client.c` |
| 7 | **cluster-proto** | 20 | 250 | 15 | `cluster_proto.c` |
| 7 | **cluster-topic-trie** | 7 | 180 | 7 | `cluster_topic.c` |
| 7 | **cluster-topic-transport** | 7 | 232 | 7 | `cluster_topic.c` |
| 7 | **cluster-health** | 7 | 197 | 7 | `cluster_health.c`, `cluster_node.c` |
| 7 | **cluster-discovery** | 11 | 289 | 10 | `cluster_discovery.c` |
| 7 | **cluster-monitor** | 9 | 242 | 9 | `cluster_monitor.c` |
| 7 | **cluster-node-outq** | 10 | 148 | 10 | `cluster_node.c` |
| 7 | **cluster-node-io** | 16 | 369 | 14 | `cluster_node.c` |
| 7 | **cluster-handshake** | 28 | 615 | 28 | `cluster.c` |
| 7 | **cluster-runtime** | 13 | 377 | 8 | `cluster.c` |
| 7 | **cluster-aot** | 30 | 553 | 16 | `cluster_aot.c` |

Wave rationale:

- **Wave 0 — delete (14 fns, 410 lines).** `compress_zlib.c` has zero callers (finding F1). Free.
- **Wave 1 — pure byte-in/byte-out, no runtime coupling (51 fns).** `crypto-hex` (3),
  `compress-checksum` (4), `compress-bitstream` (9), `compress-container` (7), `compress-deflate` (12),
  `math-numeric-semantics` (16). Every one is a total function over bytes/numbers with an existing
  differential oracle (gzip round-trip, CRC vectors, IEEE-754 rules). This is where the Xray
  toolchain gets exercised on real code with the smallest blast radius.
  Note on `compress-checksum`: its 4 stdlib functions are 3-line pass-throughs; the CRC32/Adler32
  tables and loops live in `src/shared/xr_compress_core.h`, which is also the AOT runtime's copy —
  so the real migration target is the shared core, not the stdlib wrapper (see F9).
- **Wave 2 — regex (134 fns, 3,948 lines of the module's 4,561).** Self-contained: parser -> bytecode compiler -> NFA/DFA
  engine -> API, with `regex-binding` (37) evaporating at the end. Four genuinely separable slices:
  `regex-parser` (pattern text -> AST), `regex-compiler` (AST -> program + analysis), the two
  engines, and `regex-api` (iterators, replace-template expansion, split, escape). **Blocked by
  the compiler dependency in finding F3** — `src/aot/xrt_regex_core.c` and the VM
  `OP_REGEX_COMPILE` path link against this C.
- **Wave 3 — crypto primitives (36 fns).** MD5/SHA-1/SHA-512/HMAC first (`crypto-primitives`, 21),
  then AES (`crypto-aes`, 13) since that is where the constant-time argument lives, then
  `cluster-auth` (2) which just composes HMAC-SHA256 with a constant-time compare. Same F3 blocker.
- **Wave 4 — net (73 fns, 1,409 lines of the module's 2,242).** Order inside net matters: `net-error` and `net-deadline`
  are leaf-free pure classification and go first; `net-coro-io` / `net-copy` / `net-udp` /
  `net-tls-handshake` are the CPS families; `net-blocking-io` (`stdlib/net/io.c`) is the *last*
  net slice because cluster and http2 both sit on it. `net-tls-lib` (31 of 33 host_abi) stays C.
- **Wave 5 — io/os/sys process surface (35 fns, 512 lines).** Small, but each item is a separate syscall
  contract; the value is mostly deleting `sys-pipe`/`sys-process` glue.
- **Wave 6 — http2 (61 fns, 1,690 lines of the module's 1,838).** `http2-hpack` (13) is an independently testable codec
  with RFC 7541 vectors and should be its own slice. Frames/validation/conn/streams next.
  `http2-pool` last (it is the only part with a lifetime/threading story). See F6: this wave also
  changes http2 from blocking to async.
- **Wave 7 — cluster (158 fns, 3,452 lines of the module's 3,806), hardest and last.** Bottom-up: `cluster-proto` (frame
  codec, 20) -> `cluster-topic-trie` (7) -> `cluster-topic-transport` (7) -> `cluster-health` (7,
  phi accrual) -> `cluster-discovery` (11) -> `cluster-monitor` (9) -> `cluster-node-outq` (10) ->
  `cluster-node-io` (16) -> `cluster-handshake` (28) -> `cluster-runtime` (13) -> `cluster-aot` (30,
  which should be *deleted* rather than migrated once the .xr version serves both backends, F4).

### Wave totals and what is deliberately not sequenced

| wave | functions | C lines |
|---:|---:|---:|
| 0 delete | 14 | 410 |
| 1 pure codecs/algorithms | 51 | 1,044 |
| 2 regex | 134 | 3,948 |
| 3 crypto | 36 | 564 |
| 4 net | 73 | 1,409 |
| 5 io/os/sys process | 35 | 512 |
| 6 http2 | 61 | 1,690 |
| 7 cluster | 158 | 3,452 |
| **sequenced** | **562** | **13,029** |

The other **385 functions in 30 cohorts are intentionally unsequenced**: they are glue that dies
with its owner, or leaves that stay C. Largest, with dispositions:

| cohort | fns | vanishes | leaf | note |
|---|---:|---:|---:|---|
| `sys-sync-classes` | 39 | 37 | 1 | dies when sys's five Os* classes get .xr wrappers |
| `regex-binding` | 37 | 37 | 0 | dies with wave 2 |
| `net-tls-lib` | 33 | 0 | 31 | OpenSSL wrapper — stays C (2 rows are ALPN preference + error strings) |
| `io-callbacks` | 29 | 9 | 20 | syscall adapters for `src/shared/xr_io_core.h` |
| `net-binding` | 25 | 25 | 0 | dies with wave 4 |
| `module-constants` | 22 | 22 | 0 | 22 `const` declarations, not functions (F11) |
| `math-libm` | 21 | 21 | 0 | becomes ~21 libm leaf declarations (F8) |
| `compress-binding` | 21 | 21 | 0 | dies with wave 1 |
| `mem-buffer` + `mem-raw` + `mem-page` + `mem-intrinsic-stub` | 51 | 12 | 39 | mem is the unsafe boundary; stays C |
| `crypto-binding` | 17 | 17 | 0 | dies with wave 3 |
| `sys-sync-ops` + `sys-signal` + `sys-thread` | 27 | 0 | 27 | pthread/signal/thread leaves |
| `cluster-binding` + `cluster-node` | 16 | 15 | 1 | dies with wave 7 |
| `time-clock` + `time-sleep` + `runtime-stats` + `prelude-registry` + `http2-binding` + `os-env` + `os-platform-const` + `os-misc` + `math-binding` + `math-random` + `net-platform` + `sync-binding` | 47 | 18 | 29 | clock/heap/registry facades |

## 3. Which modules are inflated, and which are dense?

### Number is inflated (mostly glue/helper/leaf)

| module | owners | must-migrate | vanishes | leaf | verdict |
|---|---:|---:|---:|---:|---|
| **mem** | 55 | 0 | 16 | 39 | **0 must migrate.** Raw pointers, fences, page alloc, cache ops, Buffer bodies, 5 no-op compiler-intrinsic stubs. This module is the unsafe boundary; it is supposed to stay C. |
| **sys** | 84 | 10 | 46 | 28 | **10 of 84.** 37 are OsMutex/OsRwLock/OsCondvar/OsBarrier/OsOnce class plumbing (`*_class`, `*_body`, `*_body_init`, `*_body_destroy`, `*_invalid_receiver`, `*_new`, `xr_sys_*_register_class`); 28 are pthread/signal/thread leaves. The only real semantics: `sys_barrier_wait` generation counting, the pipe/process CPS. |
| **math** | 53 | 16 | 36 | 1 | **16 of 53.** 21 libm wrappers + 14 constants + 1 helper + 1 random leaf. See F8/F11. |
| **io** | 37 | 8 | 9 | 20 | **8 of 37.** 20 host_abi callback adapters (`io_file_read`, `io_path_kind`, `io_remove_all_leaf`, `io_mkdirp_mkdir`, …) and 9 helpers, because the real policy already lives in `src/shared/xr_io_core.h`. Only `file_io_*` (io_uring CPS) and 3 buffering functions are Xray work. |
| **os** | 22 | 8 | 5 | 9 | **8 of 22.** 4 constants + 12 syscall facades. The only substance: `win_build_command_line` / `win_append_escaped_arg` (Windows quoting rules) and `exec_pipe_*` / `read_exec_pipes` (poll-drain buffering). |
| **time** | 11 | 0 | 7 | 4 | **0 of 11.** Clock facades plus one coroutine sleep primitive. |
| **runtime** | 6 | 0 | 1 | 5 | **0 of 6.** Heap statistics accessors. |

`net`'s 135 is half-inflated: 25 handle/marshalling glue + 44 leaf candidates (31 of them the
OpenSSL wrapper in `tls.c`), leaving 66 real. And 29 of the 66 are CPS scaffolding, so the Xray
version is far smaller than 66 functions suggests.

### Number is small but every function is hard

| module | owners | must-migrate | density |
|---|---:|---:|---|
| **crypto** | 54 | 36 of 54 (67%). MD5/SHA-1/SHA-512 compression functions, HMAC, AES-128/192/256 key schedule + CBC, all written from scratch. Bit-exact or nothing. |
| **compress** | 67 | 28 of 67 (42%, after removing the 14 dead `compress_zlib.c` rows) and the 28 are RFC 1951 inflate (175 lines, one function), LZ77 + fixed-Huffman deflate (139 lines), canonical Huffman table construction, and gzip/zlib container framing. |
| **http2** | 68 | 54 of 68 (79%). HPACK integer/string/Huffman codec + dynamic table eviction, RFC 9113 frame validation matrix, a 216-line receive dispatch loop, flow control, connection pooling. |
| **regex** | 171 | 122 of 171 (71%), 4,561 lines. A full regex implementation: 990-line parser (295-line `parse_atom`, 173-line `parse_escape`), 1,316-line bytecode compiler with anchoring/prefix/one-pass analysis, Thompson NFA with a one-pass fast path and a literal memmem path, and a lazy DFA with a state cache. |
| **cluster** | 176 | 133 of 176 (76%), 3,806 lines across 9 files. Handshake state machine, HMAC mutual auth, phi-accrual failure detector, topic-pattern trie with `*` and `>` wildcards, multicast discovery, backpressure queue with a 4 MiB high watermark, remote coroutine monitors — plus a whole second implementation for AOT. |

## 4. Cross-module C dependencies (migration ordering constraints)

Computed by extracting every non-`static` definition under `stdlib/` and matching call sites,
then filtering out same-name `static` collisions. Two apparent edges were **false positives** and
are excluded: `regex -> compress` (`emit_match` is `static` in both `xregex_compile.c` and
`compress.c`) and `regex -> crypto` (`hex_digit` is `static` in both `xregex_parse.c` and
`crypto.c`).

### Inside stdlib

- **`cluster` -> `net`** (14 symbols): `xr_io_close`, `xr_io_conn_from_fd`, `xr_io_listen`, `xr_tls_conn_handshake_server_try`, `xr_tls_conn_handshake_try`, `xr_tls_conn_new` …
  <br>cluster runs entirely on stdlib/net/io.c blocking-socket handles plus stdlib/net/tls.c; net-blocking-io and net-tls-lib must have an Xray answer (or stay leaves) before cluster can move
- **`http2` -> `net`** (10 symbols): `xr_io_close`, `xr_io_connect_tls_with_ctx`, `xr_io_set_timeout`, `xr_tls_conn_get_alpn`, `xr_tls_conn_read`, `xr_tls_conn_write` …
  <br>http2 is blocking end-to-end on xr_io_*/xr_tls_conn_read|write; migrating http2 onto the async net.xr surface is a behaviour change, not a translation
- **`cluster` -> `crypto`** (2 symbols): `xr_hmac_sha256`, `xr_secure_wipe`
  <br>cluster-auth proof = HMAC-SHA256; crypto-primitives must expose an Xray HMAC before cluster-auth migrates
- **`cluster` -> `mem`** (2 symbols): `xr_mem_buffer_bytes`, `xr_mem_buffer_copy_from_bytes`
  <br>cluster.send takes a mem.Buffer envelope; mem stays C, so this is a leaf dependency, not a migration blocker
- **`prelude` -> `sys/net/regex`** (8 symbols): `xr_sys_mutex_register_class`, `xr_sys_rwlock_register_class`, `xr_sys_condvar_register_class`, `xr_sys_barrier_register_class`, `xr_sys_once_register_class`, `xr_netconn_register_class` …
  <br>xr_prelude_register_all_native_types is the single hub that wires every native class; each class deletion has to edit this one function
- **`cluster/net/io/os/runtime` -> `_stdlib (stdlib_cache.c)`** (3 symbols): `xr_stdlib_enum_type_get`, `xr_stdlib_record_class_get`, `xr_stdlib_cache_get`
  <br>shared lookup of generated enum/record classes; only used by C-side binding glue, so it dies last

### From the compiler/runtime into stdlib (the ones that actually block)

- **`src/aot + src/vm + src/runtime` -> `stdlib/regex`** (15 symbols): `xr_regex_compile`, `xr_regex_compile_literal`, `xr_regex_test`, `xr_regex_match`, `xr_regex_match_at`, `xr_regex_full_match` …
  <br>HARD BLOCKER: the VM opcode for regex literals and the whole AOT regex runtime link against stdlib/regex C. Migrating regex to .xr does not delete this C unless OP_REGEX_COMPILE and src/aot/xrt_regex*.h are re-hosted too
- **`src/shared/xr_crypto_core.h + src/incremental + src/module + src/app/toolchain + src/aot` -> `stdlib/crypto`** (13 symbols): `xr_md5`, `xr_sha1`, `xr_sha512`, `xr_hmac_md5`, `xr_hmac_sha1`, `xr_hmac_sha256` …
  <br>HARD BLOCKER: the compiler itself hashes with these. Note SHA-256 already lives in src/base/xsha256.c, outside stdlib - the same split is the model for the rest
- **`src/shared/xr_compress_core.h + src/module/xpkg_client.c + src/aot/xrt_compress.h` -> `stdlib/compress`** (10 symbols): `xr_deflate`, `xr_deflate_bound`, `xr_inflate`, `xr_gzip`, `xr_gunzip`, `xr_gzip_original_size` …
  <br>HARD BLOCKER: the package client gunzips downloads with stdlib/compress
- **`src/aot/xrt_cluster.h` -> `stdlib/cluster`** (5 symbols): `xrt_cluster_start`, `xrt_cluster_join`, `xrt_cluster_stop`, `xrt_cluster_send`, `xrt_cluster_listen`
  <br>cluster_aot.c is a SECOND, reduced implementation (threads + blocking sockets, no TLS, no discovery, no phi-accrual, linear topic match). Migrating cluster to .xr is the only way to collapse the two
- **`src/vm/xvm_dispatch_collection.inc.c` -> `stdlib/mem`** (2 symbols): `xr_mem_buffer_length`, `xr_mem_buffer_materialize`
  <br>VM collection dispatch reads mem.Buffer bodies directly
- **`src/io/xnet_handle.c` -> `stdlib/net`** (1 symbols): `xr_tls_conn_free`
  <br>runtime net-handle teardown frees TLS connections
- **`src/frontend/analyzer/*` -> `stdlib/prelude`** (2 symbols): `xr_prelude_get_symbols`, `xr_prelude_lookup_type`
  <br>the ANALYZER (compile time) resolves prelude names through stdlib/prelude C

### Resulting order constraints

```
crypto-primitives  ->  cluster-auth            (xr_hmac_sha256)
net-blocking-io    ->  cluster-*, http2-*      (xr_io_connect/listen/close/read/write)
net-tls-lib        ->  cluster-*, http2-*      (xr_tls_context_*, xr_tls_conn_*)
mem-buffer         ->  cluster-binding         (leaf, not a blocker)
prelude-registry   ->  every native class deletion edits one function
src/aot + src/vm   ->  regex, crypto, compress, cluster, mem, net  (HARD)
src/analyzer       ->  prelude                                     (HARD, compile time)
```

## 5. Verified incidental findings

### F1. stdlib/compress/compress_zlib.c is dead code

All 14 functions (410 C lines) have zero callers anywhere in stdlib/, src/, tests/ or tools/. Only their own declarations in stdlib/compress/compress.h reference them. The file is compiled whenever CMake finds ZLIB (CMakeLists.txt:1080-1089, 1141) and links -lz for nothing. Verified by grepping every exported name: xr_zlib_stream_new_deflate, xr_zlib_stream_new_inflate, xr_zlib_stream_process, xr_zlib_stream_finished, xr_zlib_stream_free, xr_deflate_sync_flush, xr_inflate_bounded, xr_detect_content_encoding, xr_zlib_gzip_compress, xr_zlib_gzip_decompress, xr_zlib_deflate_compress, xr_zlib_deflate_decompress. The file header claims WebSocket permessage-deflate (RFC 7692) and HTTP Content-Encoding use it; neither stdlib/ws/ws.xr nor stdlib/http/http.xr does.

*Action:* Delete the file, the declarations in compress.h, and the two CMake ZLIB filters. Removes 14 of the 947 with no Xray work at all.

### F2. One of the 947 is an inventory parse artifact

stdlib/io/io.c line 217 reads `static intptr_t XR_IO_CORE_ACQUIRE_HANDLE("xray_file_stream") io_file_open_handle(const char *path) {`. The static-analysis annotation macro is parsed as the function name. The inventory therefore lists a symbol named XR_IO_CORE_ACQUIRE_HANDLE and does NOT list io_file_open_handle at all.

*Action:* One-line fix in scripts/stdlib_symbol_inventory.py (skip XR_IO_CORE_* annotation tokens). Real owner count is unchanged at 947; only the name is wrong.

### F3. The compiler links against stdlib crypto, compress and regex

These are not "stdlib only" modules. src/shared/xr_crypto_core.h calls xr_md5/xr_sha1/xr_sha512/xr_hmac_*/xr_aes_* from stdlib/crypto/crypto.c, and it is consumed by src/incremental/xr_cache_invalidate.c, xr_dependency_graph.c, xr_module_summary.c, src/module/xlockfile.c, xnative_package.c, xpkg_client.c and src/app/toolchain/xtc_probe.c, xtc_runtime_manifest.c. src/shared/xr_compress_core.h calls xr_deflate/xr_inflate/xr_gzip/xr_gunzip; src/module/xpkg_client.c gunzips package downloads with them. src/aot/xrt_regex_core.c calls 12 stdlib/regex entry points and src/vm/xvm_dispatch_assert.inc.c calls xr_regex_compile_literal for OP_REGEX_COMPILE.

*Action:* Migrating any of these three to .xr does NOT delete the C unless the compiler-side consumer is re-pointed first. SHA-256 already shows the intended shape: it lives in src/base/xsha256.c and stdlib/crypto/crypto.h merely re-declares it.

### F4. cluster is implemented twice

stdlib/cluster/cluster_aot.c (30 owners, 553 C lines) is a second, thread-and-blocking-socket implementation of node/queue/reader/writer/accept/handshake for the AOT backend, exported as xrt_cluster_start|join|stop|send|listen. It reuses only the backend-neutral kernels (cluster_proto.c frames, cluster_auth.c proof, cluster_topic_core.h matching). It has NO TLS, NO multicast discovery, NO heartbeat/phi-accrual failure detection, and uses linear xr_cluster_topic_matches instead of the trie. The VM path (cluster.c + cluster_node.c, 86 owners) has all of them.

*Action:* cluster is the one module where migrating to .xr collapses two divergent implementations into one. That is also why it is the hardest: the .xr version has to satisfy both the VM coroutine model and AOT.

### F5. 91 owners exist only because C cannot suspend

91 of the 947 are CPS scaffolding: *_continue / *_step / *_drive / *_entry / *_complete / *_wait / *_done / *_finish / *_context_destroy / *_ctx_free pairs around a xr_yield_for_io / xr_yield_for_timeout call. Distribution: cluster 40, net 29, sys 14, io 4, http2 2, time 1, os 1. 80 of the 91 are classified must_migrate_to_xray.

*Action:* Do not size these cohorts by function count. In Xray each family collapses to one `while` loop with `await`; e.g. net.c readInto is 8 C functions (net_read_into_continue/value/done/progress/error/complete/wait/step, 172 lines) that become one loop.

### F6. http2 is fully blocking; net is fully async

stdlib/http2 never touches the coroutine yield path. h2_send/h2_recv loop on xr_tls_conn_write / xr_socket_send / xr_tls_conn_read / xr_socket_recv directly (stdlib/http2/http2.c:933-948, 1259-1275), and http2_client_request drives the whole request synchronously. stdlib/net/net.c is the opposite: every public operation is a CPS state machine over xr_yield_for_io with deadlines, io_uring fast paths and TLS-vs-plain dispatch.

*Action:* Porting http2 onto the net.xr async surface is a behaviour change (it will start yielding), not a mechanical translation. Budget for it explicitly.

### F7. stdlib/net/tls.c has a complete stub twin

The extractor finds 65 function definitions in the 867-line file: 33 real OpenSSL implementations under #ifdef XR_ENABLE_TLS and 32 no-op stubs under #else (lines 713-865). The inventory counts the 33 distinct names once.

*Action:* Any per-file "how many C functions" number for net is roughly double the semantic truth.

### F8. math is 21 glue wrappers over libm, not 40 algorithms

21 of math.c bodies are exactly `unpack XrValue -> call libm -> repack` (sqrt, pow, sin, cos, tan, asin, acos, atan, atan2, log, log10, log2, exp, sinh, cosh, tanh, hypot, cbrt, fmod, log1p, expm1). Only 16 carry real semantics that Xray must reproduce: the int-vs-float result rule and DOUBLE_FITS_INT64 narrowing in abs/floor/ceil/round/trunc/sign, IEEE-754 NaN propagation in min/max/clamp, isNaN/isFinite, lerp/degToRad/radToDeg, and random/randomInt (which delegate to src/shared/xr_math_core.h).

*Action:* math.xr will still need ~21 libm leaf declarations. The glue does not disappear, it shrinks into leaf bindings.

### F9. Much of the "stdlib semantics" already lives outside stdlib/

io.c, os.c, math.c, mem.c, compress.c, crypto.c and regex all delegate their real rules to header-only kernels in src/shared/: xr_io_core.h (read-all-stream, mkdirp, remove-all), xr_os_core.h, xr_math_core.h, xr_sync_core.h, xr_compress_core.h, xr_crypto_core.h, xr_regex_core.h. Those kernels are shared with the AOT runtime (src/aot/xrt_*.h) and the VM. The stdlib C file supplies syscall callbacks plus XrValue marshalling.

*Action:* For io/os in particular the stdlib C file is mostly callback adapters, which is why io shows 20 host_abi + 9 internal_helper and 0 binding_glue: there is almost no stdlib-owned semantics left to move.

### F10. The briefing per-module numbers are raw C definition counts, not owners

os 43, sys 100, math 40, mem 51, crypto 55, compress 65, http2 69, cluster 186 are exactly the raw function-definition counts per module directory (verified with an extractor); net 195 matches net.c 111 + tls.c 65 + io.c 19 + xneterror.c 1 = 196. Those totals include the per-module loader, the 111 already-approved `__` native leaves, and the tls.c #else twins. The 947 semantic-owner set distributes differently: net 135, sys 84, io 37, os 22, math 53 (39 functions + 14 consts), mem 55 (51 + 4 consts), crypto 54, compress 67, http2 68, cluster 176, regex 171.

*Action:* Use the owner numbers when sizing migration work.

### F11. 22 of the 947 are not functions at all

22 rows have handwritten_c_body == "external" and kind == "const": math.PI/E/TAU/SQRT2/LN2/LN10/LOG2E/LOG10E/EPSILON/MAX_I64/MIN_I64/MAX_F64/INF/NAN, mem.PROT_NONE/PROT_READ/PROT_WRITE/PROT_EXEC, os.platform/arch/sep/eol. They are constant declarations bound from stdlib/defs/core.def with no C body.

*Action:* They disappear the moment the owning module gets a .xr file that declares the constants. Near-zero cost, 22 off the counter.

### F12. The only genuinely constant-time-sensitive kernels are 14 functions

crypto-aes (gf_mul, xr_aes_init, aes_add_round_key, aes_sub_bytes, aes_inv_sub_bytes, aes_shift_rows, aes_inv_shift_rows, aes_mix_columns, aes_inv_mix_columns, aes_encrypt_block, aes_decrypt_block, xr_aes_cbc_encrypt, xr_aes_cbc_decrypt = 13, 200 lines) plus cluster_proof_equal. crypto.timingSafeEqual itself is glue over xr_crypto_core_timing_safe_equal in src/shared. Note the current AES is a table-driven S-box implementation and is NOT constant-time today, so "keep it in C for constant-time" is not a defence of the status quo.

*Action:* This is the only place the "security kernel exception" applies, and it is 14 functions, not the whole crypto module.

## Per-module table

| module | owners | C lines | read verbatim | migrate | vanishes | leaf | dead |
|---|---:|---:|---:|---:|---:|---:|---:|
| cluster | 176 | 3806 | 71 | 133 | 34 | 9 | 0 |
| regex | 171 | 4561 | 20 | 122 | 49 | 0 | 0 |
| net | 135 | 2242 | 80 | 66 | 25 | 44 | 0 |
| sys | 84 | 779 | 31 | 10 | 46 | 28 | 0 |
| http2 | 68 | 1838 | 18 | 54 | 13 | 1 | 0 |
| compress | 67 | 1504 | 33 | 28 | 25 | 0 | 14 |
| mem | 55 | 420 | 26 | 0 | 16 | 39 | 0 |
| crypto | 54 | 811 | 15 | 36 | 17 | 1 | 0 |
| math | 53 | 377 | 24 | 16 | 36 | 1 | 0 |
| io | 37 | 305 | 33 | 8 | 9 | 20 | 0 |
| os | 22 | 241 | 6 | 8 | 5 | 9 | 0 |
| time | 11 | 78 | 11 | 0 | 7 | 4 | 0 |
| prelude | 7 | 142 | 5 | 0 | 7 | 0 | 0 |
| runtime | 6 | 68 | 6 | 0 | 1 | 5 | 0 |
| sync | 1 | 6 | 1 | 0 | 1 | 0 | 0 |
| **total** | **947** | **17178** | **380** | **481** | **291** | **161** | **14** |

Per-function rows (module, file, C name, Xray symbol, category, cohort, evidence, disposition,
C line count) are in `c_owner_analysis.json` under `symbols`.


## Addendum: the 18 rows the rewritten generator adds (947 -> 965)

Verified by re-running the current generator into `/tmp/inv_recheck.json` and diffing the owner
sets. **18 added, 0 removed.** Both groups are `vanishes_with_owner`; the must-migrate total stays
at 481.

**`stdlib/stdlib_cache.c` — 10 functions, module `_stdlib` (read verbatim, 10/10).**
`stdlib_object_shape_decl_find`, `stdlib_record_shape_decl_find`, `stdlib_record_class_build`,
`stdlib_enum_decl_find`, `stdlib_enum_type_build`, `xr_stdlib_cache_get`,
`xr_stdlib_enum_type_get`, `xr_stdlib_enum_type_module`, `xr_stdlib_record_class_get`,
`xr_stdlib_cache_free`. Per-isolate lazy caches that materialise the generated enum types and
record classes declared in `src/stdlib/xstdlib_defs_generated.h`, so a C binding can look up
`("net", "NetError")` or `("cluster", "ClusterInfo")` and get an `XrEnumType *` / `XrClass *`.
`binding_glue` x9 + `runtime_primitive` x1 (`xr_stdlib_cache_free`, an isolate teardown hook that
also cleans up the log module's async state). They die with the last C binding — which is exactly
why they were previously listed as the `_stdlib` target of the
`cluster/net/io/os/runtime -> _stdlib` edge in section 4.

**`Coro` — 8 rows, `handwritten_c_body == "external"`.**
`CoroInfo`, `CoroStats`, `CoroDeadlock` (object shapes), `CoroState`, `CoroGroupKey`, `CoroMetric`
(enums), `CoroLocal.get`, `CoroLocal.set` (type-methods). **These are not C functions.** They are
declarations in `stdlib/defs/core.def` whose implementation lives in `src/coro/`, and every one
carries the blocker `"declared module is outside the stdlib boundary manifest"`. Same class of
artifact as finding F11 (the 22 `const` rows): a declaration attributed to a pseudo-module, with
no stdlib C body to migrate.

Restated totals under the new definition:

| bucket | 947-row snapshot | 965-row current |
|---|---:|---:|
| must migrate | 481 | 481 |
| vanishes with owner | 291 | 309 |
| leaf candidate | 161 | 161 |
| dead code | 14 | 14 |
| **total** | **947** | **965** |
