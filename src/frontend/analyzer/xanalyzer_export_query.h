/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_export_query.h - Metered export queries with sticky failure
 *
 * KEY CONCEPT:
 *   Missing metadata and an exhausted query are distinct outcomes.
 */
#ifndef XANALYZER_EXPORT_QUERY_H
#define XANALYZER_EXPORT_QUERY_H
#include "xanalyzer.h"
#include "../../base/xhashmap.h"
#include "../../base/xfileio.h"
#include "../../base/xio_policy.inc.h"
#include "../../toolchain/xcompiler_session.h"

static inline bool xa_accept_export_io(XaAnalyzer *analyzer, XrOsIoStatus status) {
    if (!analyzer || !analyzer->compiler_session)
        return false;
    if (status == XR_OS_IO_OK)
        return true;
    XrCompileResourceStatus failure = status == XR_OS_IO_BUDGET ? XR_COMPILE_RESOURCE_BUDGET :
        status == XR_OS_IO_OUT_OF_MEMORY ? XR_COMPILE_RESOURCE_OUT_OF_MEMORY : XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    (void) xr_compile_state_fail(xr_compile_session_compile_state(analyzer->compiler_session), failure);
    return false;
}

/* Failure preserves the caller's output and never falls back to an unmetered
 * lookup. A missing map/key is an ordinary absence only on a healthy state. */
static inline bool xa_find_export_symbol(XaAnalyzer *analyzer, const XrHashMap *exports,
                                         const char *name, XaSymbol **output) {
    if (!analyzer || !output ||
        xr_compile_session_resource_status(analyzer->compiler_session) != XR_COMPILE_RESOURCE_OK)
        return false;
    if (!exports || !name) {
        *output = NULL;
        return true;
    }
    void *value = NULL;
    if (!xa_accept_export_io(analyzer, xr_hashmap_owned_get(exports, name, &value)))
        return false;
    *output = value;
    return true;
}

static inline bool xa_export_text_equal(XrIoContext *io, const char *left,
                                         const char *right, bool *output) {
    size_t left_length = 0, right_length = 0;
    if (!io_length(io, left, &left_length) || !io_length(io, right, &right_length))
        return false;
    bool equal = false;
    if (left_length == right_length) {
        if (!io_work(io, left_length))
            return false;
        equal = !memcmp(left, right, left_length);
    }
    *output = equal;
    return true;
}

/* Exact source identifiers are not filesystem locators. Failure never changes
 * output; optional missing metadata is an absence only on a healthy state. */
static inline bool xa_export_identity_equal(XaAnalyzer *analyzer, const char *left,
                                             const char *right, bool *output) {
    if (!analyzer || !output ||
        xr_compile_session_resource_status(analyzer->compiler_session) != XR_COMPILE_RESOURCE_OK)
        return false;
    if (!left || !right) {
        *output = false;
        return true;
    }
    XrOsIoPolicy policy = xr_compile_io_policy(xr_compile_session_resources(analyzer->compiler_session));
    XrIoContext io = {&policy, XR_OS_IO_OK};
    bool equal = false;
    if (!xa_export_text_equal(&io, left, right, &equal))
        return xa_accept_export_io(analyzer, io.status);
    *output = equal;
    return true;
}

/* Compare physical source locators, never a suffix or a namespace grant.
 * Platform normalization retains xr_realpath_owned's contract: Windows uses
 * lexical absolute paths; POSIX resolves existing paths. Missing POSIX paths
 * are unequal. Exact virtual locators need no filesystem probe.
 * Both temporary results use the original ledger and always release on error. */
static inline bool xa_export_source_path_equal(XaAnalyzer *analyzer, const char *left,
                                                const char *right, bool *output) {
    bool equal = false;
    if (!xa_export_identity_equal(analyzer, left, right, &equal))
        return false;
    if (equal || !left || !right) {
        *output = equal;
        return true;
    }
    XrOsIoPolicy policy = xr_compile_io_policy(xr_compile_session_resources(analyzer->compiler_session));
    XrIoContext io = {&policy, XR_OS_IO_OK};
    char *resolved_left = NULL, *resolved_right = NULL;
    io_status(&io, xr_realpath_owned(&policy, left, &resolved_left));
    if (io.status == XR_OS_IO_OK)
        io_status(&io, xr_realpath_owned(&policy, right, &resolved_right));
    if (io.status == XR_OS_IO_OK)
        (void) xa_export_text_equal(&io, resolved_left, resolved_right, &equal);
    xr_compile_resources_free(resolved_left);
    xr_compile_resources_free(resolved_right);
    if (io.status != XR_OS_IO_OK && io.status != XR_OS_IO_NOT_FOUND)
        return xa_accept_export_io(analyzer, io.status);
    *output = equal;
    return true;
}
#endif // XANALYZER_EXPORT_QUERY_H
