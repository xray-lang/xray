/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_effect_db.h - Canonical analyzer-owned error effect summaries
 */

#ifndef XA_EFFECT_DB_H
#define XA_EFFECT_DB_H

#include "../../base/xdefs.h"
#include <stdbool.h>
#include <stdint.h>

typedef uint32_t XaEffectId;
typedef uint32_t XaErrorTypeId;
typedef uint32_t XaErrorVariantId;
typedef uint32_t XaEffectEdgeId;
typedef struct XrType XrType;

#define XA_EFFECT_NONE ((XaEffectId) 0u)
#define XA_EFFECT_ID_NONE XA_EFFECT_NONE
#define XA_ERROR_TYPE_NONE ((XaErrorTypeId) 0u)
#define XA_ERROR_VARIANT_INVALID ((XaErrorVariantId) UINT32_MAX)

typedef struct XaBitSet {
    uint64_t *words;
    uint32_t word_count;
} XaBitSet;

typedef struct XaErrorTypeSet {
    XaErrorTypeId type_id;
    uint64_t stable_type_key;
    XaBitSet variants;
    bool all_variants;
} XaErrorTypeSet;

typedef struct XaErrorSet {
    XaErrorTypeSet *types;
    uint32_t count;
    uint32_t capacity;
} XaErrorSet;

typedef enum XaEffectCompleteness {
    XA_EFFECT_COMPLETE = 0,
    XA_EFFECT_INCOMPLETE = 1,
} XaEffectCompleteness;

typedef enum XaUnknownReason {
    XA_UNKNOWN_NONE = 0,
    XA_UNKNOWN_UNRESOLVED_CALLEE = 1u << 0,
    XA_UNKNOWN_MISSING_IMPORTED_EFFECT = 1u << 1,
    XA_UNKNOWN_OPEN_VIRTUAL_DISPATCH = 1u << 2,
    XA_UNKNOWN_DYNAMIC_CALL_TARGET = 1u << 3,
    XA_UNKNOWN_NATIVE_CONTRACT_MISSING = 1u << 4,
    XA_UNKNOWN_ANALYSIS_LIMIT = 1u << 5,
    XA_UNKNOWN_INVALID_PROGRAM = 1u << 6,
} XaUnknownReason;

typedef uint32_t XaUnknownReasonSet;

typedef struct XaEffectSummary {
    XaErrorSet escaping;
    XaEffectCompleteness completeness;
    XaUnknownReasonSet unknown_reasons;
    XaEffectEdgeId *roots;
    uint32_t root_count;
    uint32_t root_capacity;
    uint64_t fingerprint;
    uint32_t revision;
} XaEffectSummary;

typedef struct XaEffectDatabase XaEffectDatabase;

XR_FUNC XaEffectDatabase *xa_effect_db_new(void);
XR_FUNC void xa_effect_db_free(XaEffectDatabase *db);
XR_FUNC void xa_effect_db_clear(XaEffectDatabase *db);

XR_FUNC XaErrorTypeId xa_effect_db_register_error_type(XaEffectDatabase *db,
                                                       uint64_t stable_type_key,
                                                       XrType *type_handle);
XR_FUNC XaErrorTypeId xa_effect_db_register_error_enum(XaEffectDatabase *db, XrType *enum_type);
XR_FUNC XaErrorVariantId xa_effect_db_register_error_variant(XaEffectDatabase *db,
                                                             XaErrorTypeId type_id,
                                                             uint64_t stable_variant_key);
XR_FUNC uint64_t xa_effect_db_error_type_key(const XaEffectDatabase *db, XaErrorTypeId type_id);
XR_FUNC XrType *xa_effect_db_error_type_handle(const XaEffectDatabase *db, XaErrorTypeId type_id);
XR_FUNC uint64_t xa_effect_db_error_variant_key(const XaEffectDatabase *db, XaErrorTypeId type_id,
                                                XaErrorVariantId variant_id);

XR_FUNC void xa_effect_summary_init(XaEffectSummary *summary);
XR_FUNC void xa_effect_summary_clear(XaEffectSummary *summary);
XR_FUNC bool xa_effect_summary_add_variant(XaEffectDatabase *db, XaEffectSummary *summary,
                                           XaErrorTypeId type_id, XaErrorVariantId variant_id);
XR_FUNC bool xa_effect_summary_add_all_variants(XaEffectDatabase *db, XaEffectSummary *summary,
                                                XaErrorTypeId type_id);
XR_FUNC bool xa_effect_summary_add_summary(XaEffectDatabase *db, XaEffectSummary *summary,
                                           const XaEffectSummary *src);
XR_FUNC void xa_effect_summary_mark_incomplete(XaEffectSummary *summary, XaUnknownReason reason);
XR_FUNC bool xa_effect_summary_is_nothrow(const XaEffectSummary *summary);
XR_FUNC uint64_t xa_effect_summary_fingerprint(const XaEffectDatabase *db,
                                               const XaEffectSummary *summary);

XR_FUNC XaEffectId xa_effect_db_intern(XaEffectDatabase *db, const XaEffectSummary *summary);
XR_FUNC const XaEffectSummary *xa_effect_db_get(const XaEffectDatabase *db, XaEffectId id);
XR_FUNC uint32_t xa_effect_db_summary_count(const XaEffectDatabase *db);

XR_FUNC bool xa_bitset_test(const XaBitSet *set, uint32_t bit);
XR_FUNC uint32_t xa_bitset_word_count(const XaBitSet *set);

#endif /* XA_EFFECT_DB_H */
