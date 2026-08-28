# Private native leaf dossier — 111 `__` entries in `stdlib/defs/core.def`

Scope: every declaration in `stdlib/defs/core.def` whose name starts with `__`.
That is **108 `fn` entries plus 3 data-shape declarations** (`io.__FileStat` and `os.__ExecResult` are
`handle` blocks, `net.__CopyBidirectionalResult` is an `object` block) = 111.
`os.__kill` appears twice — two `.def` entries (argc 1 and argc 2) over one VM symbol `os_kill`.

Every classification below was made after reading the C body the entry's `vm:` field names
(and, where the two differ, the `aot:` body as well). Read-only pass; nothing in the repo was modified.

## Verdict

| class | count | share |
|---|---:|---:|
| `host_abi_leaf` | 49 | 44.1% |
| `runtime_leaf` | 4 | 3.6% |
| `machine_intrinsic_leaf` | 0 | 0.0% |
| `security_provider_leaf` | 1 | 0.9% |
| `not_a_leaf` | 57 | 51.4% |
| **total** | **111** | 100% |

**54 of 111 (48.6%) survive as approved leaves; 57 of 111 (51.4%) must migrate into `.xr`.**

### Per-module split

| module | entries | true leaves | `not_a_leaf` | true-leaf ratio |
|---|---:|---:|---:|---:|
| `io` | 37 | 17 | 20 | 17/37 = 46% |
| `net` | 28 | 7 | 21 | 7/28 = 25% |
| `os` | 26 | 18 | 8 | 18/26 = 69% |
| `sys` | 16 | 12 | 4 | 12/16 = 75% |
| `cluster` | 4 | 0 | 4 | 0/4 = 0% |
| **all** | **111** | **54** | **57** | **49%** |

`machine_intrinsic_leaf` has **zero** members: none of the 111 private entries is a math/bit/SIMD/memory intrinsic.
`security_provider_leaf` has exactly one: `net.__tlsHandshake`.

---

## `io` — 37 entries

`host_abi_leaf`: 17  `not_a_leaf`: 20

| entry | class | vm symbol | ABI | effect | provider | evidence (short) |
|---|---|---|---|---|---|---|
| `__chdir` | `host_abi_leaf` | `io_chdir` | POSIX chdir(2) / SetCurrentDirectory | not yieldable; process-global side effect | both | io.c:793 io_chdir -> xr_fs_chdir (src/os/unix/fs_unix.c: `return chdir(path)==0 ? 0 : -1`). One syscall. |
| `__chmod` | `host_abi_leaf` | `io_chmod` | POSIX chmod(2) / MSVCRT _chmod | not yieldable; may block briefly on the FS | both | io.c:1146 io_chmod: xr_io_core_chmod_mode() is only a `mode<0 \|\| mode>INT_MAX` range check and cast (xr_io_core.h:126), then a single chmod(path,mode) (_chmod on Windows). No lo… |
| `__cwd` | `host_abi_leaf` | `io_cwd` | POSIX getcwd(3) / GetCurrentDirectory | not yieldable; cannot block meaningfully | both | io.c:779 io_cwd -> xr_fs_getcwd = `return getcwd(out, out_size)` (fs_unix.c). Single call into an XR_PATH_MAX stack buffer. |
| `__exists` | `host_abi_leaf` | `io_exists` | POSIX lstat(2) / GetFileAttributesA | not yieldable; may block on the FS | both | io.c:592 io_exists -> xr_fs_exists -> xr_fs_stat = one lstat(2) plus a kind test (fs_unix.c:57). HAZARD: io.c:596-613 still contains a live debug probe that fprintf()s `[DBG io_ex… |
| `__fileClose` | `host_abi_leaf` | `io_fileClose` | POSIX fclose(3) | not yieldable; may block on flush | both | io.c:245 io_fileClose: rejects handle<=0, then `fclose((FILE*) handle)`. No policy. |
| `__fileOpen` | `host_abi_leaf` | `io_fileOpen` | POSIX fopen(3) | not yieldable; blocks on open | both | io.c:227 io_fileOpen -> io_file_open_handle = `fopen(path,"rb")` cast to intptr_t. Annotated XR_IO_CORE_ACQUIRE_HANDLE("xray_file_stream"). |
| `__fileRead` | `host_abi_leaf` | `io_fileRead` | POSIX fread(3) | not yieldable despite blocking on disk | both | io.c:235 io_fileRead -> io_stream_read_bytes: one fread() into a freshly allocated byte array, then arr->length = count. No retry, no growth loop. Note the one policy line: `handl… |
| `__fileSize` | `host_abi_leaf` | `io_fileSize` | POSIX lstat(2) | not yieldable; may block on the FS | both | io.c:648 io_fileSize -> xr_fs_stat (one lstat) then `st.size`; -1 on failure. |
| `__isDir` | `host_abi_leaf` | `io_isDir` | POSIX lstat(2) [VM] / stat(2) [AOT] | not yieldable; may block on the FS | both | io.c:636 io_isDir -> xr_fs_is_dir -> xr_fs_stat (lstat) + `kind == XR_FS_DIR`. DIVERGENCE: the AOT twin xrt_io_is_dir uses stat(2), which follows symlinks; a symlink-to-directory … |
| `__isFile` | `host_abi_leaf` | `io_isFile` | POSIX lstat(2) [VM] / stat(2) [AOT] | not yieldable; may block on the FS | both | io.c:624 io_isFile -> xr_fs_is_file -> xr_fs_stat (lstat) + `kind == XR_FS_FILE`. Same lstat/stat VM-vs-AOT divergence as __isDir. |
| `__isSymlink` | `host_abi_leaf` | `io_isSymlink` | POSIX lstat(2) / Win32 GetFileAttributesA | not yieldable; may block on the FS | both | io.c:948 io_isSymlink: POSIX `lstat + S_ISLNK`; Windows GetFileAttributesA + FILE_ATTRIBUTE_REPARSE_POINT. One call per platform. |
| `__readlink` | `host_abi_leaf` | `io_readlink` | POSIX readlink(2) / Win32 GetFinalPathNameByH… | not yieldable; may block on the FS | both | io.c:1200 io_readlink: POSIX one readlink(2) into an XR_PATH_MAX buffer; Windows CreateFileA(FILE_FLAG_OPEN_REPARSE_POINT) + GetFinalPathNameByHandleA + CloseHandle. Result goes t… |
| `__realpath` | `host_abi_leaf` | `io_realpath` | POSIX realpath(3) / Win32 GetFullPathName | not yieldable; may block on the FS | both | io.c:1233 io_realpath -> xr_fs_realpath (fs_unix.c): one realpath(path,NULL), copied into the caller buffer, then free(). Plus the same \\?\ prefix strip. |
| `__remove` | `host_abi_leaf` | `io_remove` | C remove(3) / POSIX unlink(2) | not yieldable; may block on the FS | both | io.c:665 io_remove: `remove(path) == 0`. One call. |
| `__rename` | `host_abi_leaf` | `io_rename` | C rename(3) / POSIX rename(2) | not yieldable; may block on the FS | both | io.c:677 io_rename: `rename(old,new) == 0`. One call, no errno branch. |
| `__stat` | `host_abi_leaf` | `io_stat` | POSIX stat(2) + lstat(2) / Win32 GetFileAttri… | not yieldable; may block on the FS | both | io.c:987 io_stat: one stat(2) plus one lstat(2) (solely to derive isSymlink, since stat follows links), then xr_io_core_stat_fields packs 10 scalars and 10 xr_instance_set_dynamic… |
| `__symlink` | `host_abi_leaf` | `io_symlink` | POSIX symlink(2) / Win32 CreateSymbolicLinkA | not yieldable; may block on the FS | both | io.c:1178 io_symlink: POSIX one symlink(target,path). Windows must first GetFileAttributesA(target) to pick SYMBOLIC_LINK_FLAG_DIRECTORY before CreateSymbolicLinkA — that probe is… |
| `__FileStat` | `not_a_leaf` | `—` | none | not yieldable; cannot block | runtime | Declaration only: `handle __FileStat { fields: ... }` in core.def has no vm:/aot: symbol. io.c builds it through xr_stdlib_record_class_get(X,"io","__FileStat") and 10 xr_instance… |
| `__appendFile` | `not_a_leaf` | `io_appendFile` | POSIX fopen(3)/fwrite(3)/fclose(3) | not yieldable; blocks the worker on disk I/O | both | io.c:570 io_appendFile = fopen(path,"ab") + xr_io_core_write_all(...) + fclose. xr_io_core_write_all (src/shared/xr_io_core.h:323) is a `while (off < len)` short-write drain loop. |
| `__copyFile` | `not_a_leaf` | `io_copyFile` | POSIX sendfile(2)/fcopyfile(3)/read(2)+write(… | not yieldable; blocks the worker for the whole copy | both | io.c:805 io_copyFile branches three ways: Windows CopyFileA; macOS open+open+fcopyfile+close; Linux open+fstat+open then a `while (remaining>0) { sendfile(...); if (sent<0 && errn… |
| `__mkdir` | `not_a_leaf` | `io_mkdir` | POSIX mkdir(2) + stat(2) | not yieldable; may block on the FS | both | io.c:691 io_mkdir -> xr_fs_mkdir (fs_unix.c:94): mkdir(path,mode), and on EEXIST it issues a SECOND syscall (stat + S_ISDIR) and reports success. That is errno classification plus… |
| `__mkdirp` | `not_a_leaf` | `io_mkdirp` | POSIX mkdir(2) + stat(2), repeatedly | not yieldable; blocks per segment | both | io.c:1055 copies the path into an XR_PATH_MAX buffer and calls xr_io_core_mkdirp (xr_io_core.h:670): trailing-separator trimming, xr_io_core_root_len() Windows drive-letter and UN… |
| `__readDir` | `not_a_leaf` | `io_readDir` | POSIX opendir(3)/readdir(3)/closedir(3), Win3… | not yieldable; blocks for the whole directory | both | io.c:758 io_readDir drives xr_io_core_read_dir (xr_io_core.h:492) over io_dir_for_each_entry, which is a `while (xr_dir_next(it,&e))` loop over opendir/readdir/closedir, filters "… |
| `__readDirRecursive` | `not_a_leaf` | `io_readDirRecursive` | POSIX opendir/readdir/lstat, recursively | not yieldable; blocks for the whole traversal | both | io.c:1311 drives xr_io_core_read_dir_recursive (xr_io_core.h:513-590): a recursive descent with a depth cap (XR_IO_CORE_READ_DIR_MAX_DEPTH), per-child path joining via xr_io_core_… |
| `__readFile` | `not_a_leaf` | `io_readFile` | POSIX open(2)/pread(2)/fread(3), Linux io_uri… | yieldable (suspends on the io_uring completion); bl… | both | io.c:414 io_readFile has two full data paths: (1) on Linux+io_uring, open+fstat+S_ISREG gate, allocate, then a submit/complete state machine (file_io_step/file_io_complete/file_io… |
| `__readFileBytes` | `not_a_leaf` | `io_readFileBytes` | POSIX open(2)/pread(2)/fread(3), Linux io_uri… | yieldable on io_uring; blocks otherwise | both | io.c:453 io_readFileBytes is io_readFile's twin with FILE_IO_READ_BYTES: identical io_uring state machine, identical pread EINTR loop fallback, identical sized-read helper, then a… |
| `__readLines` | `not_a_leaf` | `io_readLines` | none beyond the underlying file read | not yieldable; blocks on the read | both | io.c:919 io_readLines reads the whole file, then runs xr_io_core_read_lines_each (xr_io_core.h:159): a `for (i<len)` scan for '\n', xr_io_core_trim_line_end() stripping trailing '… |
| `__readStdin` | `not_a_leaf` | `io_readStdin` | POSIX fread(3) on stdin | not yieldable; blocks the worker until EOF on stdin | both | io.c:165 io_readStdin -> xr_io_read_stdin_all -> xr_io_core_read_all_stream_alloc (xr_io_core.h:251): a `for(;;)` read loop with capacity doubling via realloc, a max_cap ceiling, … |
| `__readStdinBytes` | `not_a_leaf` | `io_readStdinBytes` | POSIX fread(3), MSVCRT _setmode | not yieldable; blocks until EOF | both | io.c:196 io_readStdinBytes: io_prepare_binary_stdin() (_setmode(_fileno(stdin), _O_BINARY) on Windows), then the same xr_io_core_read_all_stream_alloc doubling loop, then a memcpy… |
| `__removeAll` | `not_a_leaf` | `io_removeAll` | POSIX lstat(2)/opendir/readdir/unlink(2)/rmdi… | not yieldable; blocks for the whole tree | both | io.c:1120 io_removeAll wires an XrIoCoreRemoveAllOps vtable into xr_io_core_remove_all_impl (xr_io_core.h:458): recursive descent, per-child path allocation and join, dot-entry fi… |
| `__tempDir` | `not_a_leaf` | `io_tempDir` | POSIX mkdtemp(3) / Win32 GetTempPathA+CreateD… | not yieldable; may block on the FS | both | io.c:1282 io_tempDir: POSIX path calls xr_os_core_tmpdir (xr_os_core.h:94), a four-step env fallback chain TMPDIR -> TMP -> TEMP -> hard-coded "/tmp", then xr_io_core_temp_templat… |
| `__tempFile` | `not_a_leaf` | `io_tempFile` | POSIX mkstemp(3) / Win32 GetTempFileNameA | not yieldable; may block on the FS | both | io.c:1256 io_tempFile: identical env-chain + template construction as __tempDir, then mkstemp() followed immediately by close(fd) (the fd is discarded, so the returned path is rac… |
| `__touch` | `not_a_leaf` | `io_touch` | POSIX utime(2) + fopen(3) / Win32 _utime | not yieldable; may block on the FS | both | io.c:1167 io_touch -> xr_io_core_touch (xr_io_core.h:338): `if (update_fn(path)) return true; return create_fn(path);` — a two-step fallback where update_fn is utime(path,NULL) an… |
| `__writeFile` | `not_a_leaf` | `io_writeFile` | POSIX open(2)/pwrite(2)/fwrite(3), Linux io_u… | yieldable on io_uring; blocks otherwise | both | io.c:538 io_writeFile: on Linux+io_uring, open(O_WRONLY\|O_CREAT\|O_TRUNC)+file_io_try_uring with the same submit/complete/partial-resubmit state machine and the pwrite EINTR fall… |
| `__writeFileBytes` | `not_a_leaf` | `io_writeFileBytes` | POSIX open(2)/pwrite(2)/fwrite(3), Linux io_u… | yieldable on io_uring; blocks otherwise | both | io.c:500 io_writeFileBytes is __writeFile's twin over an Array<u8>: same io_uring state machine, same xr_io_core_write_all drain loop, same hard-coded O_TRUNC/0644. |
| `__writeStderr` | `not_a_leaf` | `io_writeStderr` | POSIX fwrite(3)/fflush(3) on stderr | not yieldable; can block if stderr is a slow pipe | both | io.c:271 io_writeStderr -> io_write_stream -> xr_io_core_write_all, the `while (off < len)` short-write drain loop, then `fflush(stream) == 0`. Loop plus a flush policy. |
| `__writeStdout` | `not_a_leaf` | `io_writeStdout` | POSIX fwrite(3)/fflush(3) on stdout | not yieldable; can block if stdout is a slow pipe | both | io.c:266 io_writeStdout is the same io_write_stream body against stdout: xr_io_core_write_all drain loop + fflush. |

## `net` — 28 entries

`host_abi_leaf`: 6  `security_provider_leaf`: 1  `not_a_leaf`: 21

| entry | class | vm symbol | ABI | effect | provider | evidence (short) |
|---|---|---|---|---|---|---|
| `__close` | `host_abi_leaf` | `net_close_handle` | POSIX close(2) / Winsock closesocket, plus ru… | not yieldable; close can block on a lingering socket | both | net.c:1799 net_close_handle dispatches to xr_net_conn_close / xr_net_listener_close (src/io/xnet_handle.c:142,158): free the TLS state if present, deregister the fd from the netpo… |
| `__fd` | `host_abi_leaf` | `net_fd_handle` | none directly — it exposes the POSIX fd / Win… | not yieldable | both | net.c:1816 net_fd_handle -> handle_get_fd (net.c:256): reads c->fd or l->fd, returning -1 when closed. It exists to surface the platform file descriptor itself, which is exactly a… |
| `__nowMs` | `host_abi_leaf` | `net_now_ms_fn` | POSIX clock_gettime(CLOCK_MONOTONIC) / Win32 … | not yieldable | both | net.c:165 net_now_ms: `xr_time_monotonic_ns() / 1000000`. One clock read plus a unit divide. |
| `__shutdown` | `host_abi_leaf` | `net_shutdown_conn` | POSIX shutdown(2, SHUT_RDWR) / Winsock shutdo… | not yieldable | both | net.c:1753 -> the same net_shutdown_mode body with XR_SHUT_RDWR. |
| `__shutdownRead` | `host_abi_leaf` | `net_shutdown_read` | POSIX shutdown(2, SHUT_RD) / Winsock shutdown | not yieldable | both | net.c:1745 -> net_shutdown_mode(net.c:1727): unwrap, closed check, then one `shutdown(fd, XR_SHUT_RD)`. The only extra work is writing net_error_from_errno(errno) into the handle'… |
| `__shutdownWrite` | `host_abi_leaf` | `net_shutdown_write` | POSIX shutdown(2, SHUT_WR) / Winsock shutdown | not yieldable | both | net.c:1749 -> the same net_shutdown_mode body with XR_SHUT_WR. |
| `__tlsHandshake` | `security_provider_leaf` | `net_tls_handshake_yieldable` | OpenSSL SSL_connect / SSL_set_tlsext_host_nam… | yieldable; parks on want-read/want-write under an a… | both | net.c:2005 net_tls_handshake_yieldable: get_tls_client_ctx(), xr_tls_conn_new(ctx, fd), xr_tls_conn_set_hostname (SNI), then net_tls_handshake_step drives the OpenSSL handshake's … |
| `__CopyBidirectionalResult` | `not_a_leaf` | `—` | none | not yieldable | runtime | Declaration only: `object __CopyBidirectionalResult { fields: "const aToB: i64, const bToA: i64"; exact: true }`. No vm:/aot: symbol. net.c:1602 net_bidi_result_object looks the c… |
| `__accept` | `not_a_leaf` | `net_accept_handle_yieldable` | POSIX accept(2)/accept4(2)/setsockopt(2), Lin… | yieldable; parks for readability under the accept d… | both | net.c:647 net_accept_handle_yieldable + net_accept_step: a try-accept fast path, then a deadline computed from listener->accept_deadline_ms via net_timeout_until, then an io_uring… |
| `__connectFd` | `not_a_leaf` | `net_connect_fd_yieldable` | POSIX socket(2)/connect(2)/setsockopt(2)/gets… | yieldable; parks on writability or the uring CQE wi… | both | net.c:476 net_connect_fd_yieldable composes at least eight decisions: port range validation, xr_dns_resolve + a redundant net_is_literal_ip guard, socket(2), xr_io_set_nonblocking… |
| `__copyBidirectional` | `not_a_leaf` | `net_copy_bidirectional_yieldable` | POSIX recv(2)/send(2)/shutdown(2) [VM], selec… | yieldable in the VM (three coroutines); the AOT sel… | both | TWO DIFFERENT IMPLEMENTATIONS. VM (net.c:1660): allocates a NetBidiShared with five _Atomic fields, spawns TWO coroutines (net_copy_direction_coro) with a 65536-byte buffer each, … |
| `__hasTLS` | `not_a_leaf` | `net_has_tls` | none | not yieldable | runtime | net.c:1923 net_has_tls is `#ifdef XR_ENABLE_TLS return true; #else return false; #endif` — a compile-time build-configuration constant dressed up as a runtime call. Nothing execut… |
| `__lastCode` | `not_a_leaf` | `net_last_code` | none | not yieldable | runtime | net.c:1881 net_last_code reads back c->last_error / l->last_error — the value that net_error_from_errno (net.c:200) stashed there during the last native operation. No syscall. It … |
| `__lastConnectCode` | `not_a_leaf` | `net_last_connect_code` | none | not yieldable | runtime | net.c:1901 net_last_connect_code returns the XR_THREAD_LOCAL int g_last_connect_code (net.c:388). No syscall, no ABI. The source comment states outright that the slot exists only … |
| `__lastErrno` | `not_a_leaf` | `net_last_errno` | none | not yieldable | runtime | net.c:1908 net_last_errno reads back c->last_errno / l->last_errno, the raw platform errno stashed by the same native ops. No syscall. |
| `__listenFd` | `not_a_leaf` | `net_listen_fd` | POSIX socket(2)/setsockopt(2)/bind(2)/listen(… | not yieldable; bind/listen do not block meaningfully | both | net.c:1764 net_listen_fd -> xr_io_listen (stdlib/net/io.c:356), which is a dual-stack preference algorithm: inet_pton-probe the address to decide force_ipv4, try socket(AF_INET6) … |
| `__readInto` | `not_a_leaf` | `net_read_into_yieldable` | POSIX recv(2), Linux io_uring IORING_OP_RECV,… | yieldable; parks for readability/writability under … | both | net.c:876 + net_read_into_step (net.c:789): a `for(;;)` recv loop with `if (socket_error == XR_EINTR) continue`, an EAGAIN park branch, an io_uring IORING_OP_RECV branch, a separa… |
| `__resolveAll` | `not_a_leaf` | `net_resolve_all_yieldable` | POSIX getaddrinfo(3) via xr_dns_resolve_all, … | yieldable; suspends on the async pool for a cache m… | both | net.c:345 net_resolve_all_yieldable: net_is_literal_ip() short-circuits with two inet_pton probes; otherwise it submits to the async DNS pool, probes whether the cache answered in… |
| `__setAcceptDeadline` | `not_a_leaf` | `net_set_accept_deadline` | none | not yieldable | runtime | net.c:1867: assigns `l->accept_deadline_ms` on the listener. No syscall. |
| `__setDeadline` | `not_a_leaf` | `net_set_deadline` | none | not yieldable | runtime | net.c:1852: assigns BOTH read_deadline_ms and write_deadline_ms from one value. No syscall — it is literally a two-line convenience over the other two setters. |
| `__setReadDeadline` | `not_a_leaf` | `net_set_read_deadline` | none | not yieldable | runtime | net.c:1824: validates the int and assigns `c->read_deadline_ms = deadline`. There is NO syscall — no SO_RCVTIMEO, nothing. The field is read back only by the native read paths (ne… |
| `__setWriteDeadline` | `not_a_leaf` | `net_set_write_deadline` | none | not yieldable | runtime | net.c:1838: identical body assigning `c->write_deadline_ms`. No syscall. |
| `__udpBind` | `not_a_leaf` | `net_udp_bind_handle` | POSIX socket(2)/bind(2)/inet_pton(3)/fcntl(O_… | not yieldable; bind does not block | both | net.c:2062 net_udp_bind_handle INFERS THE ADDRESS FAMILY BY PARSING THE STRING — `if (addr[0] && strchr(addr,':')) family = AF_INET6;` — then branches into two full sockaddr build… |
| `__udpFromHost` | `not_a_leaf` | `net_udp_from_host` | none | not yieldable | runtime | net.c:2316 net_udp_from_host interns and returns conn->udp_from_host, the char[] that __udpRecvInto filled with inet_ntop. No syscall. It is the read half of the out-of-band sende… |
| `__udpFromPort` | `not_a_leaf` | `net_udp_from_port` | none | not yieldable | runtime | net.c:2324 net_udp_from_port returns conn->udp_from_port. No syscall. Read half of the same out-of-band channel. |
| `__udpRecvInto` | `not_a_leaf` | `net_udp_recv_into_yieldable` | POSIX recvfrom(2)/inet_ntop(3) | yieldable; parks for readability under the deadline | both | net.c:2285 + net_udp_recv_step (net.c:2237): recvfrom into the caller buffer, then a family branch that inet_ntop's the sender into conn->udp_from_host and stores conn->udp_from_p… |
| `__udpSendTo` | `not_a_leaf` | `net_udp_send_to_yieldable` | POSIX sendto(2) | yieldable; parks for writability under the deadline | both | net.c:2161: validates the port range, re-checks net_is_literal_ip, resolves through xr_dns_resolve, builds a family-dependent sockaddr_storage with htons, converts a relative time… |
| `__writeBytes` | `not_a_leaf` | `net_write_bytes_yieldable` | POSIX send(2), Linux io_uring IORING_OP_SEND,… | yieldable; parks for writability under the write de… | both | net.c:1115 net_write_bytes_yieldable: a `while (written < len)` drain loop over xr_socket_send, an EAGAIN park handoff to net_write_wait, a parallel TLS `while` loop over xr_tls_c… |

## `os` — 26 entries

`host_abi_leaf`: 17  `runtime_leaf`: 1  `not_a_leaf`: 8

| entry | class | vm symbol | ABI | effect | provider | evidence (short) |
|---|---|---|---|---|---|---|
| `__clock` | `host_abi_leaf` | `os_clock` | C clock(3) | not yieldable | both | os.c:557 os_clock: `xr_float((double) clock() / CLOCKS_PER_SEC)`. |
| `__cpuCount` | `host_abi_leaf` | `os_cpuCount` | POSIX sysconf(3) / Win32 GetSystemInfo | not yieldable | both | os.c:348 os_cpuCount: POSIX `sysconf(_SC_NPROCESSORS_ONLN)` clamped with `n > 0 ? n : 1`; Windows GetSystemInfo().dwNumberOfProcessors. One host query plus a default. |
| `__exit` | `host_abi_leaf` | `os_exit` | C exit(3) | not yieldable; terminates the process | both | os.c:223 os_exit: extracts an optional int and calls `exit(code)`. Nothing else executes. |
| `__freeMemory` | `host_abi_leaf` | `os_freeMemory` | Win32 GlobalMemoryStatusEx / Linux sysinfo(2)… | not yieldable | both | os.c:391 os_freeMemory: Win32 GlobalMemoryStatusEx().ullAvailPhys; Linux sysinfo().freeram*mem_unit; macOS host_statistics64(HOST_VM_INFO64) with the definition `(free_count + ina… |
| `__getcwd` | `host_abi_leaf` | `os_getcwd` | POSIX getcwd(3) / GetCurrentDirectory | not yieldable | both | os.c:244 os_getcwd -> xr_fs_getcwd = getcwd(3) into an XR_PATH_MAX stack buffer. Byte-for-byte the same body as io.__cwd (io.c:779). |
| `__getenv` | `host_abi_leaf` | `os_getenv` | POSIX getenv(3) / Win32 GetEnvironmentVariabl… | not yieldable; cannot block | both | os.c:132 os_getenv -> os_core_getenv: POSIX `getenv(name)`; Windows GetEnvironmentVariableA into a 32 KiB thread-local, distinguishing ERROR_ENVVAR_NOT_FOUND from an empty value. … |
| `__getpid` | `host_abi_leaf` | `os_getpid` | POSIX getpid(2) | not yieldable; cannot block | both | os.c:236 os_getpid -> os_getpid_impl = getpid() (POSIX) / _getpid() (Windows). |
| `__gid` | `host_abi_leaf` | `os_gid` | POSIX getgid(2) | not yieldable | posix (Windows returns a constant 0) | os.c:334 os_gid: `return xr_int(getgid())` on POSIX, hard-coded 0 on Windows. |
| `__hostname` | `host_abi_leaf` | `os_hostname` | POSIX gethostname(2) / Winsock gethostname | not yieldable; local call | both | os.c:256 os_hostname: POSIX one gethostname(buf,256). Windows wraps it in WSAStartup/WSACleanup because Winsock requires initialisation — an ABI precondition, not policy. |
| `__kill` | `host_abi_leaf` | `os_kill` | POSIX kill(2) | not yieldable | posix (Windows is a hard false) | os.c:508 os_kill (argc=1 overload): defaults sig to SIGTERM, then `kill(pid, sig) == 0`. Windows returns false unconditionally — os.kill is simply unimplemented there. |
| `__kill` | `host_abi_leaf` | `os_kill` | POSIX kill(2) | not yieldable | posix (Windows is a hard false) | os.c:508 os_kill (argc=2 overload, aot xrt_os_kill_signal): the identical body with an explicit signal. Both .def entries point at the same VM symbol. |
| `__loadavg` | `host_abi_leaf` | `os_loadavg` | POSIX getloadavg(3) | not yieldable | posix (Windows returns zeros) | os.c:454 os_loadavg: one `getloadavg(avg, 3)` then three pushes into a fresh XrArray; Windows pushes three zeros. No loop over an unknown length, no branch on the result. |
| `__ppid` | `host_abi_leaf` | `os_ppid` | POSIX getppid(2) / Win32 Toolhelp32 snapshot | not yieldable | both | os.c:480 os_ppid: POSIX `getppid()`. Windows has no equivalent, so it walks a CreateToolhelp32Snapshot with Process32First/Process32Next until th32ProcessID matches GetCurrentProc… |
| `__setenv` | `host_abi_leaf` | `os_setenv` | POSIX setenv(3) / Win32 SetEnvironmentVariabl… | not yieldable; process-global mutation, not thread-… | both | os.c:147 os_setenv -> os_setenv_impl: POSIX `setenv(name,value,1)`; Windows SetEnvironmentVariableA plus a documented _putenv_s CRT-view sync. No loop or branch beyond the platfor… |
| `__totalMemory` | `host_abi_leaf` | `os_totalMemory` | Win32 GlobalMemoryStatusEx / macOS sysctlbyna… | not yieldable | both | os.c:364 os_totalMemory: one platform query each — Win32 GlobalMemoryStatusEx().ullTotalPhys, macOS sysctlbyname("hw.memsize"), Linux sysinfo().totalram * mem_unit. Returns 0 on u… |
| `__uid` | `host_abi_leaf` | `os_uid` | POSIX getuid(2) | not yieldable | posix (Windows returns a constant 0) | os.c:322 os_uid: `return xr_int(getuid())` on POSIX, hard-coded 0 on Windows. |
| `__unsetenv` | `host_abi_leaf` | `os_unsetenv` | POSIX unsetenv(3) / Win32 SetEnvironmentVaria… | not yieldable; process-global mutation | both | os.c:162 os_unsetenv -> os_unsetenv_impl: POSIX `unsetenv(name)`; Windows SetEnvironmentVariableA(name,NULL) + _putenv_s(name,""). |
| `__sleep` | `runtime_leaf` | `os_sleep` | none — the runtime timer wheel (src/coro) | yieldable; parks the coroutine, does not block the … | runtime | os.c:541 os_sleep makes NO syscall at all: it validates ms>0 and returns xr_yield_for_timeout(X, ms, os_sleep_done, NULL, result), parking the coroutine on the scheduler's timer w… |
| `__ExecResult` | `not_a_leaf` | `—` | none | not yieldable | runtime | Declaration only: `handle __ExecResult { fields: "const stdout: string, const stderr: string, const exitCode: i64" }`. No vm:/aot: symbol. os.c:62 os_exec_result_new looks the cla… |
| `__environ` | `not_a_leaf` | `os_environ` | POSIX environ / Win32 GetEnvironmentStringsA | not yieldable; cannot block | both | os.c:176 os_environ walks `for (char **env = environ; *env; env++)`, finds '=' with strchr, splits key and value, interns each, and inserts into an XrMap. Windows walks the double… |
| `__exec` | `not_a_leaf` | `os_exec` | POSIX fork(2)/execl(3)/pipe(2)/poll(2)/waitpi… | NOT yieldable: poll(pfds,nfds,-1) and waitpid block… | both | os.c:697 os_exec is a full subprocess driver. POSIX: two pipe(2)s, fork(2), child dup2s and `execl("/bin/sh","sh","-c",cmd)` with _exit(127) on failure, then read_exec_pipes (os.c… |
| `__homedir` | `not_a_leaf` | `os_homedir` | POSIX getenv(3) + getpwuid(3) | not yieldable; the passwd branch may block on NSS | both | os.c:310 os_homedir -> xr_os_core_homedir (xr_os_core.h:145): getenv("HOME"), then (Windows) getenv("USERPROFILE"), then the system getpwuid(getuid())->pw_dir. A three-source prec… |
| `__spawn` | `not_a_leaf` | `os_spawn` | POSIX fork(2)/execvp(3)/pipe(2)/poll(2)/waitp… | NOT yieldable: poll(-1)/WaitForSingleObject(INFINIT… | both | os.c:871 os_spawn builds an argv vector, then on POSIX repeats the whole os_exec pipe/fork/poll/waitpid machinery without the shell; on Windows it builds a CommandLineToArgvW-comp… |
| `__tmpdir` | `not_a_leaf` | `os_tmpdir` | none (only getenv(3), itself already a leaf) | not yieldable; cannot block | both | os.c:282 os_tmpdir is a bare call to xr_os_core_tmpdir (xr_os_core.h:94), which is nothing but a precedence chain: TMPDIR, then TMP, then TEMP, each guarded by xr_os_core_has_env_… |
| `__uptime` | `not_a_leaf` | `os_uptime` | macOS sysctlbyname(kern.boottime) / Linux sys… | not yieldable | both | os.c:422 os_uptime is a source-selection fallback chain, not one query. macOS: sysctlbyname("kern.boottime") then `time(NULL) - boottime.tv_sec`, and IF THAT FAILS it falls back t… |
| `__username` | `not_a_leaf` | `os_username` | POSIX getpwuid(3)+getuid(2) / Win32 GetUserNa… | not yieldable; may touch NSS/LDAP and block | both | DIVERGENT bodies. VM (os.c:291): POSIX `getpwuid(getuid())->pw_name`, null on failure. AOT (xrt_os.h:318): the same getpwuid, then a fallback chain getenv("USER") then getenv("LOG… |

## `sys` — 16 entries

`host_abi_leaf`: 9  `runtime_leaf`: 3  `not_a_leaf`: 4

| entry | class | vm symbol | ABI | effect | provider | evidence (short) |
|---|---|---|---|---|---|---|
| `__dylibClose` | `host_abi_leaf` | `sys_dylib_close` | POSIX dlclose(3) / Win32 FreeLibrary | not yieldable; runs library destructors | both | sys.c:646 sys_dylib_close: null handle is a no-op success, otherwise one xr_dylib_close = dlclose/FreeLibrary. |
| `__dylibLastError` | `host_abi_leaf` | `sys_dylib_last_error` | POSIX dlerror(3) / Win32 FormatMessage(GetLas… | not yieldable | both | sys.c:657 sys_dylib_last_error: one xr_dylib_last_error() (dlerror on POSIX, a FormatMessage'd thread-local on Windows), interned into a string, empty when null. |
| `__dylibOpen` | `host_abi_leaf` | `sys_dylib_open` | POSIX dlopen(3) / Win32 LoadLibraryA | not yieldable; dlopen can block on disk and run lib… | both | sys.c:625 sys_dylib_open -> xr_dylib_open, documented (src/os/os_dylib.h) as exactly dlopen(path, RTLD_NOW\|RTLD_LOCAL) / LoadLibraryA. The AOT twin (xrt_sys.h:203) inlines that s… |
| `__dylibSymbol` | `host_abi_leaf` | `sys_dylib_symbol` | POSIX dlsym(3) / Win32 GetProcAddress | not yieldable; cannot block meaningfully | both | sys.c:634 sys_dylib_symbol: casts the i64 back to XrDylib*, then one xr_dylib_sym = dlsym/GetProcAddress; null symbol becomes xr_null. |
| `__pipeClose` | `host_abi_leaf` | `sys_pipe_close` | POSIX close(2) / Win32 CloseHandle | not yieldable | both | sys.c:1140 sys_pipe_close: one int check then `xr_pipe_close(handle) == 0`. |
| `__pipeOpen` | `host_abi_leaf` | `sys_pipe_open` | POSIX pipe(2) / Win32 CreatePipe | not yieldable; cannot block | both | sys.c:901 sys_pipe_open: one xr_pipe_create(&pipe, NULL), then pushes the two handles into a 2-element i64 array; on allocation failure it correctly closes both ends. |
| `__processKill` | `host_abi_leaf` | `sys_process_kill` | POSIX kill(2) / Win32 TerminateProcess | not yieldable | both | sys.c:893 sys_process_kill: two int checks then one xr_proc_kill = kill(pid,signal) (proc_unix.c:238) / TerminateProcess. |
| `__processSpawn` | `host_abi_leaf` | `sys_process_spawn` | POSIX fork(2)/setsid(2)/setpgid(2)/dup2(2)/ch… | not yieldable; the detached path blocks briefly in … | both | sys.c:732 sys_process_spawn only marshals: it validates 9 args, builds argv, validates env keys (no '='), unpacks three optional pipe handles, and calls xr_proc_spawn_ex. All the … |
| `__processTryWait` | `host_abi_leaf` | `sys_process_try_wait` | POSIX waitpid(2) WNOHANG / Win32 WaitForSingl… | not yieldable; WNOHANG does not block | both | sys.c:879 sys_process_try_wait -> xr_proc_try_wait (proc_unix.c:209): one waitpid(pid,&status,WNOHANG) with an EINTR restart, mapping r==0 to RUNNING (script null) and decoding th… |
| `__osMutexNew` | `runtime_leaf` | `sys_mutex_new` | none directly — the runtime shared heap and t… | not yieldable; construction cannot block | runtime | sys.c:349 sys_mutex_new allocates an OsMutex instance through sys_shared_instance_new on the isolate's shared heap and throws XR_ERR_OUT_OF_MEMORY on failure. No syscall: it is an… |
| `__threadLocalAlive` | `runtime_leaf` | `sys_thread_local_alive` | none — the runtime thread-object registry | not yieldable | runtime | sys.c:618 sys_thread_local_alive: type-checks the arg then `xr_thread_obj_threadlocal_id_alive(id)`. A single registry lookup. |
| `__threadLocalId` | `runtime_leaf` | `sys_thread_local_id` | none — the runtime thread-object registry | not yieldable | runtime | sys.c:611 sys_thread_local_id: `xr_int(xr_thread_obj_threadlocal_current_id())`. A read of the runtime's own thread-object registry, not an OS TID. |
| `__onSignal` | `not_a_leaf` | `sys_on_signal` | POSIX sigaction(2) / C signal(3) | the installer is not yieldable; the spawned dispatc… | both | sys.c:1263 sys_on_signal does four separable things: (1) sigaction/signal installs a handler that only atomically sets a pending flag; (2) it allocates a dispatch context carrying… |
| `__pipeRead` | `not_a_leaf` | `sys_pipe_read_yieldable` | POSIX read(2) on a pipe fd / Win32 ReadFile | yieldable on POSIX via a 1 ms timer poll; blocks th… | both | sys.c:1012 sys_pipe_read_yieldable: when the caller can yield it allocates a max_bytes staging buffer and enters sys_pipe_read_yield_step, a POLLING LOOP over xr_pipe_try_read tha… |
| `__pipeWrite` | `not_a_leaf` | `sys_pipe_write_yieldable` | POSIX write(2) on a pipe fd / Win32 WriteFile | yieldable on POSIX via a 1 ms timer poll; blocks th… | both | sys.c:1104 sys_pipe_write_yieldable is __pipeRead's mirror: it COPIES the whole input array into a heap buffer, then sys_pipe_write_yield_step polls xr_pipe_try_write and yields o… |
| `__processWait` | `not_a_leaf` | `sys_process_wait_yieldable` | POSIX waitpid(2) / MSVCRT _cwait | VM: yieldable via a 1 ms timer poll. AOT: blocks th… | both | DIVERGENT bodies. VM (sys.c:857 sys_process_wait_yieldable): if the caller can yield it runs sys_process_wait_yield_step, a POLLING LOOP that calls xr_proc_try_wait then xr_yield_… |

## `cluster` — 4 entries

`not_a_leaf`: 4

| entry | class | vm symbol | ABI | effect | provider | evidence (short) |
|---|---|---|---|---|---|---|
| `__join` | `not_a_leaf` | `cluster_join` | indirectly POSIX connect(2) via the cluster t… | yieldable (XrCFuncResult); parks on the netpoll dur… | runtime | cluster.c:1130 cluster_join parses a host:port string by hand (strrchr for the last ':', bounds checks, strtol with errno and end-pointer validation, port range checks), then buil… |
| `__listen` | `not_a_leaf` | `cluster_listen_fn` | none | not yieldable; the returned Channel is what the cal… | runtime | cluster.c:1344 cluster_listen_fn validates a capacity against XR_CLUSTER_SUBSCRIPTION_CAPACITY_MAX then calls cluster_transport_listen (cluster_topic.c:350), which inserts the pat… |
| `__start` | `not_a_leaf` | `cluster_start_primitive` | indirectly POSIX socket/bind/listen via xr_io… | not yieldable itself; it spawns long-lived yieldabl… | runtime | cluster.c:1097 cluster_start_primitive marshals 8 scalars then calls cluster_runtime_start (cluster.c:445), which is a distributed-runtime bootstrap: a printable-ASCII validation … |
| `__stop` | `not_a_leaf` | `cluster_stop_fn` | none directly | not yieldable; waits on the background coroutines i… | runtime | cluster.c:1218 cluster_stop_fn calls cluster_runtime_stop, which tears down the whole subsystem: signalling the heartbeat and accept coroutines, draining nodes, releasing topics a… |

---

## The 57 `not_a_leaf` entries — what each one is hiding

| entry | the policy that must move into `.xr` | thinnest ABI that remains |
|---|---|---|
| `io.__FileStat` | The whole record is pure data. io.xr's FileStat class is already its twin: delete the __FileStat handle and have __stat return the 10 scalars (or a fixed-shape tuple) that io.xr assembles. | — |
| `io.__appendFile` | The short-write drain loop and the append open-mode choice belong in io.xr. | __fdOpen(path, flags, mode) -> i64, __fdWrite(fd, bytes, off, len) -> i64 (one write(2), returns n), __fdClose(fd). |
| `io.__copyFile` | The short-transfer retry loop, the buffered fallback loop, and the 'which fast path' selection all move to io.xr. | __fdCopyRange(srcFd, dstFd, off, len) -> i64 (one sendfile/copy_file_range/fcopyfile attempt, returns bytes moved or -errno), plus the fd open/close leaves. |
| `io.__mkdir` | The 'EEXIST and it is a directory counts as success' rule is io.xr policy. | __mkdir(path, mode) -> i64 returning 0 or -errno; io.xr decides whether EEXIST is success and issues the confirming __isDir. |
| `io.__mkdirp` | The entire segment walk, root detection, and separator normalisation belong in io.xr (path.Path already owns separator policy). | the errno-returning __mkdir leaf above; io.xr loops. |
| `io.__readDir` | The iteration loop, the dot-entry filter, and the array build move to io.xr. | __dirOpen(path) -> i64, __dirNext(handle) -> string?, __dirClose(handle) -> bool. |
| `io.__readDirRecursive` | The recursion, depth cap, path joining, and relative-path derivation are io.xr's job. | the __dirOpen/__dirNext/__dirClose triple above plus __isDir. |
| `io.__readFile` | The whole-file read loop, the SQ-full fallback, the size gate and the buffering are io.xr policy. | __fdOpen / __fdReadInto(fd, buf, off, len) -> i64 (one read, yieldable) / __fdClose / __fdSize. |
| `io.__readFileBytes` | Same decomposition as __readFile: an fd leaf plus an io.xr read loop. | — |
| `io.__readLines` | The scan, the CRLF trim, and the tail-line rule are text policy and belong in io.xr (or stdlib/text). No new leaf is needed at all once __readFile is decomposed. | — |
| `io.__readStdin` | The grow-and-read-to-EOF loop belongs in io.xr. | __fdReadInto(0, buf, len) -> i64. |
| `io.__readStdinBytes` | Same as __readStdin. The Windows binary-mode flip is the only genuinely native bit and is a one-line __setBinaryMode(fd) leaf. | — |
| `io.__removeAll` | The recursion, the child-path joins, the dot filter and the delete ordering are io.xr policy. | the __dirOpen/__dirNext/__dirClose triple plus __remove and __rmdir. |
| `io.__tempDir` | The TMPDIR/TMP/TEMP precedence and the template join are os.xr/path policy. | __mkdtemp(template) -> string? taking a caller-built template, plus __getenv (already a leaf). |
| `io.__tempFile` | The env chain and template join move to io.xr/os.xr; the discarded-fd design should be replaced by __mkstemp(template) -> (path, fd) so the caller keeps the exclusive handle. | — |
| `io.__touch` | The update-then-create fallback belongs in io.xr. | __utimeNow(path) -> i64 (0 or -errno) plus the __fdOpen/__fdClose pair. |
| `io.__writeFile` | The write-drain loop, the SQ-full fallback, the open flags and the 0644 mode are io.xr policy. | __fdOpen(path, flags, mode) / __fdWrite(fd, bytes, off, len) -> i64 (yieldable) / __fdClose. |
| `io.__writeFileBytes` | Same decomposition as __writeFile. | — |
| `io.__writeStderr` | The drain loop and the flush-every-write rule move to io.xr. | __fdWrite(2, bytes, off, len) -> i64. |
| `io.__writeStdout` | Same as __writeStderr: a single __fdWrite leaf plus an io.xr loop. | — |
| `net.__CopyBidirectionalResult` | A two-i64 pair. It exists only because __copyBidirectional is native; once that migrates, net.xr returns its own tuple or class. | — |
| `net.__accept` | The retry/park loop, the deadline arithmetic, the TCP_NODELAY choice and the errno classification are net.xr policy. | __acceptTry(fd) -> i64 (new fd, -EAGAIN, or -errno) plus a yieldable __waitReadable(fd, timeoutMs). |
| `net.__connectFd` | Socket option choice, the EINPROGRESS dance, the SO_ERROR probe, the errno classification and the thread-local error slot are net.xr policy. | __socket(af,type) -> i64, __setNonblock(fd), __setsockoptInt(fd,level,opt,val), __connectStart(fd,addrBytes,port) -> i64 errno, a yieldable __waitWritable(fd,timeoutMs), and __soError(fd) -> i64. |
| `net.__copyBidirectional` | This is a proxy pump, not a boundary: the two-direction fan-out, the half-close-on-EOF rule, the buffer size and the join are net.xr policy over __readInto/__writeBytes/__shutdownWrite. Deleting it also removes the VM/AOT behavioural split and the TLS gap. | — |
| `net.__hasTLS` | It is a build flag, not a boundary. core.def already supports module constants (`const` entries with vm_value/aot_const); declare `net.hasTLS: bool` as a generated constant and delete the function. | — |
| `net.__lastCode` | The errno->portable-code classification and its out-of-band slot are net.xr policy. Once the I/O leaves return -errno directly there is nothing to read back. | — |
| `net.__lastConnectCode` | This is not a boundary at all — it is a workaround for a compiler limitation on unions of a builtin native class with a scalar. It disappears when __connectFd is decomposed into fd-level leaves that return errno directly. | — |
| `net.__lastErrno` | Same out-of-band channel as __lastCode. When the I/O leaves return -errno the diagnostic is the return value. | — |
| `net.__listenFd` | The v6-then-v4 fallback, the V6ONLY and SO_REUSEADDR choices, and the ephemeral-port readback are net.xr policy. | __socket/__setsockoptInt/__bind(fd,addrBytes,port)/__listen(fd,backlog)/__getsockname(fd) -> (addrBytes, port). |
| `net.__readInto` | The EINTR retry, the exact-mode accumulation, the partial-progress policy, the TLS/TCP dispatch and the errno classification are net.xr policy. | __recvInto(fd, buf, off, len) -> i64 (one recv, -EAGAIN sentinel), a yieldable __waitReadable, and a separate __tlsRead leaf. |
| `net.__resolveAll` | The literal-IP short-circuit, the result loop and the string formatting move to net.xr. | a yieldable __dnsResolve(host, family) returning a count plus __dnsResolveAt(i) -> raw 4/16-byte address, or an __inetNtop/__inetPton pair so net.xr formats. |
| `net.__setAcceptDeadline` | Pure Xray state; moves with __accept. | — |
| `net.__setDeadline` | A convenience wrapper with zero native content; net.xr can call the other two setters (or set its own fields). | — |
| `net.__setReadDeadline` | Pure Xray state. It is native only because NetConn is a native class and the native read loop consumes it. When __readInto moves to net.xr the deadline becomes an ordinary field of an .xr connection type. | — |
| `net.__setWriteDeadline` | Pure Xray state; moves with __writeBytes. | — |
| `net.__udpBind` | The ':' family sniff and the sockaddr construction are net.xr policy. | __socket(af,type), __bind(fd, addrBytes, port), __setNonblock(fd), with net.xr deciding the family from a parsed address. |
| `net.__udpFromHost` | Delete along with the out-of-band channel: __udpRecvInto should return the sender address as part of its result. | — |
| `net.__udpFromPort` | Delete along with the out-of-band channel. | — |
| `net.__udpRecvInto` | The EAGAIN retry, the sender formatting and the out-of-band sender channel are net.xr policy. | __recvFrom(fd, buf, off, len) -> (n, addrBytes, port) as a real return value, plus a yieldable __waitReadable. |
| `net.__udpSendTo` | The literal check, the sockaddr construction, the deadline arithmetic, the EAGAIN retry and the errno classification are net.xr policy. | __sendTo(fd, bytes, off, len, addrBytes, port) -> i64 (one sendto, -EAGAIN sentinel) plus a yieldable __waitWritable. |
| `net.__writeBytes` | The whole-buffer drain loop, the TLS want-mode dispatch and the errno classification are net.xr policy. | __sendFrom(fd, bytes, off, len) -> i64 (one send, -EAGAIN sentinel) plus a yieldable __waitWritable, and a separate __tlsWrite leaf. |
| `os.__ExecResult` | Pure data. Once __exec/__spawn are decomposed there is nothing to declare: os.xr assembles its own result class from the three scalars. | — |
| `os.__environ` | The walk, the '=' split and the Map build belong in os.xr. | __environCount() -> i64 and __environAt(i) -> string? (one raw "KEY=VALUE" entry), or a single __environRaw() -> Array<string>. |
| `os.__exec` | Everything except the fork/exec pair moves to os.xr: the two-pipe drain loop, the growth buffers, the EINTR restarts, the exit-status classification, and the /bin/sh -c wrapping. | the existing sys.__processSpawn + sys.__pipeOpen/__pipeTryRead/__pipeClose + sys.__processTryWait leaves — os.__exec is already fully expressible over the sys module. |
| `os.__homedir` | The HOME/USERPROFILE/passwd precedence is os.xr policy. | __passwdHome() -> string? = getpwuid(getuid())->pw_dir, composed in os.xr with the existing __getenv. |
| `os.__spawn` | Same as __exec: argv assembly, the Windows quoting rules, both drain loops and the exit classification belong in os.xr over sys.__processSpawn + the pipe leaves. Note there is no aot: symbol at all for this entry, so it is VM-only today. | — |
| `os.__tmpdir` | The entire body is env-precedence policy expressible in three lines of os.xr over the existing __getenv leaf. Delete the symbol; no replacement ABI is needed. | — |
| `os.__uptime` | The failure-driven choice between two sources with different meanings is os.xr policy. | __bootTimeSec() -> i64? (0 or -1 when unavailable) plus the existing monotonic clock leaf; os.xr picks. |
| `os.__username` | The USER/LOGNAME fallback chain is os.xr policy over the existing __getenv leaf. | __passwdName() -> string? = getpwuid(getuid())->pw_name and nothing else. |
| `sys.__onSignal` | The generation policy, the 10 ms poll loop, the dispatcher coroutine and the closure invocation are all sys.xr policy. | __signalInstall(sig) -> bool (one sigaction) and __signalTake(sig) -> bool (one atomic_exchange of the pending flag); sys.xr spawns its own loop. |
| `sys.__pipeRead` | The 1 ms retry loop and the double buffering are sys.xr policy. | __pipeTryRead(handle, buf, maxBytes) -> i64 (n, 0 = EOF, -1 = would-block, -2 = error) reading straight into a caller-supplied Array<u8>. |
| `sys.__pipeWrite` | The retry loop and the defensive copy are sys.xr policy. | __pipeTryWrite(handle, bytes, off, len) -> i64 over the caller's array. |
| `sys.__processWait` | The 1 ms poll loop is pure sys.xr policy: `while (true) { let c = __processTryWait(id); if (c != null) return c; sleep(1) }`. | the existing __processTryWait leaf. |
| `cluster.__join` | String parsing and a wire handshake are textbook Xray work. | the net connect/read/write leaves. |
| `cluster.__listen` | Topic pattern matching and subscription bookkeeping are ordinary Xray over an existing Channel primitive. | — |
| `cluster.__start` | This is a native library, not a boundary. Name validation, heartbeat tuning, the tombstone array, the topic trie and the accept/heartbeat loops are all ordinary Xray. The genuine leaves underneath are the net socket primitives and the TLS provider that already exist. | — |
| `cluster.__stop` | Lifecycle management of a native subsystem that should not be native. Nothing here is a host ABI. | — |

---

## Class summaries

### `host_abi_leaf` — 49 entries

These are the entries whose whole body is one host call plus argument marshalling.
They cluster into six families:

- **stat-family path predicates** (`io.__exists`, `__isFile`, `__isDir`, `__isSymlink`, `__fileSize`, `__stat`) — one `lstat`/`stat` and a predicate.
- **single-syscall path mutations** (`io.__remove`, `__rename`, `__chmod`, `__chdir`, `__symlink`, `__cwd`, `__readlink`, `__realpath`).
- **stdio handle primitives** (`io.__fileOpen`, `__fileRead`, `__fileClose`) — carrying a raw-`FILE*`-as-`i64` ownership hazard (see below).
- **process and environment scalars** (`os.__getenv`/`__setenv`/`__unsetenv`/`__exit`/`__getpid`/`__getcwd`/`__hostname`/`__uid`/`__gid`/`__cpuCount`/`__ppid`/`__kill`×2/`__totalMemory`/`__freeMemory`/`__loadavg`/`__clock`).
- **dylib + process + pipe ABI** (`sys.__dylibOpen`/`__dylibSymbol`/`__dylibClose`/`__dylibLastError`, `sys.__processSpawn`/`__processTryWait`/`__processKill`, `sys.__pipeOpen`/`__pipeClose`).
- **socket handle primitives** (`net.__close`, `__fd`, `__shutdown`/`__shutdownRead`/`__shutdownWrite`, `__nowMs`).

`sys.__processSpawn` is the strongest case in the whole set: the body it delegates to runs **between `fork(2)` and `exec`**, where only async-signal-safe code may run, so no managed Xray code can ever live there.

### `runtime_leaf` — 4 entries

`os.__sleep` (parks on the scheduler timer wheel; makes no syscall at all), `sys.__osMutexNew`
(shared-heap object representation for a runtime synchronisation primitive), and
`sys.__threadLocalId` / `sys.__threadLocalAlive` (reads of the runtime's own thread-object registry, not OS TIDs).

### `machine_intrinsic_leaf` — 0 entries

No private `__` entry is a math, bit, SIMD, or memory intrinsic. Those live under the public `math`/`simd`/`mem` modules, outside this inventory.

### `security_provider_leaf` — 1 entry

`net.__tlsHandshake` only. The OpenSSL handshake, certificate verification and record layer are provider code
the compiler cannot verify. The caveat is recorded in its row: the *close-the-conn-on-every-failure* rule and
the deadline arithmetic are `net.xr` policy sitting inside the leaf; the ideal shape is
`__tlsHandshakeStep(conn) -> {done|wantRead|wantWrite|error}`.

### `not_a_leaf` — 57 entries

Six recurring shapes account for nearly all of them:

1. **Drain / grow loops** — `xr_io_core_write_all` (`while (off < len)`) and `xr_io_core_read_all_stream_alloc` (capacity-doubling read-to-EOF) sit under `io.__writeFile`, `__writeFileBytes`, `__appendFile`, `__writeStdout`, `__writeStderr`, `__readStdin`, `__readStdinBytes`.
2. **1 ms timer polling loops** — `sys.__processWait`, `sys.__pipeRead`, `sys.__pipeWrite`, `sys.__onSignal` (10 ms), and `net.__copyBidirectional`'s join all busy-poll via `xr_yield_for_timeout(…, 1, …)`.
3. **Environment precedence chains** — `os.__tmpdir` (TMPDIR→TMP→TEMP→`/tmp`), `os.__homedir` (HOME→USERPROFILE→passwd), `os.__username` (AOT-only USER→LOGNAME), `os.__uptime` (boottime→monotonic). Zero syscalls beyond `getenv`.
4. **Path algorithms** — `io.__mkdirp` (segment walk + Windows drive/UNC root detection), `io.__removeAll` and `io.__readDirRecursive` (recursive descent with joins and a depth cap), `io.__readDir` (iterate + dot-filter).
5. **errno classification and out-of-band error slots** — `net_error_from_errno` plus the `last_error`/`last_errno` handle fields and the thread-local `g_last_connect_code`, exposed as `net.__lastCode`, `net.__lastErrno`, `net.__lastConnectCode`.
6. **Pure state accessors with no ABI at all** — `net.__setReadDeadline`, `__setWriteDeadline`, `__setDeadline`, `__setAcceptDeadline`, `__udpFromHost`, `__udpFromPort`, `net.__hasTLS` (a compile-time `#ifdef` dressed as a call), and the three data-shape declarations.

`cluster`'s four entries are a category of their own: they are the API surface of a ~200 KB C distributed-runtime
subsystem (heartbeats, phi-accrual failure detection, a topic-routing trie, a framing protocol, multicast discovery).
Nothing in them is a host ABI, a runtime primitive, an intrinsic, or a security provider.

---

## Facts worth acting on that surfaced while reading the implementations

1. **A live debug probe is committed in `io.__exists`.** `stdlib/io/io.c:596-613` still contains an
   `fprintf(stderr, "[DBG io_exists] argc=%d tag=%d …")` block that also dumps the `Path` instance's class name,
   field count and first field name. It fires on **every** `io.exists()` call in the VM. This is unrelated to the
   leaf question and should be removed on its own.
2. **`io.__exists` / `__isFile` / `__isDir` disagree between VM and AOT.** The VM path goes through
   `xr_fs_stat`, which uses **`lstat(2)`** (`src/os/unix/fs_unix.c:57`); the AOT twins
   `xrt_io_exists`/`xrt_io_is_file`/`xrt_io_is_dir` (`src/aot/xrt_io.h:129/139/156`) use **`stat(2)`**.
   A symlink pointing at a file answers `isFile=false` under the VM and `isFile=true` under AOT.
3. **`os.__username` diverges between backends.** The VM body (`stdlib/os/os.c:291`) returns null when
   `getpwuid` fails; the AOT body (`src/aot/xrt_os.h:318`) falls back to `getenv("USER")` then
   `getenv("LOGNAME")`. The AOT file also gates on `_WIN32` rather than `XR_OS_WINDOWS`.
4. **`net.__copyBidirectional` is two different programs.** VM: three coroutines + atomics + a 1 ms poll join,
   TLS supported. AOT (`src/aot/xrt_net.h:917`): a single-threaded blocking `select(2)` loop that
   **rejects TLS connections outright** with `XRT_NETERR_TLS`.
5. **`sys.__processWait` also diverges.** VM: a 1 ms `try_wait` polling loop that yields. AOT: a plain blocking
   `waitpid`. One `.def` entry, two suspension semantics.
6. **Raw pointers are laundered through `i64` into script.** `io.__fileOpen` returns a `FILE*` cast to `i64`
   and `io.__fileRead`/`__fileClose` cast an arbitrary script integer straight back to `FILE*`
   (`stdlib/io/io.c:235`, `:245`; same in `src/aot/xrt_io.h:923/934`). `sys.__dylibOpen`/`__dylibSymbol`/`__dylibClose`
   do the same with `XrDylib*`, and `__dylibSymbol` hands back a raw code pointer as `Ptr<u8>` with no lifetime
   tie to the library. These ownership contracts are unsound as written, independent of the leaf classification.
7. **`os.__exec` and `os.__spawn` block the worker thread outright.** `poll(pfds, nfds, -1)` and blocking
   `waitpid` on POSIX, `WaitForSingleObject(…, INFINITE)` plus a `Sleep(1)` busy-poll on Windows.
   Neither is yieldable, so a single subprocess stalls every coroutine on that worker.
   Both are also **fully expressible today** over the `sys` module's own leaves
   (`__processSpawn` + `__pipeOpen`/`__pipeRead`/`__pipeClose` + `__processTryWait`) — this is the
   clearest duplicate implementation in the set.
8. **`os.__spawn` has no `aot:` symbol at all** (`stdlib/defs/core.def`), so it is VM-only.
9. **Two exact duplicates already exist.** `io.__cwd` (`stdlib/io/io.c:779`) and `os.__getcwd`
   (`stdlib/os/os.c:244`) are the same `xr_fs_getcwd` body. `net.__nowMs` (`stdlib/net/net.c:165`) is
   `xr_time_monotonic_ns()/1e6`, which is exactly the already-public `time.monotonic`.
10. **`sys.__onSignal` silently supports only SIGTERM and SIGINT.** `sys_signal_pending_slot`
    (`stdlib/sys/sys.c:1159`) returns NULL for every other signal number, so the call returns `false`
    despite the `(signal: i64, handler: fn(): ())` signature promising any signal.
11. **`io.__tempFile` is racy by construction.** It calls `mkstemp(tpl)` and then immediately `close(fd)`
    (`stdlib/io/io.c:1256`), returning only the path — so the caller must re-open by name and loses
    mkstemp's exclusivity guarantee.
12. **`net.__udpRecvInto` returns the sender out of band.** It writes `conn->udp_from_host` /
    `conn->udp_from_port` on the shared handle (`stdlib/net/net.c:2237`), read back later by
    `__udpFromHost`/`__udpFromPort`. Two coroutines receiving on the same UDP handle race on that state.
13. **`net.__lastConnectCode` exists to work around a compiler limitation.** The source comment at
    `stdlib/net/net.c:466` states that `__connectFd` cannot return `NetConn | int` because a union of a
    builtin native class with a scalar "miscompiles suspended handle results in coroutine frames".
    The thread-local error slot is the workaround, not a design choice.
14. **A `not_a_leaf` verdict is not always a big job.** `os.__tmpdir` is *only* an env precedence chain and
    `net.__hasTLS` is *only* a compile-time `#ifdef`; both can be deleted today (the latter by re-declaring it
    as a `.def` module constant). At the other extreme, `io.__readFile` carries a full io_uring state machine
    plus a `pread` EINTR fallback loop, and the four `cluster` entries front an entire distributed runtime.

### Where the reading is uncertain

- **`sys.__processTryWait`** is filed `host_abi_leaf`, but its `WIFEXITED(status) ? WEXITSTATUS(status) : -1`
  collapse is lossy: a signal-terminated child is indistinguishable from an error. The `WIFEXITED` macros
  themselves are POSIX ABI (a raw `status` is opaque without them), so the decode is not policy — the
  *collapse to -1* is. Returning the raw status would settle it cleanly.
- **`io.__stat`** is filed `host_abi_leaf` although it issues two syscalls (`stat` + `lstat`), the second one
  purely to derive `isSymlink`. If the rule is "exactly one host call", this is a `not_a_leaf`; splitting it
  into `__stat` and `__lstat` would remove the ambiguity.
- **`net.__shutdownRead`/`__shutdownWrite`/`__shutdown`** are filed `host_abi_leaf` even though they write
  `net_error_from_errno(errno)` into the handle. That classification is *redundant* with `__lastErrno`, which
  already exposes the raw value, so it is bookkeeping rather than a load-bearing decision — but a stricter
  reading of "no error classification in a leaf" would move all three to `not_a_leaf`.
- **`os.__freeMemory`** encodes a definition ("free = free_count + inactive_count" on macOS). One arithmetic
  expression over one host call, so it is filed as a leaf; a reviewer who considers that a policy choice
  would move it.
- **`net.__resolveAll`'s** replacement ABI is the least obvious in the set. `getaddrinfo` inherently returns a
  list, so the thin leaf needs either a resolver handle with a `next` step or a fixed-capacity out-array; the
  entry above proposes the former but this needs a design decision, not just a refactor.

