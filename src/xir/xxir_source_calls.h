/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_calls.h - Owned source call provenance without execution authority
 */
#ifndef XXIR_SOURCE_CALLS_H
#define XXIR_SOURCE_CALLS_H
#include "xxir_source_query.h"
/* A selected CALL reference retains its exact reference ordinal. An indirect
 * source invocation has UINT32_MAX instead; it grants no guessed target.
 * caller is the actual source declaration of the body, zero for implicit or
 * synthetic bodies. Only the real module initializer sets module_initializer.
 * A missing lexical selection has selection_known=false and a zero span in
 * its actual module; implicit operations never acquire a fabricated token. */
typedef struct XrXirSourceCall {
    uint32_t reference, caller;
    XrXirSourceRange range;
    bool module_initializer, selection_known;
} XrXirSourceCall;
typedef struct XrXirSourceFunctionRange {
    XrXirSourceRange range;
    bool known;
} XrXirSourceFunctionRange;
typedef struct XrXirSourceCalls {
    const XrXirSourceCall *items;
    uint32_t count;
    /* Indexed by SourceDeclaration.id - 1. Unknown bounds stay explicit. */
    const XrXirSourceFunctionRange *functions;
    uint32_t declaration_count;
} XrXirSourceCalls;
/* NULL means this snapshot did not receive canonical call observations.
 * Every scalar/range belongs to the snapshot after all Source producers die.
 * No construction, dispatch, ownership, or runtime-call authority is granted. */
XR_FUNC const XrXirSourceCalls *xr_xir_compile_source_snapshot_calls(const XrXirSourceSnapshot *);
#endif
