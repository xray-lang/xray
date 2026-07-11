/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#ifndef XAOT_STORAGE_PLAN_H
#define XAOT_STORAGE_PLAN_H

#include "../base/xdefs.h"
#include "../analysis/xglobal_summary.h"
#include "../runtime/core/xr_exec_context.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct XaotBundle;
struct XiFunc;
struct XiModule;

typedef enum XaotMaterializationKind {
    XAOT_MATERIALIZE_INLINE = 0,
    XAOT_MATERIALIZE_EXEC_LOCAL,
    XAOT_MATERIALIZE_MODULE_READONLY,
    XAOT_MATERIALIZE_MODULE_RUNTIME,
    XAOT_MATERIALIZE_SHARED_SYSTEM,
    XAOT_MATERIALIZE_REJECT,
} XaotMaterializationKind;

enum {
    XAOT_STORAGE_DEEP_READONLY = 1u << 0,
    XAOT_STORAGE_SHARE_SAFE = 1u << 1,
    XAOT_STORAGE_CONTAINS_EXEC_LOCAL_REF = 1u << 2,
    XAOT_STORAGE_CONTAINS_BORROW = 1u << 3,
    XAOT_STORAGE_CONTAINS_FOREIGN_REF = 1u << 4,
    XAOT_STORAGE_REQUIRES_DROP = 1u << 5,
};

typedef struct XaotStoragePlan {
    uint32_t module_index;
    uint32_t slot;
    uint32_t flags;
    uint8_t owner;
    uint8_t mutability;
    uint8_t address_identity;
    uint8_t materialization_kind;
} XaotStoragePlan;

enum {
    XAOT_MODULE_INIT_EV_ENTRY_FUNC = 1u << 0,
    XAOT_MODULE_INIT_EV_STORAGE_OWNER = 1u << 1,
    XAOT_MODULE_INIT_EV_NONSUSPEND = 1u << 2,
};

typedef struct XaotModuleInitPlan {
    const struct XiFunc *func;
    XgFuncId body_func_id;
    uint32_t module_index;
    uint32_t evidence;
    uint8_t allocation_owner;
    bool may_suspend;
} XaotModuleInitPlan;

typedef enum XaotCaptureAction {
    XAOT_CAPTURE_INLINE_VALUE = 0,
    XAOT_CAPTURE_DEEP_COPY,
    XAOT_CAPTURE_MOVE,
    XAOT_CAPTURE_MODULE_READONLY,
    XAOT_CAPTURE_SHARED_REF,
    XAOT_CAPTURE_REJECT,
} XaotCaptureAction;

enum {
    XAOT_CAPTURE_EV_CLOSED_CAPTURE = 1u << 0,
    XAOT_CAPTURE_EV_STORAGE_OWNER = 1u << 1,
    XAOT_CAPTURE_EV_TYPE_SHAPE = 1u << 2,
    XAOT_CAPTURE_EV_MUTABILITY = 1u << 3,
};

typedef struct XaotCapturePlan {
    const struct XiFunc *func;
    uint16_t capture_index;
    uint8_t source_owner;
    uint8_t action;
    uint32_t evidence;
} XaotCapturePlan;

XR_FUNC bool xaot_storage_capture_plans_build(struct XaotBundle *bundle);
XR_FUNC bool xaot_storage_capture_plans_verify(const struct XaotBundle *bundle, char *errbuf,
                                               size_t errbuf_len);
XR_FUNC const XaotStoragePlan *xaot_storage_plan_find(const struct XaotBundle *bundle,
                                                      const struct XiModule *module, uint32_t slot);
XR_FUNC const char *xaot_materialization_kind_name(uint8_t value);
XR_FUNC const char *xaot_capture_action_name(uint8_t value);

#endif /* XAOT_STORAGE_PLAN_H */
