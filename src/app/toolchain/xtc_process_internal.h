/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_process_internal.h - Request composition without a second execution loop
 */
#ifndef XTC_PROCESS_INTERNAL_H
#define XTC_PROCESS_INTERNAL_H
#include "xtc_process.h"
#include <stdio.h>
static inline XrProcessStatus xtc_process_request_run(XrToolchainProcessContext *context,
    const XrProcessSpec *spec, XrProcessResult *result, char *error, size_t error_size) {
    XrToolchainProcess *owner = NULL;
    XrProcessStatus status = !context || !context->resources ? XTC_PROCESS_INVALID : context->status;
    if (status == XTC_PROCESS_OK) status = xtc_process_prepare(context->resources, spec, &owner);
    if (status == XTC_PROCESS_OK) status = xtc_process_run(owner, NULL, NULL, result);
    xtc_process_free(owner);
    if (context) context->status = status;
    if (status != XTC_PROCESS_OK && error && error_size)
        snprintf(error, error_size, "%s", xtc_process_status_name(status));
    return status;
}
#endif // XTC_PROCESS_INTERNAL_H
