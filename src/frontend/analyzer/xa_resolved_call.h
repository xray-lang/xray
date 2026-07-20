/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu
 * Licensed under the MIT License
 *
 * xa_resolved_call.h - Analyzer-owned canonical call identity sidecar
 */

#ifndef XA_RESOLVED_CALL_H
#define XA_RESOLVED_CALL_H

#include "xa_intrinsic_registry.h"
#include "../../base/xdefs.h"
#include <stdint.h>

struct AstNode;

typedef enum XaResolvedCallReason {
    XA_RESOLVED_CALL_REASON_RESOLVED = 0,
    XA_RESOLVED_CALL_REASON_UNRESOLVED_CALLEE,
    XA_RESOLVED_CALL_REASON_INCOMPLETE_SIGNATURE,
    XA_RESOLVED_CALL_REASON_RECOVERY_POISON,
} XaResolvedCallReason;

typedef struct XaResolvedCall {
    uint32_t source_node_id;
    uint32_t target_symbol_id;
    XaIntrinsicId intrinsic_id;
    XaResolvedCallReason reason;
} XaResolvedCall;

typedef struct XaResolvedCallTable XaResolvedCallTable;

XR_FUNC XaResolvedCallTable *xa_resolved_call_table_new(void);
XR_FUNC void xa_resolved_call_table_free(XaResolvedCallTable *table);
XR_FUNC void xa_resolved_call_table_set(XaResolvedCallTable *table, const struct AstNode *node,
                                        const XaResolvedCall *call);
XR_FUNC const XaResolvedCall *xa_resolved_call_table_get(const XaResolvedCallTable *table,
                                                         const struct AstNode *node);
XR_FUNC int xa_resolved_call_table_size(const XaResolvedCallTable *table);

#endif  // XA_RESOLVED_CALL_H
