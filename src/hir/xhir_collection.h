/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_collection.h - Exact collection types and syntactic constant key facts
 */
#ifndef XHIR_COLLECTION_H
#define XHIR_COLLECTION_H
#include "../xir/xxir.h"
#include "../base/xcompile_request.h"
typedef enum XrHirCollectionKind { XR_HIR_COLLECTION_ARRAY,XR_HIR_COLLECTION_MAP } XrHirCollectionKind;
typedef struct XrHirCollectionEntry {
    XrXirType key,value;
    XrXirConstantKey constant;
    bool spread;
} XrHirCollectionEntry;
typedef struct XrHirCollectionInput {
    XrHirCollectionKind kind;
    XrXirType expected;
    bool has_expected;
    const XrHirCollectionEntry *entries;
    uint32_t count;
} XrHirCollectionInput;
typedef struct XrHirCollection {
    XrHirCollectionKind kind;
    XrXirType key,value;
    uint32_t count,operands;
} XrHirCollection;
typedef enum XrHirCollectionErrorKind {
    XR_HIR_COLLECTION_OK,XR_HIR_COLLECTION_EMPTY,XR_HIR_COLLECTION_KEY_TYPE,
    XR_HIR_COLLECTION_VALUE_TYPE,XR_HIR_COLLECTION_DUPLICATE,XR_HIR_COLLECTION_CONTEXT,
    XR_HIR_COLLECTION_SPREAD_SOURCE,XR_HIR_COLLECTION_NONVALUE
} XrHirCollectionErrorKind;
typedef struct XrHirCollectionError {
    XrHirCollectionErrorKind kind;
    XrXirType expected,actual;
    uint32_t first,second;
} XrHirCollectionError;
/* Explicit items and whole-collection spreads share one effective type
 * projection. Semantic failures use ordinal zero for caller remapping. */
XR_FUNC XrXirStatus xr_hir_collection_entry_types(const XrXirTypes *,XrHirCollectionKind,
    const XrHirCollectionEntry *,XrHirCollection *,XrHirCollectionError *);
/* The checked node owns scalar facts by value and retains no input pointer.
 * Resource/cancellation failure preserves both outputs; semantic failure
 * supplies ordinals for the bridge's actual source spans. */
XR_FUNC XrXirStatus xr_hir_collection_check(const XrXirCompileContext *,const XrXirTypes *,
    const XrHirCollectionInput *,XrCompileRequestControl *,XrHirCollection *,XrHirCollectionError *);
#endif
