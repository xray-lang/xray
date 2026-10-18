/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * analyzer_registry_fixture.h - Real pool-backed registry construction
 */
#ifndef ANALYZER_REGISTRY_FIXTURE_H
#define ANALYZER_REGISTRY_FIXTURE_H
#include "runtime/value/xtype_pool.h"
static XrCompileResourceStatus open_registry_with_pool(XrCompileState *state,
                                                       XaSymbolRegistry **output) {
    XrTypePool *pool = NULL;
    XrCompileResourceStatus status = xr_compile_type_pool_open(state, &pool);
    if (status == XR_COMPILE_RESOURCE_OK)
        status = xa_symbol_registry_new(state, pool, output);
    xr_type_pool_free(pool);
    return status;
}
#endif // ANALYZER_REGISTRY_FIXTURE_H
