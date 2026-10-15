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
#endif // XANALYZER_EXPORT_QUERY_H
