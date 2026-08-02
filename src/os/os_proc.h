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
 *       parent's stdio and environment by default. Search-PATH semantics
 *       match execvp on POSIX and CreateProcess(lpApplicationName=NULL)
 *       on Windows (the resolver walks PATHEXT for unqualified names).
 *     - Wait for a child, returning its non-negative exit code on a
 *       clean exit or -1 if the child was signaled / terminated
 *       abnormally.
 *     - Send a portable termination signal to a child.
 *     - Query the current process id.
 *     - Detect whether a debugger is attached.
 *
 *   Anything more (signal forwarding, pipe redirection) is intentionally out
 *   of scope until a concrete in-tree caller needs it. Environment overrides
 *   are supported as a small add/update overlay on the inherited environment.
 */

#ifndef XR_OS_OS_PROC_H
#define XR_OS_OS_PROC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/xdefs.h"
#include "os_pipe.h"

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

typedef enum XrProcWaitResult {
    XR_PROC_WAIT_ERROR = -1,
    XR_PROC_WAIT_RUNNING = 0,
    XR_PROC_WAIT_EXITED = 1,
} XrProcWaitResult;

typedef struct XrProcSpawnOptions {
    // NULL or empty means inherit the parent's current working directory.
    const char *cwd;
    // Optional environment overrides. NULL / count 0 means inherit the
    // parent's environment unchanged; entries override or add variables.
    const char *const *env_keys;
    const char *const *env_values;
    size_t env_count;
    // Optional stdio redirection. The handle names describe the end consumed by
    // the child process; ownership remains with the caller.
    bool has_stdin;
    XrPipeHandle stdin_read;
    bool has_stdout;
    XrPipeHandle stdout_write;
    bool has_stderr;
    XrPipeHandle stderr_write;
    // When true, the child is released from the wait/tryWait lifecycle. The
    // returned id remains informational; callers must not wait it.
    bool detached;
    // Start the child as the leader of a new process group. This is used by
    // bounded compiler/linker probes so timeout cleanup can terminate the
    // complete subprocess tree without affecting the caller's group.
    bool new_process_group;
} XrProcSpawnOptions;

// Spawn a child process running `prog`. `argv` is a NULL-terminated
// array; argv[0] is conventionally the program name. The child
// inherits the parent's stdin / stdout / stderr and environment. PATH is
// searched for unqualified program names (POSIX execvp / Win32 CreateProcessW
// with lpApplicationName=NULL).
//
// Returns the child's process id on success, XR_PROC_INVALID on
// failure (no fork/CreateProcess possible, exec failed, etc.).
XR_FUNC XrProcId xr_proc_spawn(const char *prog, const char *const argv[]);

// Spawn with structured options. Unsupported / empty fields behave like
// xr_proc_spawn. The options object is intentionally small and grows only when
// a general process capability needs it; it must not become a bag of
// algorithm-specific switches.
XR_FUNC XrProcId xr_proc_spawn_ex(const char *prog, const char *const argv[],
                                  const XrProcSpawnOptions *options);

// Wait for the child identified by `pid` to exit. Blocks until the
// child terminates. On a clean exit, writes the child's exit status
// (0..255) to `*exit_code` and returns 0. If the child was signaled
// or terminated abnormally, writes -1 to `*exit_code` and still
// returns 0. Returns -1 only when the wait itself failed.
//
// `exit_code` may be NULL if the caller does not need the value.
XR_FUNC int xr_proc_wait(XrProcId pid, int *exit_code);

// Non-blocking variant of xr_proc_wait. Returns XR_PROC_WAIT_RUNNING
// if the child is still alive, XR_PROC_WAIT_EXITED if it has exited,
// and XR_PROC_WAIT_ERROR if the wait query failed. When the child has
// exited cleanly, writes 0..255 to `*exit_code`; if the child was
// signaled / forcibly terminated, writes -1. A reported EXITED child
// has been reaped and must not be waited again.
XR_FUNC XrProcWaitResult xr_proc_try_wait(XrProcId pid, int *exit_code);

// Send `signal` to the child identified by `pid`. Returns 0 on success
// and -1 on failure. POSIX uses kill(2). Windows has no POSIX signal
// model, so the portable subset maps to TerminateProcess; xr_proc_wait
// reports such forced termination as -1, matching the POSIX signaled
// path.
XR_FUNC int xr_proc_kill(XrProcId pid, int signal);

// Terminate the process group rooted at pid when the child was spawned with
// new_process_group. Platforms without group support fall back to killing the
// direct child until their native job-object implementation is available.
XR_FUNC int xr_proc_kill_tree(XrProcId pid, int signal);

// Current process id. Always succeeds.
XR_FUNC int64_t xr_proc_self_pid(void);

// Absolute filesystem path of the running executable. Writes a
// NUL-terminated path into `buf` and returns 0 on success; returns -1
// when the platform query fails or `buf` is too small. Symlinks are
// resolved where the platform allows (macOS realpath, Linux
// /proc/self/exe). Callers use this to locate resources shipped
// alongside the binary (e.g. the stdlib directory).
XR_FUNC int xr_proc_self_exe_path(char *buf, size_t size);

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
