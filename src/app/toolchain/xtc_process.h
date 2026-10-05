/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_process.h - Bounded, argv-only process execution for toolchain probes
 */

#ifndef XTC_PROCESS_H
#define XTC_PROCESS_H

#include "../../base/xdefs.h"
#include "../../base/xcompile_resources.h"
#include "../../os/os_proc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XTC_PROCESS_MAX_ARGS 96
#define XTC_PROCESS_MAX_ENV 256
#define XTC_PROCESS_DEFAULT_OUTPUT_LIMIT (1024u * 1024u)

typedef enum XrProcessStatus {
    XTC_PROCESS_OK, XTC_PROCESS_INVALID, XTC_PROCESS_UNRESOLVED,
    XTC_PROCESS_BUDGET, XTC_PROCESS_OUT_OF_MEMORY, XTC_PROCESS_IO,
    XTC_PROCESS_TIMEOUT, XTC_PROCESS_CANCELLED, XTC_PROCESS_UNSUPPORTED
} XrProcessStatus;
typedef enum XrProcessEnvironmentSource {
    XTC_PROCESS_ENV_EXPLICIT, XTC_PROCESS_ENV_SNAPSHOT
} XrProcessEnvironmentSource;
typedef struct XrToolchainProcess XrToolchainProcess;
typedef bool (*XrProcessCancelled)(void *context);
/* One logical discovery/probe owns this context. A typed process failure stops
 * subsequent commands; successful child exit codes remain ordinary results.
 * Callers serialize this context; ledger synchronization does not protect it. */
typedef struct XrToolchainProcessContext {
    XrCompileResources *resources;
    XrProcessStatus status;
} XrToolchainProcessContext;

typedef struct XrProcessSpec {
    XrProcessEnvironmentSource environment_source;
    const char *executable;
    const char *argv[XTC_PROCESS_MAX_ARGS];
    const char *env_keys[XTC_PROCESS_MAX_ENV];
    const char *env_values[XTC_PROCESS_MAX_ENV];
    size_t env_count;
    const char *cwd;
    uint32_t timeout_ms;
    size_t output_limit;
    XrProcImageMode image_mode;
    XrProcCompletionPolicy completion_policy;
    /* Copied by prepare; context is borrowed only throughout each run and
     * its cleanup. Callback work and retained records use this process ledger.
     * Observations are provisional until run succeeds; the callback owns its
     * rollback. No image callback occurs during failure cleanup. */
    XrProcImageObserver image_observer;
} XrProcessSpec;

typedef struct XrProcessByteBuffer {
    uint8_t *data;
    size_t length;
    bool truncated;
} XrProcessByteBuffer;

typedef struct XrProcessResult {
    int exit_code;
    uint64_t duration_ms;
    XrProcessByteBuffer stdout_bytes;
    XrProcessByteBuffer stderr_bytes;
} XrProcessResult;

typedef struct XrProcessView {
    const char *executable, *cwd;
    const char *const *argv;
    size_t argc;
    const char *const *env_keys;
    const char *const *env_values;
    size_t env_count;
    uint32_t timeout_ms;
    size_t output_limit;
    XrProcImageMode image_mode;
    XrProcCompletionPolicy completion_policy;
} XrProcessView;

XR_FUNC void xtc_process_spec_init(XrProcessSpec *spec, const char *executable,
                                   uint32_t timeout_ms);
/* Prepare freezes all text and the explicitly selected environment source.
 * An explicit environment requires an explicit absolute cwd. Snapshot mode
 * captures cwd when omitted. The executable must always be absolute. */
XR_FUNC XrProcessStatus xtc_process_prepare(XrCompileResources *resources,
    const XrProcessSpec *spec, XrToolchainProcess **output);
/* Deep-copy an actual prepared owner on its original ledger, including system
 * environment keys and an executable distinct from argv[0]. Only observation
 * changes: WINDOWS_TREE uses the new callback/context, borrowed during run and
 * cleanup. No ambient environment or cwd is captured. Source may then be freed.
 * Non-Windows returns UNSUPPORTED. Output must be empty; failure preserves it.
 * Work covers text scans including NUL, copied bytes, copied policy fields and
 * the ledger's actual allocation/zeroing work. Cleanup consumes no new work. */
XR_FUNC XrProcessStatus xtc_process_clone_observed(const XrToolchainProcess *source,
    const XrProcImageObserver *observer, XrToolchainProcess **output);
/* Borrow the exact frozen inputs used by run until process_free. These queries
 * do not scan text, allocate, capture ambient state or consume work. The view
 * describes a complete environment, including captured system-reserved keys.
 * Invalid arguments preserve the entire output. No execution authority is
 * issued; recipients must copy borrowed inputs into their own resource owner. */
XR_FUNC XrCompileResources *xtc_process_resources(const XrToolchainProcess *process);
XR_FUNC XrProcessStatus xtc_process_view(const XrToolchainProcess *process,
    XrProcessView *output);
XR_FUNC XrProcessStatus xtc_process_run(const XrToolchainProcess *process,
    XrProcessCancelled cancelled, void *context, XrProcessResult *output);
XR_FUNC void xtc_process_free(XrToolchainProcess *process);
XR_FUNC const char *xtc_process_status_name(XrProcessStatus status);
XR_FUNC void xtc_process_redact_bytes(const uint8_t *input, size_t input_size, char *output,
                                      size_t output_size);
XR_FUNC bool xtc_process_copy_ascii_line(const XrProcessByteBuffer *bytes, char *output,
                                         size_t output_size);
XR_FUNC bool xtc_process_copy_ascii(const XrProcessByteBuffer *bytes, char *output,
                                    size_t output_size);
XR_FUNC bool xtc_process_copy_utf8_line(const XrProcessByteBuffer *bytes, char *output,
                                        size_t output_size);
XR_FUNC bool xtc_process_bytes_contains_ascii(const XrProcessByteBuffer *bytes,
                                              const char *needle);
XR_FUNC void xtc_process_result_free(XrProcessResult *result);

#endif /* XTC_PROCESS_H */
