/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate_runtime.c - Runtime-ABI VM construction for bytecode embedders
 *
 * Initializes core runtime ABI classes and scheduler substrate without pulling
 * in the compiler frontend, module loader table, or prelude import graph.
 */

#include "../base/xlog.h"
#include "../runtime/xisolate_internal.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/class/xclass.h"
#include "../runtime/class/xclass_system.h"
#include "../runtime/class/xenum.h"
#include "../runtime/mem/xobj_destroy_ops.h"
#include "../base/xconfig.h"
#include "../runtime/class/xreflect_registry.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xvalue.h"
#include "../coro/xscope_transfer.h"
#include "../base/xglobal_indices.h"
#include "../../include/xray_isolate.h"

static XrEnumType *runtime_register_prelude_enum(XrayIsolate *isolate, const char *name,
                                                 char **members, int member_count,
                                                 const int *payload_counts) {
    if (!isolate || !name || !members || member_count <= 0)
        return NULL;

    XrValue values[8];
    if (member_count > (int) (sizeof(values) / sizeof(values[0])))
        return NULL;
    for (int i = 0; i < member_count; i++)
        values[i] = XR_FROM_INT(i);

    XrEnumType *type = xr_enum_type_new(isolate, name, XR_TINT, members, values, member_count);
    if (!type || !payload_counts)
        return type;

    type->is_adt = true;
    type->payload_counts = (int *) xr_calloc((size_t) member_count, sizeof(int));
    int max_payload = 0;
    if (type->payload_counts) {
        for (int i = 0; i < member_count; i++) {
            type->payload_counts[i] = payload_counts[i];
            if (payload_counts[i] > max_payload)
                max_payload = payload_counts[i];
        }
    }
    type->max_payload = max_payload;
    if (type->enum_class && max_payload > 0) {
        type->enum_class->field_count = (uint16_t) (1 + max_payload);
        type->enum_class->own_field_count = (uint16_t) (1 + max_payload);
        type->enum_class->builtin_kind = XR_BK_ADT_ENUM;
    }
    return type;
}

static void isolate_register_runtime_prelude_enums(XrayIsolate *isolate) {
    if (!isolate)
        return;

    char *ordering_members[] = {"Relaxed", "Acquire", "Release", "AcquireRelease", "SeqCst"};
    char *recv_members[] = {"Value", "Empty", "Timeout", "Closed"};
    char *send_result_members[] = {"Sent", "Full", "Timeout", "Closed"};
    char *task_result_members[] = {"Success", "Failed", "Cancelled", "Timeout", "Pending"};
    char *task_status_members[] = {"Pending", "Running", "Success", "Failed", "Cancelled"};
    const int recv_payloads[] = {1, 0, 0, 0};
    const int task_result_payloads[] = {1, 1, 0, 0, 0};

    XrEnumType *ordering =
        runtime_register_prelude_enum(isolate, "Ordering", ordering_members, 5, NULL);
    XrEnumType *recv =
        runtime_register_prelude_enum(isolate, "Recv", recv_members, 4, recv_payloads);
    XrEnumType *send_result =
        runtime_register_prelude_enum(isolate, "SendResult", send_result_members, 4, NULL);
    XrEnumType *task_result = runtime_register_prelude_enum(
        isolate, "TaskResult", task_result_members, 5, task_result_payloads);
    XrEnumType *task_status =
        runtime_register_prelude_enum(isolate, "TaskStatus", task_status_members, 5, NULL);

    if (ordering)
        isolate->vm.builtins[XR_GLOBAL_VAR_ORDERING] = XR_FROM_PTR(ordering);
    if (recv)
        isolate->vm.builtins[XR_GLOBAL_VAR_RECV] = XR_FROM_PTR(recv);
    if (send_result)
        isolate->vm.builtins[XR_GLOBAL_VAR_SEND_RESULT] = XR_FROM_PTR(send_result);
    if (task_result)
        isolate->vm.builtins[XR_GLOBAL_VAR_TASK_RESULT] = XR_FROM_PTR(task_result);
    if (task_status)
        isolate->vm.builtins[XR_GLOBAL_VAR_TASK_STATUS] = XR_FROM_PTR(task_status);
}

static void isolate_register_vm_builtins(XrayIsolate *isolate) {
    if (!isolate || !isolate->core)
        return;
    if (isolate->core->reflectClass)
        isolate->vm.builtins[XR_GLOBAL_VAR_REFLECT] =
            xr_value_from_class(isolate->core->reflectClass);
    if (isolate->core->arrayClass)
        isolate->vm.builtins[XR_GLOBAL_VAR_ARRAY] = xr_value_from_class(isolate->core->arrayClass);
    if (isolate->core->setClass)
        isolate->vm.builtins[XR_GLOBAL_VAR_SET] = xr_value_from_class(isolate->core->setClass);
    if (isolate->core->mapClass)
        isolate->vm.builtins[XR_GLOBAL_VAR_MAP] = xr_value_from_class(isolate->core->mapClass);
    if (isolate->core->stringClass)
        isolate->vm.builtins[XR_GLOBAL_VAR_STRING] =
            xr_value_from_class(isolate->core->stringClass);
    if (isolate->core->jsonClass)
        isolate->vm.builtins[XR_GLOBAL_VAR_JSON] = xr_value_from_class(isolate->core->jsonClass);
    if (isolate->core_rt->native_type_classes[XR_TWORKQUEUE])
        isolate->vm.builtins[XR_GLOBAL_VAR_WORKQUEUE] =
            xr_value_from_class(isolate->core_rt->native_type_classes[XR_TWORKQUEUE]);
    if (isolate->core_rt->native_type_classes[XR_TRESULTGROUP])
        isolate->vm.builtins[XR_GLOBAL_VAR_RESULTGROUP] =
            xr_value_from_class(isolate->core_rt->native_type_classes[XR_TRESULTGROUP]);
    isolate_register_runtime_prelude_enums(isolate);
    if (isolate->vm.builtin_count < XR_USER_GLOBALS_START)
        isolate->vm.builtin_count = XR_USER_GLOBALS_START;
}

static int isolate_init_runtime(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "isolate_init_runtime: NULL isolate");

    xr_type_global_init();
    isolate->core_rt->symbol_table = xr_symbol_table_create();
    if (!isolate->core_rt->symbol_table)
        return -1;
    xr_symbol_table_init_builtins((XrSymbolTable *) isolate->core_rt->symbol_table);
    xr_registry_init(isolate);
    xr_runtime_core_enable_full_destroy_ops(isolate->core_rt);
    xr_core_init(isolate);
    xr_scope_transfer_enable_core(isolate->core_rt);

    isolate_register_vm_builtins(isolate);
    return 0;
}

static void isolate_cleanup_runtime(XrayIsolate *isolate) {
    if (isolate->core) {
        xr_core_free(isolate);
        isolate->core = NULL;
    }
    if (isolate->core_rt->type_registry) {
        xr_registry_free(isolate);
        isolate->core_rt->type_registry = NULL;
    }
    if (isolate->core_rt->symbol_table) {
        xr_symbol_table_destroy((XrSymbolTable *) isolate->core_rt->symbol_table);
        isolate->core_rt->symbol_table = NULL;
    }
}

XRAY_API XrayIsolate *xray_isolate_new_runtime(const XrayIsolateParams *params) {
    XrayIsolate *isolate = xray_isolate_new(params);
    if (!isolate)
        return NULL;

    if (isolate_init_runtime(isolate) != 0) {
        isolate_cleanup_runtime(isolate);
        xray_isolate_delete(isolate);
        return NULL;
    }
    isolate->lifecycle_cleanup = isolate_cleanup_runtime;
    return isolate;
}
