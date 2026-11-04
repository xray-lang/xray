/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_process_owned.h - Stable process and job owners without PID authority
 */
#ifndef XR_OS_PROCESS_OWNED_H
#define XR_OS_PROCESS_OWNED_H
#include "../base/xio_policy.h"
#include <stdbool.h>

typedef struct XrOsProcessOwner { void *state; bool busy; } XrOsProcessOwner;
typedef struct XrOsProcessText { const char *text; size_t length; } XrOsProcessText;
typedef struct XrOsProcessQuery { void *context; bool (*current)(void *); } XrOsProcessQuery;
typedef struct XrOsProcessRequest {
    XrOsProcessText program;
    const XrOsProcessText *arguments;
    size_t argument_count;
    const XrOsProcessText *cwd;
    const XrOsProcessText *env_keys,*env_values;
    size_t environment_count;
    intptr_t streams[3];
    bool redirected[3],detached;
} XrOsProcessRequest;
typedef struct XrOsProcessProgress { bool exited; int64_t code; } XrOsProcessProgress;

/* Zero-initialize a stable owner. Copies have no authority. The policy context
 * outlives close; the request, query and all text are borrowed only by prepare.
 * Prepare creates a suspended child inside a kill-on-close job. Ordinary
 * platform refusal is OK/false; typed failures preserve output. Every failure
 * may retain an owner and must be followed by close. No PID is exposed.
 * Redirection handles are borrowed only until an immediate inheritable copy.
 * Neither this API nor a borrowed handle grants script or Root authority. */
XR_FUNC XrOsIoStatus xr_os_io_process_prepare(const XrOsIoPolicy *memory,
    const XrOsProcessQuery *query,const XrOsProcessRequest *request,
    XrOsProcessOwner *owner,bool *created);
/* Explicit system-shell transport, independent of ordinary argv encoding.
 * The request supplies environment/cwd/stdio only: program, arguments and
 * detached must be empty. Windows resolves its actual system cmd.exe and
 * passes the strict command span as the raw tail of /D /S /C. Request and
 * command are borrowed synchronously; all owner/start/close rules above apply.
 * Other platforms currently return UNSUPPORTED without publishing output. */
XR_FUNC XrOsIoStatus xr_os_io_process_prepare_shell(const XrOsIoPolicy *memory,
    const XrOsProcessQuery *query,const XrOsProcessRequest *request,
    const XrOsProcessText *command,XrOsProcessOwner *owner,bool *created);
/* C-only commit after result admission. Successful start is the last fallible
 * publication step. A detached child becomes independent only on success;
 * subsequent close releases handles without terminating its execution. */
XR_FUNC XrOsIoStatus xr_os_io_process_start(XrOsProcessOwner *owner);
/* Nonblocking leader query; descendants do not delay an observed leader exit.
 * The process/job remains owned until close, including after this query. */
XR_FUNC XrOsIoStatus xr_os_io_process_poll(XrOsProcessOwner *owner,XrOsProcessProgress *output);
XR_FUNC XrOsIoStatus xr_os_io_process_kill(XrOsProcessOwner *owner,int64_t signal,bool *sent);
/* Nonblocking terminate-tree, confirm actual job termination, then close each
 * native resource exactly once. Pending is OK/false; failure preserves the
 * exact remaining duties. Already-started detached children are never killed.
 * Closing requires no allocation, request query, or guest execution. */
XR_FUNC XrOsIoStatus xr_os_io_process_close(XrOsProcessOwner *owner,bool *closed);
#endif // XR_OS_PROCESS_OWNED_H
