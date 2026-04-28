/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_proc.h - Cross-platform process spawning and introspection.
 *
 * Why a shim:
 *   POSIX fork() + execvp() + waitpid() and Win32 CreateProcess /
 *   WaitForSingleObject / GetExitCodeProcess do not share types,
 *   semantics, or error-reporting conventions. Callers in cli/,
 *   module/ and elsewhere reached for the POSIX side directly,
 *   which would not compile on Windows.
 *
 *   This header gives a minimal, opinionated process surface:
 *     - Spawn a child with an argv vector. The child inherits the
 *       parent's stdio, cwd, and environment. Search-PATH semantics
 *       match execvp on POSIX and CreateProcess(lpApplicationName=NULL)
 *       on Windows (the resolver walks PATHEXT for unqualified names).
 *     - Wait for a child, returning its non-negative exit code on a
 *       clean exit or -1 if the child was signaled / terminated
 *       abnormally.
 *     - Query the current process id.
 *     - Detect whether a debugger is attached.
 *
 *   Anything more (kill, signal forwarding, pipe redirection, env
 *   overrides, working-directory overrides) is intentionally out of
 *   scope until a concrete in-tree caller needs it.
 */

#ifndef XR_OS_OS_PROC_H
#define XR_OS_OS_PROC_H

#include <stdbool.h>
#include <stdint.h>

#include "../base/xdefs.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque process handle. Positive on success, -1 on error.
//
// On POSIX this is the pid_t value returned by fork(); on Windows
// it is a small numeric token that maps to an internally-tracked
// HANDLE. Callers must not assume the int64_t is a Win32 HANDLE
// or a pid_t — go through xr_proc_wait / xr_proc_self_pid for
// cross-platform behaviour.
typedef int64_t XrProcId;

#define XR_PROC_INVALID ((XrProcId) - 1)

// Spawn a child process running `prog`. `argv` is a NULL-terminated
// array; argv[0] is conventionally the program name. The child
// inherits the parent's stdin / stdout / stderr, current working
// directory, and environment. PATH is searched for unqualified
// program names (POSIX execvp / Win32 CreateProcessA with
// lpApplicationName=NULL).
//
// Returns the child's process id on success, XR_PROC_INVALID on
// failure (no fork/CreateProcess possible, exec failed, etc.).
XR_FUNC XrProcId xr_proc_spawn(const char *prog, const char *const argv[]);

// Wait for the child identified by `pid` to exit. Blocks until the
// child terminates. On a clean exit, writes the child's exit status
// (0..255) to `*exit_code` and returns 0. If the child was signaled
// or terminated abnormally, writes -1 to `*exit_code` and still
// returns 0. Returns -1 only when the wait itself failed.
//
// `exit_code` may be NULL if the caller does not need the value.
XR_FUNC int xr_proc_wait(XrProcId pid, int *exit_code);

// Current process id. Always succeeds.
XR_FUNC int64_t xr_proc_self_pid(void);

// Returns true if a debugger (lldb / gdb / Visual Studio) is
// attached to the current process at the time of the call. Best
// effort: macOS uses sysctl P_TRACED, Linux reads /proc/self/status
// TracerPid, Windows uses IsDebuggerPresent. Returns false on
// platforms where the query is unsupported.
XR_FUNC bool xr_proc_debugger_attached(void);

#ifdef __cplusplus
}
#endif

#endif  // XR_OS_OS_PROC_H
