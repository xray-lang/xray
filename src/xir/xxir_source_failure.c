/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_failure.c - Independently owned Source failure display facts
 */
#include "xxir_source_query.h"
XR_FUNC bool xr_xir_compile_source_failure_empty(const XrXirSourceFailure *failure) {
    return !failure||(!failure->path&&failure->origin==XR_XIR_SOURCE_DIAGNOSTIC_DEFAULT&&
        !failure->has_utf16_position&&!failure->utf16_line&&!failure->utf16_column);
}
XR_FUNC void xr_xir_compile_source_failure_free(XrXirSourceFailure *failure) {
    if(!failure)return;
    xr_compile_resources_free(failure->path);*failure=(XrXirSourceFailure){0};
}
