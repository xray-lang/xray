/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_memory_effect_db.h - Canonical root-relative memory effect summaries
 */

#ifndef XA_MEMORY_EFFECT_DB_H
#define XA_MEMORY_EFFECT_DB_H

#include "xa_effect_db.h"
#include <stdbool.h>
#include <stdint.h>

typedef uint32_t XaMemoryEffectId;
typedef uint32_t XaMemoryPlacePathId;
typedef uint32_t XaMemoryRangeExprId;

#define XA_MEMORY_EFFECT_NONE ((XaMemoryEffectId) 0u)
#define XA_MEMORY_PLACE_PATH_NONE ((XaMemoryPlacePathId) 0u)
#define XA_MEMORY_RANGE_EXPR_NONE ((XaMemoryRangeExprId) 0u)
#define XA_MEMORY_PLACE_PATH_WILDCARD ((XaMemoryPlacePathId) 1u)
#define XA_MEMORY_RANGE_EXPR_MIXED ((XaMemoryRangeExprId) UINT32_MAX)

typedef enum XaMemoryRootKind {
    XA_MEMORY_ROOT_PARAM = 1,
    XA_MEMORY_ROOT_RECEIVER,
    XA_MEMORY_ROOT_RETURN,
    XA_MEMORY_ROOT_FOREIGN_HANDLE,
} XaMemoryRootKind;

typedef struct XaMemoryRootRef {
    XaMemoryRootKind kind;
    uint32_t index;
} XaMemoryRootRef;

typedef enum XaMemoryRelocation {
    XA_MEMORY_ADDRESS_STABLE = 0,
    XA_MEMORY_MAY_RELOCATE,
} XaMemoryRelocation;

typedef enum XaMemoryShortening {
    XA_MEMORY_NEVER_SHORTENS = 0,
    XA_MEMORY_MAY_SHORTEN,
} XaMemoryShortening;

typedef enum XaMemoryInvalidation {
    XA_MEMORY_NEVER_INVALIDATES = 0,
    XA_MEMORY_INVALIDATES_VIEWS,
} XaMemoryInvalidation;

typedef struct XaMemoryRootEffect {
    XaMemoryRootRef root;
    XaMemoryPlacePathId *writes;
    uint32_t write_count;
    uint32_t write_capacity;
    bool descriptor_rebind;
    XaMemoryRelocation relocation;
    XaMemoryShortening shortening;
    XaMemoryRangeExprId shortening_range;
    XaMemoryInvalidation invalidation;
} XaMemoryRootEffect;

typedef struct XaMemoryEffectSummary {
    XaMemoryRootEffect *roots;
    uint32_t root_count;
    uint32_t root_capacity;
    XaEffectCompleteness completeness;
    XaUnknownReasonSet unknown_reasons;
    uint64_t fingerprint;
    uint32_t revision;
} XaMemoryEffectSummary;

typedef struct XaMemoryEffectDatabase XaMemoryEffectDatabase;

XR_FUNC XaMemoryEffectDatabase *xa_memory_effect_db_new(void);
XR_FUNC void xa_memory_effect_db_free(XaMemoryEffectDatabase *db);
XR_FUNC void xa_memory_effect_db_clear(XaMemoryEffectDatabase *db);

XR_FUNC void xa_memory_effect_summary_init(XaMemoryEffectSummary *summary);
XR_FUNC void xa_memory_effect_summary_clear(XaMemoryEffectSummary *summary);
XR_FUNC bool xa_memory_effect_summary_add_write(XaMemoryEffectSummary *summary,
                                                XaMemoryRootRef root, XaMemoryPlacePathId path);
XR_FUNC bool xa_memory_effect_summary_mark_descriptor_rebind(XaMemoryEffectSummary *summary,
                                                             XaMemoryRootRef root);
XR_FUNC bool xa_memory_effect_summary_mark_relocation(XaMemoryEffectSummary *summary,
                                                      XaMemoryRootRef root);
XR_FUNC bool xa_memory_effect_summary_mark_shortening(XaMemoryEffectSummary *summary,
                                                      XaMemoryRootRef root,
                                                      XaMemoryRangeExprId range);
XR_FUNC bool xa_memory_effect_summary_mark_invalidation(XaMemoryEffectSummary *summary,
                                                        XaMemoryRootRef root);
XR_FUNC void xa_memory_effect_summary_mark_incomplete(XaMemoryEffectSummary *summary,
                                                      XaUnknownReason reason);
XR_FUNC bool xa_memory_effect_summary_add_summary(XaMemoryEffectSummary *summary,
                                                  const XaMemoryEffectSummary *source);
XR_FUNC bool xa_memory_effect_summary_is_complete(const XaMemoryEffectSummary *summary);
XR_FUNC bool xa_memory_effect_summary_has_resource_failure(const XaMemoryEffectSummary *summary);
XR_FUNC bool xa_memory_effect_summary_invalidates_live_view(const XaMemoryEffectSummary *summary,
                                                            XaMemoryRootRef root);
XR_FUNC uint64_t xa_memory_effect_summary_fingerprint(const XaMemoryEffectSummary *summary);

/* AnalysisResourceFailure summaries are deliberately not internable. */
XR_FUNC XaMemoryEffectId xa_memory_effect_db_intern(XaMemoryEffectDatabase *db,
                                                    const XaMemoryEffectSummary *summary);
XR_FUNC const XaMemoryEffectSummary *xa_memory_effect_db_get(const XaMemoryEffectDatabase *db,
                                                             XaMemoryEffectId id);
XR_FUNC uint32_t xa_memory_effect_db_summary_count(const XaMemoryEffectDatabase *db);

#endif /* XA_MEMORY_EFFECT_DB_H */
