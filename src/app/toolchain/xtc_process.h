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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XTC_PROCESS_MAX_ARGS 96
#define XTC_PROCESS_MAX_ENV 32
#define XTC_PROCESS_DEFAULT_OUTPUT_LIMIT (1024u * 1024u)

typedef struct XrProcessSpec {
    const char *executable;
    const char *argv[XTC_PROCESS_MAX_ARGS];
    const char *env_keys[XTC_PROCESS_MAX_ENV];
    const char *env_values[XTC_PROCESS_MAX_ENV];
    size_t env_count;
    const char *cwd;
    uint32_t timeout_ms;
    size_t output_limit;
} XrProcessSpec;

typedef struct XrProcessResult {
    int exit_code;
    bool timed_out;
    bool output_truncated;
    uint64_t duration_ms;
    char *stdout_data;
    char *stderr_data;
} XrProcessResult;

XR_FUNC void xtc_process_spec_init(XrProcessSpec *spec, const char *executable,
                                   uint32_t timeout_ms);
XR_FUNC bool xtc_process_run(const XrProcessSpec *spec, XrProcessResult *out, char *err,
                             size_t err_size);
XR_FUNC void xtc_process_redact_output(const char *input, size_t input_size, char *output,
                                       size_t output_size);
XR_FUNC void xtc_process_result_free(XrProcessResult *result);

#endif /* XTC_PROCESS_H */
