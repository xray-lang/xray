/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_alloc_effect.h - Canonical analyzer-owned allocation effects
 */

#ifndef XA_ALLOC_EFFECT_H
#define XA_ALLOC_EFFECT_H

#include "../../base/xdefs.h"
#include <stdint.h>

typedef uint32_t XaAllocEffectId;

#define XA_ALLOC_EFFECT_NONE ((XaAllocEffectId) 0u)

typedef enum XaAllocState {
    XA_ALLOC_PROVEN_NONE = 0,
    XA_ALLOC_MAY = 1,
    XA_ALLOC_UNKNOWN = 2,
} XaAllocState;

typedef enum XaAllocReason {
    XA_ALLOC_REASON_NONE = 0,
    XA_ALLOC_REASON_HEAP_CONSTRUCT = 1u << 0,
    XA_ALLOC_REASON_CONTAINER = 1u << 1,
    XA_ALLOC_REASON_STRING = 1u << 2,
    XA_ALLOC_REASON_RUNTIME = 1u << 3,
    XA_ALLOC_REASON_CALLEE = 1u << 4,
    XA_ALLOC_REASON_CALLBACK = 1u << 5,
    XA_ALLOC_REASON_UNRESOLVED_CALLEE = 1u << 16,
    XA_ALLOC_REASON_DYNAMIC_CALL = 1u << 17,
    XA_ALLOC_REASON_OPEN_DISPATCH = 1u << 18,
    XA_ALLOC_REASON_NATIVE_CONTRACT_MISSING = 1u << 19,
    XA_ALLOC_REASON_ANALYSIS_LIMIT = 1u << 20,
    XA_ALLOC_REASON_INVALID_PROGRAM = 1u << 21,
} XaAllocReason;

typedef uint32_t XaAllocReasonSet;

typedef enum XaAllocationContractKind {
    XA_ALLOCATION_CONTRACT_MISSING = 0,
    XA_ALLOCATION_CONTRACT_NO_HEAP,
    XA_ALLOCATION_CONTRACT_MAY_HEAP,
} XaAllocationContractKind;

typedef struct XaAllocationSummary {
    XaAllocState state;
    XaAllocReasonSet reason_bits;
    uint32_t first_site_node_id;
    uint32_t first_callee_symbol_id;
    uint32_t line;
    uint32_t column;
    XaAllocEffectId callee_effect_id;
    const char *cause_kind;
    const char *cause_detail;
    const char *callee_name;
    uint64_t stable_fingerprint;
} XaAllocationSummary;

typedef struct XaAllocationDatabase XaAllocationDatabase;

XR_FUNC XaAllocationDatabase *xa_allocation_db_new(void);
XR_FUNC void xa_allocation_db_free(XaAllocationDatabase *db);
XR_FUNC void xa_allocation_db_clear(XaAllocationDatabase *db);
XR_FUNC XaAllocEffectId xa_allocation_db_intern(XaAllocationDatabase *db,
                                                const XaAllocationSummary *summary);
XR_FUNC const XaAllocationSummary *xa_allocation_db_get(const XaAllocationDatabase *db,
                                                        XaAllocEffectId id);
XR_FUNC uint32_t xa_allocation_db_summary_count(const XaAllocationDatabase *db);
XR_FUNC uint64_t xa_allocation_summary_fingerprint(const XaAllocationSummary *summary);

#endif /* XA_ALLOC_EFFECT_H */
