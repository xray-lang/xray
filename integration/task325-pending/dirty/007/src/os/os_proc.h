/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_proc.h - Cross-platform process spawning and introspection.
 *
 * The sole process owner accepts an explicit allocation/work policy and either
 * a complete environment or an explicit inherited environment with overrides.
 * Compiler callers freeze inputs before spawning; execution callers choose
 * their runtime policy. Neither domain silently acquires the other's budget.
 *
 * Ordinary waits consume their child. Group waits retain the leader identity
 * until close terminates remaining descendants and reaps the leader. A caller
 * serializes group operations. Ordinary runtime IDs permit competing waiters;
 * at most one waiter consumes the child.
 */

#ifndef XR_OS_OS_PROC_H
#define XR_OS_OS_PROC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/xdefs.h"
#include "../base/xio_policy.h"
#include "os_pipe.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque process handle. Positive on success, -1 on error.
//
// On POSIX this is the pid_t value returned by fork(); on Windows
// the numeric PID maps to internally tracked process and job handles. Callers must not assume the int64_t is a Win32 HANDLE
// or a pid_t — go through xr_proc_wait / xr_proc_self_pid for
// cross-platform behaviour.
typedef int64_t XrProcId;

#define XR_PROC_INVALID ((XrProcId) - 1)

typedef enum XrProcWaitResult {
    XR_PROC_WAIT_ERROR = -1,
    XR_PROC_WAIT_RUNNING = 0,
    XR_PROC_WAIT_EXITED = 1,
} XrProcWaitResult;

typedef enum XrOsProcStatus {
    XR_PROC_OK, XR_PROC_INVALID_ARGUMENT, XR_PROC_UNRESOLVED, XR_PROC_BUDGET,
    XR_PROC_OUT_OF_MEMORY, XR_PROC_IO, XR_PROC_UNSUPPORTED
} XrOsProcStatus;

/* Callbacks are mandatory. The context is borrowed during spawn, and until
 * close when image observation is enabled.
 * Compiler callers pass their ledger; execution callers explicitly choose
 * their allocation policy. No allocation policy is selected implicitly. */
typedef struct XrProcMemory {
    void *context;
    XrOsProcStatus (*alloc)(void *context, size_t bytes, void **output);
    void (*free)(void *context, void *memory);
    XrOsProcStatus (*work)(void *context, uint64_t units);
} XrProcMemory;
XR_FUNC XrProcMemory xr_proc_system_memory(void);
XR_FUNC XrOsProcStatus xr_proc_last_error(void);

typedef enum XrProcImageMode {
    XR_PROC_IMAGES_NONE, XR_PROC_IMAGES_WINDOWS_TREE
} XrProcImageMode;
/* ROOT revokes remaining group execution after root EXIT is acknowledged.
 * Image events and job accounting still have to drain normally. */
typedef enum XrProcCompletionPolicy {
    XR_PROC_COMPLETE_TREE, XR_PROC_COMPLETE_ROOT
} XrProcCompletionPolicy;
typedef enum XrProcImageKind {
    XR_PROC_IMAGE_EXECUTABLE, XR_PROC_IMAGE_DLL
} XrProcImageKind;
typedef struct XrProcImageEvent {
    XrProcId pid;
    XrProcImageKind kind;
    /* A real Windows file HANDLE, borrowed only during observe. Duplicate it
     * inside the callback to retain it; never close this borrowed handle. */
    intptr_t file_handle;
} XrProcImageEvent;
typedef struct XrProcImageObserver {
    void *context;
    XrOsProcStatus (*observe)(void *context, const XrProcImageEvent *event);
} XrProcImageObserver;

typedef struct XrProcSpawnOptions {
    XrProcMemory memory;
    const char *cwd;
    const char *const *env_keys;
    const char *const *env_values;
    size_t env_count;
    /* False explicitly chooses the runtime's inherited environment plus
     * overrides. True supplies the complete environment, including empty. */
    bool complete_environment;
    bool has_stdin;
    XrPipeHandle stdin_read;
    bool has_stdout;
    XrPipeHandle stdout_write;
    bool has_stderr;
    XrPipeHandle stderr_write;
    bool detached;
    bool new_process_group;
    XrProcImageMode image_mode;
    XrProcImageObserver image_observer;
    XrProcCompletionPolicy completion_policy;
} XrProcSpawnOptions;

typedef struct XrProcImagePumpResult {
    bool drained;
    bool progressed;
} XrProcImagePumpResult;

/* Image observation requires a non-detached owned group. Its memory policy
 * and observer are copied, and their contexts remain borrowed until close.
 * Spawn/pump/wait/close use the creating thread. A thread cannot own two
 * active image roots; callbacks cannot recursively pump or close their root.
 * Each pump requests at most a 1 ms event wait and processes at most one event.
 * OS scheduling can exceed that request. Progressed means one
 * event was continued; idle polls can sleep without throttling queued events.
 * Failure preserves the entire output and is sticky until close. Drained means all observed debuggees
 * exited, not that these facts confer target authority. */
XR_FUNC XrOsProcStatus xr_proc_pump_images(XrProcId pid, XrProcImagePumpResult *output);

/* Failure preserves output. Successful waits consume ordinary child owners. Group IDs require close
 * after wait, including failed waits; close terminates remaining descendants.
 * Detached IDs are informational and must not be waited or closed. */
XR_FUNC XrOsProcStatus xr_proc_spawn(const char *prog, const char *const argv[],
    const XrProcSpawnOptions *options, XrProcId *output);
/* Only an owned new_process_group request can be closed. */
XR_FUNC int xr_proc_close(XrProcId pid);

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
// must not be waited again. Ordinary children are reaped; group owners retain
// the leader identity until close terminates descendants and reaps it.
XR_FUNC XrProcWaitResult xr_proc_try_wait(XrProcId pid, int *exit_code);

// Send `signal` to the child identified by `pid`. Returns 0 on success
// and -1 on failure. POSIX uses kill(2). Windows has no POSIX signal
// model, so the portable subset maps to TerminateProcess; xr_proc_wait
// reports such forced termination as -1, matching the POSIX signaled
// path.
XR_FUNC int xr_proc_kill(XrProcId pid, int signal);

// Terminate the owned process group or job. No direct-child fallback.
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

/* Owned UTF-8 process queries use the caller's mandatory I/O policy. Output
 * must be NULL and changes only on OK. Free through that same policy. Empty
 * environment values are owned empty strings; an absent key is NOT_FOUND.
 * The OS environment is observed during this call, not retained or frozen as
 * a whole. OS/libc internal storage is outside the caller allocation domain. */
XR_FUNC XrOsIoStatus xr_os_io_self_exe_path(const XrOsIoPolicy *policy, char **output);
XR_FUNC XrOsIoStatus xr_os_io_environment_get(const XrOsIoPolicy *policy,
    const char *name, char **output);

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
