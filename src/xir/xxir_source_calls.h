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
/* Display facts after the canonical direct-call inference and conversion pass.
 * Parameters include the real receiver prefix and have their display Cell layer
 * removed. Generic owners are the caller's actual declaration identities.
 * These observations neither construct nor authorize executable calls. */
typedef struct XrXirSourceCallSignature {
    const XrXirSourceType *parameters;
    uint32_t parameter_count,receiver_count;
    XrXirSourceType result;
    bool known;
    /* Exact caller-scope names, indexed by the canonical type parameter ordinal.
     * Closed signatures keep this vector and its owner absent. */
    const char *const *type_parameters;
    uint32_t type_parameter_count, type_parameter_owner;
} XrXirSourceCallSignature;
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
    XrXirSourceCallSignature signature;
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
