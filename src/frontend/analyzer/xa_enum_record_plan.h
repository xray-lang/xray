/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_enum_record_plan.h - Resolved enum variant and named payload access plans
 *
 * The parser preserves field source order. The analyzer resolves each field
 * exactly once to a declaration slot and publishes this immutable plan for
 * lowering. Unit patterns publish the same exact variant identity with an
 * empty field mapping. Backends must not repeat name lookup or infer enum
 * identity from syntax.
 */

#ifndef XA_ENUM_RECORD_PLAN_H
#define XA_ENUM_RECORD_PLAN_H

#include "../../base/xdefs.h"
#include <stdbool.h>
#include <stdint.h>

struct AstNode;

typedef enum XaEnumRecordPlanKind {
    XA_ENUM_RECORD_CONSTRUCT = 1,
    XA_ENUM_VARIANT_PATTERN = 2,
} XaEnumRecordPlanKind;

typedef struct XaEnumRecordPlan {
    XaEnumRecordPlanKind kind;
    uint32_t enum_symbol_id;
    uint32_t enum_layout_id;
    uint16_t variant_ordinal;
    uint16_t source_field_count;
    uint16_t declaration_field_count;
    const uint16_t *source_to_slot;
    bool complete;
} XaEnumRecordPlan;

typedef struct XaEnumRecordPlanTable XaEnumRecordPlanTable;

XR_FUNC XaEnumRecordPlanTable *xa_enum_record_plan_table_new(void);
XR_FUNC void xa_enum_record_plan_table_free(XaEnumRecordPlanTable *table);
XR_FUNC void xa_enum_record_plan_table_clear(XaEnumRecordPlanTable *table);
XR_FUNC void xa_enum_record_plan_table_begin_analysis(XaEnumRecordPlanTable *table);
XR_FUNC int xa_enum_record_plan_table_size(const XaEnumRecordPlanTable *table);

/* Copies source_to_slot into table-owned storage. A conflicting second plan
 * for the same AST node is rejected instead of silently changing semantics. */
XR_FUNC bool xa_enum_record_plan_table_set(XaEnumRecordPlanTable *table, struct AstNode *node,
                                           const XaEnumRecordPlan *plan);
XR_FUNC const XaEnumRecordPlan *
xa_enum_record_plan_table_get(const XaEnumRecordPlanTable *table, const struct AstNode *node);

#endif  // XA_ENUM_RECORD_PLAN_H
