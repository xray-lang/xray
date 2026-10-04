/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_type_match_internal.h - Operation-owned active matching stacks
 *
 * KEY CONCEPT:
 *   Scratch stores only capacity; every invocation recomputes its type proof.
 */
#ifndef XXIR_TYPE_MATCH_INTERNAL_H
#define XXIR_TYPE_MATCH_INTERNAL_H
#include "xxir_types.h"
struct TypeMatchMemory;
typedef struct XrXirTypeMatchScratch {
    XrCompileResources *resources;
    struct TypeMatchMemory *memory;
} XrXirTypeMatchScratch;
/* The operation borrows its live ledger and excludes concurrent users. Active
 * invocations claim distinct blocks, including nested destination matching. */
XR_FUNC XrXirStatus xr_xir_compile_type_substitution_matches_between_scratch(
    const XrXirCompileContext *context, const XrXirTypes *source_types,
    const XrXirTypes *types, const XrXirType *arguments, uint32_t count,
    XrXirType expected, XrXirType actual, XrXirTypeMatchScratch *scratch);
/* Only after all invocations have returned. No proof survives this operation. */
XR_FUNC void xr_xir_type_match_scratch_free(XrXirTypeMatchScratch *scratch);
#endif // XXIR_TYPE_MATCH_INTERNAL_H
