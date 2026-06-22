/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_coro.c - AOT coroutine runtime bridge
 */

#include "xaot_coro.h"
#include "xaot_runtime_internal.h"
#include "xaot_await.h"
#include "xaot_task.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../base/xchecks.h"
#include "../base/xconstants.h"
#include "../base/xglobal_indices.h"
#include "../base/xmalloc.h"
#include "../runtime/class/xclass_system.h"
#include "../runtime/class/xenum.h"
#include "../runtime/class/xinstance.h"
#include "../runtime/gc/xgc.h"
#include "../runtime/gc/xcoro_gc.h"
#include "../runtime/gc/xsystem_heap.h"
#include "../runtime/object/xarray.h"
#include "../runtime/object/xexception.h"
#include "../runtime/object/xmap.h"
#include "../runtime/object/xstring.h"
#include "../runtime/object/xtuple.h"
#include "../runtime/value/xchunk.h"
#include "../os/os_time.h"
#include "xblock.h"
#include "xchannel_ops.h"
#include "xcoroutine.h"
#include "xcoro_registry.h"
#include "xcoro_pool.h"
#include "xcoro_snapshot.h"
#include "xtask.h"
#include "xworker.h"
#include "xwork_queue.h"
#include "xresult_group.h"

enum {
    XR_AOT_VALUE_TAG_ARRAY = 15,
    XR_AOT_VALUE_TAG_ENUM = 24,
};

static XrEnumType *aot_runtime_register_prelude_enum(XrAotRuntime *runtime, int builtin_index,
                                                     const char *name, char **members,
                                                     int member_count, const int *payload_counts) {
    XrRuntimeCore *core = xr_aot_runtime_core(runtime);
    if (!core)
        return NULL;

    XrValue values[8];
    if (member_count <= 0 || member_count > (int) (sizeof(values) / sizeof(values[0])))
        return NULL;
    for (int i = 0; i < member_count; i++)
        values[i] = XR_FROM_INT(i);

    XrEnumType *type = xr_enum_type_new_core(core, name, XR_TINT, members, values, member_count);
    if (!type)
        return NULL;
    if (payload_counts && !xr_enum_type_set_adt_payloads(type, payload_counts, member_count))
        return NULL;
    xr_aot_runtime_set_builtin(runtime, builtin_index, XR_FROM_PTR(type));
    return type;
}

static bool aot_runtime_register_prelude_enums(XrAotRuntime *runtime) {
    char *ordering_members[] = {"Relaxed", "Acquire", "Release", "AcquireRelease", "SeqCst"};
    char *recv_members[] = {"Value", "Empty", "Timeout", "Closed"};
    char *send_result_members[] = {"Sent", "Full", "Timeout", "Closed"};
    char *task_result_members[] = {"Success", "Failed", "Cancelled", "Timeout", "Pending"};
    char *task_status_members[] = {"Pending", "Running", "Success", "Failed", "Cancelled"};
    const int recv_payloads[] = {1, 0, 0, 0};
    const int task_result_payloads[] = {1, 1, 0, 0, 0};

    return aot_runtime_register_prelude_enum(runtime, XR_GLOBAL_VAR_ORDERING, "Ordering",
                                             ordering_members, 5, NULL) &&
           aot_runtime_register_prelude_enum(runtime, XR_GLOBAL_VAR_RECV, "Recv", recv_members, 4,
                                             recv_payloads) &&
           aot_runtime_register_prelude_enum(runtime, XR_GLOBAL_VAR_SEND_RESULT, "SendResult",
                                             send_result_members, 4, NULL) &&
           aot_runtime_register_prelude_enum(runtime, XR_GLOBAL_VAR_TASK_RESULT, "TaskResult",
                                             task_result_members, 5, task_result_payloads) &&
           aot_runtime_register_prelude_enum(runtime, XR_GLOBAL_VAR_TASK_STATUS, "TaskStatus",
                                             task_status_members, 5, NULL);
}

static const char *aot_script_dir_bounds(const char *file, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!file || !file[0])
        return NULL;
    const char *last_slash = strrchr(file, '/');
    if (!last_slash)
        return NULL;
    size_t len = (size_t) (last_slash - file);
    if (len == 0)
        len = 1;
    if (out_len)
        *out_len = len;
    return file;
}

static bool aot_value_is_runtime_instance(XrValue value);

static bool aot_builtin_index_is_prelude_enum(int32_t index) {
    return index == XR_GLOBAL_VAR_ORDERING || index == XR_GLOBAL_VAR_RECV ||
           index == XR_GLOBAL_VAR_SEND_RESULT || index == XR_GLOBAL_VAR_TASK_RESULT ||
           index == XR_GLOBAL_VAR_TASK_STATUS;
}

static XrValue aot_runtime_script_builtin_lazy(XrAotRuntime *runtime, int32_t index) {
    XrRuntimeCore *core = xr_aot_runtime_core(runtime);
    if (!core)
        return XR_NULL_VAL;

    const char *text = NULL;
    size_t text_len = 0;
    if (index == XR_GLOBAL_VAR_FILE) {
        text = core->script_info.file;
        text_len = text ? strlen(text) : 0;
    } else if (index == XR_GLOBAL_VAR_DIR) {
        text = aot_script_dir_bounds(core->script_info.file, &text_len);
    } else {
        return XR_NULL_VAL;
    }
    if (!text || text_len == 0)
        return XR_NULL_VAL;

    XrString *s = xr_string_intern_core(core, text, text_len, 0);
    XrValue value = s ? xr_string_value(s) : XR_NULL_VAL;
    xr_aot_runtime_set_builtin(runtime, index, value);
    return value;
}

static XrValue aot_runtime_builtin_lazy(XrAotRuntime *runtime, int32_t index) {
    XrValue value = xr_aot_runtime_builtin(runtime, index);
    if (!XR_IS_NULL(value))
        return value;
    if (index == XR_GLOBAL_VAR_FILE || index == XR_GLOBAL_VAR_DIR)
        return aot_runtime_script_builtin_lazy(runtime, index);
    if (!aot_builtin_index_is_prelude_enum(index))
        return value;
    if (!aot_runtime_register_prelude_enums(runtime))
        return XR_NULL_VAL;
    return xr_aot_runtime_builtin(runtime, index);
}

static XrValue aot_runtime_process_field(const XrAotContext *ctx, const char *field) {
    if (!ctx || !ctx->runtime || !field)
        return XR_NULL_VAL;
    XrRuntimeCore *core = xr_aot_runtime_core(ctx->runtime);
    if (!core)
        return XR_NULL_VAL;

    XrScriptInfo *info = &core->script_info;
    if (strcmp(field, "args") == 0) {
        int argc = info->argc > 0 ? info->argc : 0;
        XrArray *args = xr_array_new_shared_core(core, argc);
        if (!args)
            return XR_NULL_VAL;
        for (int i = 0; i < argc; i++) {
            const char *arg = info->argv && info->argv[i] ? info->argv[i] : "";
            XrString *s = xr_string_intern_core(core, arg, strlen(arg), 0);
            xr_array_push(args, s ? xr_string_value(s) : XR_NULL_VAL);
        }
        return XR_FROM_PTR(args);
    }

    const char *text = NULL;
    size_t text_len = 0;
    if (strcmp(field, "file") == 0) {
        text = info->file;
        text_len = text ? strlen(text) : 0;
    } else if (strcmp(field, "dir") == 0) {
        text = aot_script_dir_bounds(info->file, &text_len);
    } else {
        return XR_NULL_VAL;
    }
    if (!text)
        return XR_NULL_VAL;
    XrString *s = xr_string_intern_core(core, text, text_len, 0);
    return s ? xr_string_value(s) : XR_NULL_VAL;
}

void xr_aot_trace_frame_value(void *visitor, XrValue value) {
    if (!visitor || !XR_IS_PTR(value))
        return;
    XrAotRootVisitor *root_visitor = (XrAotRootVisitor *) visitor;
    if (root_visitor->visit)
        root_visitor->visit(value, root_visitor->ctx);
}

void xr_aot_release_frame_value(XrCoroGC *gc, XrValue value) {
    if (!gc || !XR_IS_PTR(value))
        return;
    xr_rc_release_value(gc, value);
}

XrValue xr_aot_get_builtin(const XrAotContext *ctx, int32_t index) {
    if (!ctx || index < 0)
        return XR_NULL_VAL;
    if (ctx->runtime && index < XR_USER_GLOBALS_START)
        return aot_runtime_builtin_lazy(ctx->runtime, index);
    if (!ctx->vm_host || !ctx->vm_host_ops || !ctx->vm_host_ops->get_builtin ||
        index >= XR_GLOBALS_MAX) {
        return XR_NULL_VAL;
    }
    return ctx->vm_host_ops->get_builtin(ctx->vm_host, index);
}

XrValue xr_aot_load_builtin_field(const XrAotContext *ctx, int32_t index, const char *field) {
    if (!field)
        return XR_NULL_VAL;

    XrValue builtin = xr_aot_get_builtin(ctx, index);
    if (XR_IS_STRING(builtin)) {
        if (strcmp(field, "length") == 0 || strcmp(field, "size") == 0) {
            XrString *str = XR_TO_STRING(builtin);
            return XR_FROM_INT(str ? (int64_t) str->length : 0);
        }
        return XR_NULL_VAL;
    }

    if (aot_value_is_runtime_instance(builtin)) {
        XrObjHeader *gc = (XrObjHeader *) builtin.ptr;
        if (XR_OBJ_GET_TYPE(gc) == XR_TINSTANCE) {
            XrInstance *inst = xr_value_to_instance(builtin);
            XrEnumType *enum_type = (XrEnumType *) inst;
            bool is_enum_type =
                inst && ((inst->klass && inst->klass->builtin_kind == XR_BK_ENUM_TYPE) ||
                         (aot_builtin_index_is_prelude_enum(index) && enum_type->members &&
                          enum_type->member_count > 0));
            if (is_enum_type) {
                for (uint32_t i = 0; i < enum_type->member_count; i++) {
                    if (enum_type->members[i].name &&
                        strcmp(enum_type->members[i].name, field) == 0 &&
                        enum_type->members[i].instance) {
                        XrValue value = XR_FROM_PTR(enum_type->members[i].instance);
                        int payload_count = 0;
                        if (enum_type->is_adt && enum_type->payload_counts)
                            payload_count = enum_type->payload_counts[i];
                        if (!enum_type->is_adt || payload_count == 0) {
                            value.tag = XR_AOT_VALUE_TAG_ENUM;
                            value.ext = i;
                        }
                        return value;
                    }
                }
                return XR_NULL_VAL;
            }
        }
    }

    if (index != XR_GLOBAL_VAR_PROCESS)
        return XR_NULL_VAL;

    XrValue runtime_field = aot_runtime_process_field(ctx, field);
    if (!XR_IS_NULL(runtime_field))
        return runtime_field;

    XrValue process = builtin;
    if (!XR_IS_INSTANCE(process))
        return XR_NULL_VAL;

    int field_index = -1;
    if (strcmp(field, "file") == 0) {
        field_index = PROCESS_FIELD_FILE;
    } else if (strcmp(field, "args") == 0) {
        field_index = PROCESS_FIELD_ARGS;
    } else if (strcmp(field, "dir") == 0) {
        field_index = PROCESS_FIELD_DIR;
    }
    if (field_index < 0)
        return XR_NULL_VAL;
    return xr_instance_get_field_by_index((XrInstance *) process.ptr, field_index);
}

static bool aot_value_is_runtime_instance(XrValue value) {
    if (!XR_IS_PTR(value) || !value.ptr)
        return false;
    XrObjHeader *gc = (XrObjHeader *) value.ptr;
    return XR_OBJ_GET_TYPE(gc) == XR_TINSTANCE;
}

bool xr_aot_runtime_enum_value_info(XrValue value, const char **enum_name, const char **member_name,
                                    uint32_t *member_index, bool *is_adt, int *payload_count) {
    if (!aot_value_is_runtime_instance(value))
        return false;
    XrInstance *inst = xr_value_to_instance(value);
    if (!inst || (inst->klass && inst->klass->builtin_kind != XR_BK_ENUM_VALUE))
        return false;

    XrEnumValue *enum_value = (XrEnumValue *) inst;
    XrEnumType *parent = enum_value->parent_type;
    if (!parent || enum_value->member_index >= parent->member_count ||
        parent->members[enum_value->member_index].instance != enum_value) {
        return false;
    }
    if (enum_name)
        *enum_name = enum_value->enum_name ? enum_value->enum_name : "";
    if (member_name)
        *member_name = enum_value->member_name ? enum_value->member_name : "";
    if (member_index)
        *member_index = enum_value->member_index;
    if (is_adt)
        *is_adt = parent && parent->is_adt;
    if (payload_count) {
        int count = 0;
        if (parent && parent->is_adt && parent->payload_counts &&
            enum_value->member_index < parent->member_count) {
            count = parent->payload_counts[enum_value->member_index];
        }
        *payload_count = count;
    }
    return true;
}

bool xr_aot_runtime_adt_value_info(XrValue value, const char **enum_name, const char **member_name,
                                   uint32_t *member_index, int *payload_count) {
    if (!aot_value_is_runtime_instance(value))
        return false;
    XrInstance *inst = xr_value_to_instance(value);
    if (!inst || !inst->klass || inst->klass->builtin_kind != XR_BK_ADT_ENUM)
        return false;

    bool is_adt = false;
    return xr_aot_runtime_enum_value_info(inst->fields[0], enum_name, member_name, member_index,
                                          &is_adt, payload_count) &&
           is_adt;
}

XrValue xr_aot_runtime_adt_payload(XrValue value, int index) {
    if (index < 0 || !aot_value_is_runtime_instance(value))
        return XR_NULL_VAL;
    XrInstance *inst = xr_value_to_instance(value);
    if (!inst || !inst->klass || inst->klass->builtin_kind != XR_BK_ADT_ENUM)
        return XR_NULL_VAL;

    int payload_count = 0;
    if (!xr_aot_runtime_adt_value_info(value, NULL, NULL, NULL, &payload_count) ||
        index >= payload_count) {
        return XR_NULL_VAL;
    }
    return inst->fields[1 + index];
}

XrValue xr_aot_time_now(void) {
    return XR_FROM_INT((int64_t) (xr_time_realtime_ns() / 1000000ULL));
}

XrValue xr_aot_time_monotonic(void) {
    return XR_FROM_INT(xr_runtime_current_monotonic_ms());
}

XrValue xr_aot_time_nanos(void) {
    return XR_FROM_INT(xr_runtime_current_monotonic_ns());
}

XrValue xr_aot_time_micros(void) {
    return XR_FROM_INT(xr_runtime_current_monotonic_ns() / 1000LL);
}

XrValue xr_aot_time_clock(void) {
    return XR_FROM_INT((int64_t) (xr_time_process_cpu_ns() / 1000000ULL));
}

static void aot_detach_completed_executor(XrTask *task) {
    if (!task || !xr_task_executor_peek(task))
        return;
    /* Exchange-claim mirrors the VM await path: only the winner detaches. */
    XrCoroutine *exec = xr_task_claim_executor(task);
    if (!exec)
        return;
    if (!xr_coro_flags_has(exec, XR_CORO_FLG_DONE)) {
        /* Executor not done: this claim was premature, restore it. Callers
         * only reach here for terminal tasks, so this is defensive. */
        atomic_store_explicit(&task->coro, exec, memory_order_release);
        return;
    }
    exec->task = NULL;
}

static void aot_task_recycle_cancelled_executor(XrTask *task, XrCoroutine *coro,
                                                uint32_t before_cancel_flags) {
    if (!task || !coro || xr_coro_flags_has(coro, XR_CORO_FLG_MAIN))
        return;
    if ((task->flags & XR_TASK_FLG_RUNTIME_OWNED) == 0)
        return;
    if (xr_task_claim_executor(task) != coro)
        return;

    coro->task = NULL;
    coro->gc_flags |= XR_CORO_GC_RECYCLABLE | XR_CORO_GC_TRIM_BACKEND_STORAGE;

    if ((before_cancel_flags & XR_CORO_FLG_BLOCKED) == 0)
        return;
    if (coro->ext && (coro->ext->wait_bucket || xr_coro_select_wait(coro)))
        return;

    XrWorker *worker = xr_current_worker();
    XrRuntime *runtime = worker ? worker->p.runtime : (XrRuntime *) xr_coro_scheduler(coro);
    int owner_id = atomic_load_explicit(&coro->affinity_p, memory_order_acquire);
    if (worker && runtime == worker->p.runtime && worker->p.id == owner_id) {
        xr_worker_unblock(worker, coro);
        xr_coro_recycle_local(worker, coro);
        return;
    }
    if (runtime && owner_id >= 0 && owner_id < runtime->worker_count)
        xr_worker_inbox_enqueue(runtime, owner_id, coro);
}

static XrAotResult aot_store_slot_result(XrSlotRef out_slot, XrValue value) {
    if (!xr_slot_store_value(out_slot, value))
        return xr_aot_error(XR_NULL_VAL, false);
    return xr_aot_done(value);
}

static void *aot_slot_native_addr(XrSlotRef slot) {
    if (!slot.base)
        return NULL;
    if (slot.kind == XR_SLOT_NATIVE_PTR)
        return slot.base;
    if (slot.kind == XR_SLOT_AOT_FRAME_OFFSET)
        return (uint8_t *) slot.base + slot.offset;
    return NULL;
}

static bool aot_slot_store_i64_fast(XrSlotRef slot, int64_t value) {
    switch (slot.kind) {
        case XR_SLOT_NONE:
            return true;
        case XR_SLOT_XVALUE_PTR:
            if (!slot.base)
                return false;
            *(XrValue *) slot.base = XR_FROM_INT(value);
            return true;
        case XR_SLOT_NATIVE_PTR:
        case XR_SLOT_AOT_FRAME_OFFSET: {
            void *addr = aot_slot_native_addr(slot);
            if (!addr)
                return false;
            if (slot.type_id == XR_REP_I64) {
                *(int64_t *) addr = value;
                return true;
            }
            if (slot.type_id == XR_REP_TAGGED) {
                *(XrValue *) addr = XR_FROM_INT(value);
                return true;
            }
            break;
        }
    }
    return xr_slot_store_value(slot, XR_FROM_INT(value));
}

static bool aot_slot_store_f64_fast(XrSlotRef slot, double value) {
    switch (slot.kind) {
        case XR_SLOT_NONE:
            return true;
        case XR_SLOT_XVALUE_PTR:
            if (!slot.base)
                return false;
            *(XrValue *) slot.base = XR_FROM_FLOAT(value);
            return true;
        case XR_SLOT_NATIVE_PTR:
        case XR_SLOT_AOT_FRAME_OFFSET: {
            void *addr = aot_slot_native_addr(slot);
            if (!addr)
                return false;
            if (slot.type_id == XR_REP_F64) {
                *(double *) addr = value;
                return true;
            }
            if (slot.type_id == XR_REP_TAGGED) {
                *(XrValue *) addr = XR_FROM_FLOAT(value);
                return true;
            }
            break;
        }
    }
    return xr_slot_store_value(slot, XR_FROM_FLOAT(value));
}

static bool aot_slot_store_bool_fast(XrSlotRef slot, bool value) {
    switch (slot.kind) {
        case XR_SLOT_NONE:
            return true;
        case XR_SLOT_XVALUE_PTR:
            if (!slot.base)
                return false;
            *(XrValue *) slot.base = XR_FROM_BOOL(value);
            return true;
        case XR_SLOT_NATIVE_PTR:
        case XR_SLOT_AOT_FRAME_OFFSET: {
            void *addr = aot_slot_native_addr(slot);
            if (!addr)
                return false;
            if (slot.type_id == XR_REP_I64) {
                *(int64_t *) addr = value ? 1 : 0;
                return true;
            }
            if (slot.type_id == XR_REP_TAGGED) {
                *(XrValue *) addr = XR_FROM_BOOL(value);
                return true;
            }
            break;
        }
    }
    return xr_slot_store_value(slot, XR_FROM_BOOL(value));
}

static bool aot_slot_store_scalar_null_fast(XrSlotRef slot) {
    switch (slot.kind) {
        case XR_SLOT_NONE:
            return true;
        case XR_SLOT_XVALUE_PTR:
            if (!slot.base)
                return false;
            *(XrValue *) slot.base = XR_NULL_VAL;
            return true;
        case XR_SLOT_NATIVE_PTR:
        case XR_SLOT_AOT_FRAME_OFFSET: {
            void *addr = aot_slot_native_addr(slot);
            if (!addr)
                return false;
            if (slot.type_id == XR_REP_I64) {
                *(int64_t *) addr = 0;
                return true;
            }
            if (slot.type_id == XR_REP_F64) {
                *(double *) addr = 0.0;
                return true;
            }
            if (slot.type_id == XR_REP_TAGGED) {
                *(XrValue *) addr = XR_NULL_VAL;
                return true;
            }
            break;
        }
    }
    return xr_slot_store_value(slot, XR_NULL_VAL);
}

typedef struct XrAotVmHost {
    void *host;
    const XrAotVmHostOps *ops;
} XrAotVmHost;

static XrAotVmHost aot_context_vm_host(const XrAotContext *ctx, const XrTask *task) {
    (void) task;
    XrAotVmHost vm_host = {0};
    if (ctx && ctx->vm_host && ctx->vm_host_ops) {
        vm_host.host = ctx->vm_host;
        vm_host.ops = ctx->vm_host_ops;
    }
    return vm_host;
}

static bool aot_vm_host_available(XrAotVmHost vm_host) {
    return vm_host.host && vm_host.ops;
}

static XrCoroutine *aot_context_coro(const XrAotContext *ctx, XrAotVmHost vm_host,
                                     const XrTask *task) {
    if (ctx && ctx->coro)
        return ctx->coro;
    XrCoroutine *coro = aot_vm_host_available(vm_host) && vm_host.ops->current_coro
                            ? vm_host.ops->current_coro(vm_host.host)
                            : NULL;
    if (coro)
        return coro;
    return task ? xr_task_executor_peek(task) : NULL;
}

#define XR_AOT_CORO_COLLECT_MAX 10000

static XrValue aot_vm_host_string_value(XrAotVmHost vm_host, const char *data, size_t len) {
    if (!aot_vm_host_available(vm_host) || !vm_host.ops->intern_string_value || !data)
        return XR_NULL_VAL;
    return vm_host.ops->intern_string_value(vm_host.host, data, len);
}

static XrValue aot_coro_intern_key(XrAotVmHost vm_host, const char *key) {
    return key ? aot_vm_host_string_value(vm_host, key, strlen(key)) : XR_NULL_VAL;
}

static XrMap *aot_coro_new_map(XrAotVmHost vm_host, XrCoroutine *coro) {
    if (!coro)
        coro = aot_context_coro(NULL, vm_host, NULL);
    if (!aot_vm_host_available(vm_host) || !vm_host.ops->new_map)
        return NULL;
    return vm_host.ops->new_map(vm_host.host, coro);
}

static XrArray *aot_coro_new_array(XrAotVmHost vm_host, XrCoroutine *coro) {
    if (!coro)
        coro = aot_context_coro(NULL, vm_host, NULL);
    if (!aot_vm_host_available(vm_host) || !vm_host.ops->new_array)
        return NULL;
    return vm_host.ops->new_array(vm_host.host, coro);
}

static XrValue aot_coro_empty_array_value(XrAotVmHost vm_host, XrCoroutine *coro) {
    XrArray *arr = aot_coro_new_array(vm_host, coro);
    return arr ? xr_value_from_array(arr) : XR_NULL_VAL;
}

static XrValue aot_coro_empty_map_value(XrAotVmHost vm_host, XrCoroutine *coro) {
    XrMap *map = aot_coro_new_map(vm_host, coro);
    return map ? xr_value_from_map(map) : XR_NULL_VAL;
}

static XrValue aot_coro_name_value(XrAotVmHost vm_host, const XrCoroutine *coro) {
    const char *name = xr_coro_name(coro);
    if (!name)
        return XR_NULL_VAL;
    return aot_vm_host_string_value(vm_host, name, strlen(name));
}

static void aot_coro_set_source_field(XrAotVmHost vm_host, XrMap *info, const XrCoroutine *coro) {
    const char *source_file = xr_coro_source_file(coro);
    if (!aot_vm_host_available(vm_host) || !info || !source_file)
        return;
    char source_buf[256];
    snprintf(source_buf, sizeof(source_buf), "%s:%d", source_file, xr_coro_source_line(coro));
    XrValue source = aot_vm_host_string_value(vm_host, source_buf, strlen(source_buf));
    if (!XR_IS_NULL(source))
        xr_map_set(info, aot_coro_intern_key(vm_host, "source"), source);
}

static XrRuntime *aot_vm_host_scheduler(XrAotVmHost vm_host) {
    return aot_vm_host_available(vm_host) && vm_host.ops->scheduler
               ? vm_host.ops->scheduler(vm_host.host)
               : NULL;
}

static XrRuntimeCore *aot_vm_host_runtime_core(XrAotVmHost vm_host) {
    return aot_vm_host_available(vm_host) && vm_host.ops->runtime_core
               ? vm_host.ops->runtime_core(vm_host.host)
               : NULL;
}

static int aot_coro_collect_all(XrAotVmHost vm_host, XrCoroSnapshotEntry *out, int max_out) {
    XrRuntime *runtime = aot_vm_host_scheduler(vm_host);
    return runtime ? xr_runtime_collect_coros(runtime, out, max_out) : 0;
}

static XrValue aot_coro_stats(XrAotVmHost vm_host, XrCoroutine *owner) {
    XrRuntime *runtime = aot_vm_host_scheduler(vm_host);
    if (!runtime)
        return XR_NULL_VAL;

    int blocked_count = 0;
    int ready_count = 0;
    int active_count = 0;
    uint64_t total_created = 0;
    for (int si = 0; si < runtime->worker_count; si++)
        total_created += runtime->workers[si].p.stats.spawned_count;

    XrCoroSnapshotEntry *entries =
        (XrCoroSnapshotEntry *) xr_malloc(sizeof(XrCoroSnapshotEntry) * XR_AOT_CORO_COLLECT_MAX);
    if (entries) {
        int total = aot_coro_collect_all(vm_host, entries, XR_AOT_CORO_COLLECT_MAX);
        for (int i = 0; i < total; i++) {
            if (strcmp(entries[i].state, "ready") == 0)
                ready_count++;
            else if (strcmp(entries[i].state, "blocked") == 0)
                blocked_count++;
            else if (strcmp(entries[i].state, "running") == 0)
                active_count++;
        }
        xr_free(entries);
    } else {
        active_count = xr_runtime_active_coros(runtime);
        for (int wi = 0; wi < runtime->worker_count; wi++) {
            XrWorker *w = &runtime->workers[wi];
            blocked_count += w->p.blocked_count;
            ready_count += xr_runq_len(&w->p.runq);
        }
    }

    XrMap *result = aot_coro_new_map(vm_host, owner);
    if (!result)
        return XR_NULL_VAL;
    int total_alive = ready_count + blocked_count + active_count;
    xr_map_set(result, aot_coro_intern_key(vm_host, "active"), xr_int(active_count));
    xr_map_set(result, aot_coro_intern_key(vm_host, "blocked"), xr_int(blocked_count));
    xr_map_set(result, aot_coro_intern_key(vm_host, "ready"), xr_int(ready_count));
    xr_map_set(result, aot_coro_intern_key(vm_host, "total"), xr_int(total_alive));
    xr_map_set(result, aot_coro_intern_key(vm_host, "created"), xr_int((int) total_created));
    return xr_value_from_map(result);
}

static XrValue aot_coro_list(XrAotVmHost vm_host, XrCoroutine *owner, const XrValue *args,
                             int argc) {
    int limit = 100000;
    int state_filter = 0;

    if (argc > 0 && XR_IS_INT(args[0])) {
        limit = (int) XR_TO_INT(args[0]);
        if (limit <= 0)
            limit = 100000;
    }
    if (argc > 1) {
        XrValue state_val = args[1];
        if (XR_IS_INT(state_val)) {
            state_filter = (int) XR_TO_INT(state_val);
        } else if (XR_IS_STRING(state_val)) {
            XrString *s = XR_TO_STRING(state_val);
            if (strcmp(s->data, "ready") == 0)
                state_filter = 1;
            else if (strcmp(s->data, "blocked") == 0)
                state_filter = 2;
        }
    }

    XrArray *result = aot_coro_new_array(vm_host, owner);
    if (!result)
        return XR_NULL_VAL;

    XrCoroSnapshotEntry *entries =
        (XrCoroSnapshotEntry *) xr_malloc(sizeof(XrCoroSnapshotEntry) * XR_AOT_CORO_COLLECT_MAX);
    if (!entries)
        return xr_value_from_array(result);

    int total = aot_coro_collect_all(vm_host, entries, XR_AOT_CORO_COLLECT_MAX);
    int count = 0;
    for (int i = 0; i < total && count < limit; i++) {
        XrCoroutine *coro = entries[i].coro;
        const char *st = entries[i].state;
        bool is_ready = strcmp(st, "ready") == 0;
        bool is_blocked = strcmp(st, "blocked") == 0;

        if (state_filter == 1 && !is_ready)
            continue;
        if (state_filter == 2 && !is_blocked)
            continue;

        XrMap *info = aot_coro_new_map(vm_host, owner);
        if (!info)
            continue;
        xr_map_set(info, aot_coro_intern_key(vm_host, "id"), xr_int(coro->id));
        xr_map_set(info, aot_coro_intern_key(vm_host, "name"), aot_coro_name_value(vm_host, coro));
        xr_map_set(info, aot_coro_intern_key(vm_host, "state"),
                   aot_vm_host_string_value(vm_host, st, strlen(st)));
        aot_coro_set_source_field(vm_host, info, coro);
        xr_array_push(result, xr_value_from_map(info));
        count++;
    }

    xr_free(entries);
    return xr_value_from_array(result);
}

static XrValue aot_coro_info(XrAotVmHost vm_host, XrCoroutine *owner, XrValue coro_val) {
    if (!xr_value_is_coro(coro_val))
        return XR_NULL_VAL;

    XrCoroutine *coro = xr_value_to_coro(coro_val);
    XrMap *info = aot_coro_new_map(vm_host, owner);
    if (!info)
        return XR_NULL_VAL;

    uint32_t flags = xr_coro_flags_load(coro);
    const char *state_str = "unknown";
    if (flags & XR_CORO_FLG_DONE)
        state_str = "done";
    else if (flags & XR_CORO_FLG_BLOCKED)
        state_str = "blocked";
    else if (flags & XR_CORO_FLG_RUNNING)
        state_str = "running";
    else if (flags & XR_CORO_FLG_READY)
        state_str = "ready";

    xr_map_set(info, aot_coro_intern_key(vm_host, "id"), xr_int(coro->id));
    xr_map_set(info, aot_coro_intern_key(vm_host, "name"), aot_coro_name_value(vm_host, coro));
    xr_map_set(info, aot_coro_intern_key(vm_host, "state"),
               aot_vm_host_string_value(vm_host, state_str, strlen(state_str)));
    xr_map_set(info, aot_coro_intern_key(vm_host, "reductions"), xr_int(xr_coro_reds(coro)));
    aot_coro_set_source_field(vm_host, info, coro);

    XrMap *locals = coro->ext ? coro->ext->locals : NULL;
    xr_map_set(info, aot_coro_intern_key(vm_host, "locals"),
               locals ? xr_value_from_map(locals) : aot_coro_empty_map_value(vm_host, owner));

    const XrCoroWaitState *wait = xr_coro_wait_state_const(coro);
    int wait_count = wait ? atomic_load(&wait->wait_count) : 0;
    xr_map_set(info, aot_coro_intern_key(vm_host, "waitCount"), xr_int(wait_count));
    xr_map_set(info, aot_coro_intern_key(vm_host, "cancelled"),
               xr_bool(flags & XR_CORO_FLG_CANCELLED));
    if (flags & XR_CORO_FLG_DONE)
        xr_map_set(info, aot_coro_intern_key(vm_host, "result"), coro->result);
    if (flags & XR_CORO_FLG_BLOCKED) {
        void *wait_channel =
            coro->ext ? atomic_load_explicit(&coro->ext->wait_channel, memory_order_acquire) : NULL;
        const char *reason = wait_channel ? "channel" : "await";
        xr_map_set(info, aot_coro_intern_key(vm_host, "blockedOn"),
                   aot_vm_host_string_value(vm_host, reason, strlen(reason)));
    }
    return xr_value_from_map(info);
}

static XrValue aot_coro_dump(XrAotVmHost vm_host, const XrValue *args, int argc) {
    int limit = 100;
    if (argc > 0 && XR_IS_INT(args[0])) {
        limit = (int) XR_TO_INT(args[0]);
        if (limit <= 0)
            limit = 100;
    }

    XrCoroSnapshotEntry *entries =
        (XrCoroSnapshotEntry *) xr_malloc(sizeof(XrCoroSnapshotEntry) * XR_AOT_CORO_COLLECT_MAX);
    if (!entries)
        return XR_NULL_VAL;
    int total = aot_coro_collect_all(vm_host, entries, XR_AOT_CORO_COLLECT_MAX);

    int ready_count = 0;
    int blocked_count = 0;
    for (int i = 0; i < total; i++) {
        if (strcmp(entries[i].state, "ready") == 0)
            ready_count++;
        else if (strcmp(entries[i].state, "blocked") == 0)
            blocked_count++;
    }

    printf("┌─────────────────────────────────────────────────────────────────────────┐\n");
    printf("│                     Coroutine Status Snapshot                           │\n");
    printf("├─────────────────────────────────────────────────────────────────────────┤\n");
    printf("│ Stats: Total %-5d | Ready %-4d | Blocked %-4d                        │\n", total,
           ready_count, blocked_count);
    printf("├──────┬────────────────┬─────────┬─────────────────┬─────────────────────┤\n");
    printf("│ ID   │ Name           │ State   │ Block Reason    │ Location            │\n");
    printf("├──────┼────────────────┼─────────┼─────────────────┼─────────────────────┤\n");

    int shown = 0;
    for (int i = 0; i < total && shown < limit; i++) {
        XrCoroutine *coro = entries[i].coro;
        const char *state_upper = "READY";
        if (strcmp(entries[i].state, "blocked") == 0)
            state_upper = "BLOCKED";
        else if (strcmp(entries[i].state, "running") == 0)
            state_upper = "RUNNING";
        const char *block_reason = "-";
        if (strcmp(entries[i].state, "blocked") == 0) {
            void *wait_channel =
                coro->ext ? atomic_load_explicit(&coro->ext->wait_channel, memory_order_acquire)
                          : NULL;
            block_reason = wait_channel ? "channel" : "await";
        }

        const char *name = xr_coro_name(coro);
        name = name ? name : "(anonymous)";
        char name_buf[15];
        snprintf(name_buf, sizeof(name_buf), "%.14s", name);

        char source_buf[20] = "-";
        const char *source_file = xr_coro_source_file(coro);
        if (source_file) {
            const char *fname = strrchr(source_file, '/');
            fname = fname ? fname + 1 : source_file;
            snprintf(source_buf, sizeof(source_buf), "%.12s:%d", fname, xr_coro_source_line(coro));
        }

        printf("│ %-4d │ %-14s │ %-7s │ %-15s │ %-19s │\n", coro->id, name_buf, state_upper,
               block_reason, source_buf);
        shown++;
    }

    printf("└──────┴────────────────┴─────────┴─────────────────┴─────────────────────┘\n");
    xr_free(entries);
    return XR_NULL_VAL;
}

static XrValue aot_coro_top(XrAotVmHost vm_host, XrCoroutine *owner, const XrValue *args,
                            int argc) {
    int top_n = 10;
    int metric = 0;
    if (argc > 0 && XR_IS_INT(args[0])) {
        top_n = (int) XR_TO_INT(args[0]);
        if (top_n <= 0)
            top_n = 10;
        if (top_n > 1000)
            top_n = 1000;
    }
    if (argc > 1 && XR_IS_STRING(args[1])) {
        XrString *s = XR_TO_STRING(args[1]);
        metric = strcmp(s->data, "reductions") == 0 ? 2 : 0;
    }

    XrArray *result = aot_coro_new_array(vm_host, owner);
    if (!result)
        return XR_NULL_VAL;

    XrCoroSnapshotEntry *entries =
        (XrCoroSnapshotEntry *) xr_malloc(sizeof(XrCoroSnapshotEntry) * XR_AOT_CORO_COLLECT_MAX);
    if (!entries)
        return xr_value_from_array(result);
    int count = aot_coro_collect_all(vm_host, entries, XR_AOT_CORO_COLLECT_MAX);
    for (int j = 0; j < top_n && j < count; j++) {
        int max_idx = j;
        for (int k = j + 1; k < count; k++) {
            int64_t lhs =
                metric == 2 ? xr_coro_reds(entries[k].coro) : (int64_t) entries[k].coro->id;
            int64_t rhs = metric == 2 ? xr_coro_reds(entries[max_idx].coro)
                                      : (int64_t) entries[max_idx].coro->id;
            if (lhs > rhs)
                max_idx = k;
        }
        if (max_idx != j) {
            XrCoroSnapshotEntry tmp = entries[j];
            entries[j] = entries[max_idx];
            entries[max_idx] = tmp;
        }
    }

    int result_count = top_n < count ? top_n : count;
    for (int j = 0; j < result_count; j++) {
        XrCoroutine *coro = entries[j].coro;
        XrMap *info = aot_coro_new_map(vm_host, owner);
        if (!info)
            continue;
        xr_map_set(info, aot_coro_intern_key(vm_host, "id"), xr_int(coro->id));
        xr_map_set(info, aot_coro_intern_key(vm_host, "name"), aot_coro_name_value(vm_host, coro));
        xr_map_set(info, aot_coro_intern_key(vm_host, "state"),
                   aot_vm_host_string_value(vm_host, entries[j].state, strlen(entries[j].state)));
        xr_map_set(info, aot_coro_intern_key(vm_host, "reductions"), xr_int(xr_coro_reds(coro)));
        aot_coro_set_source_field(vm_host, info, coro);
        xr_array_push(result, xr_value_from_map(info));
    }

    xr_free(entries);
    return xr_value_from_array(result);
}

static XrValue aot_coro_group_by(XrAotVmHost vm_host, XrCoroutine *owner, const XrValue *args,
                                 int argc) {
    int group_by = 0;
    if (argc > 0 && XR_IS_STRING(args[0])) {
        XrString *s = XR_TO_STRING(args[0]);
        if (strcmp(s->data, "state") == 0)
            group_by = 1;
    }

    XrMap *result = aot_coro_new_map(vm_host, owner);
    if (!result)
        return XR_NULL_VAL;

    XrCoroSnapshotEntry *entries =
        (XrCoroSnapshotEntry *) xr_malloc(sizeof(XrCoroSnapshotEntry) * XR_AOT_CORO_COLLECT_MAX);
    if (!entries)
        return xr_value_from_map(result);
    int total = aot_coro_collect_all(vm_host, entries, XR_AOT_CORO_COLLECT_MAX);
    for (int i = 0; i < total; i++) {
        XrCoroutine *coro = entries[i].coro;
        const char *key_str = group_by == 0 ? xr_coro_name(coro) : entries[i].state;
        if (!key_str)
            key_str = "(anonymous)";
        XrValue key = aot_vm_host_string_value(vm_host, key_str, strlen(key_str));
        bool found = false;
        XrValue existing = xr_map_get(result, key, &found);
        xr_map_set(result, key,
                   found && XR_IS_INT(existing) ? xr_int(XR_TO_INT(existing) + 1) : xr_int(1));
    }

    xr_free(entries);
    return xr_value_from_map(result);
}

static XrCoroState *aot_coro_sched(XrAotVmHost vm_host) {
    return aot_vm_host_available(vm_host) && vm_host.ops->coro_state
               ? vm_host.ops->coro_state(vm_host.host)
               : NULL;
}

XrValue xr_aot_coro_op(const XrAotContext *ctx, int32_t sub_op, const XrValue *args, int argc) {
    if (argc < 0)
        argc = 0;
    XrAotVmHost vm_host = aot_context_vm_host(ctx, NULL);
    XrCoroutine *current = aot_context_coro(ctx, vm_host, NULL);
    if (!aot_vm_host_available(vm_host))
        return XR_NULL_VAL;

    switch (sub_op) {
        case 0: {
            if (argc < 2)
                return XR_NULL_VAL;
            if (!current) {
                XrMap *main_locals = vm_host.ops->ensure_main_locals
                                         ? vm_host.ops->ensure_main_locals(vm_host.host, current)
                                         : NULL;
                if (main_locals)
                    xr_map_set(main_locals, args[0], args[1]);
            } else {
                XrCoroExt *ext = xr_coro_ensure_ext(current);
                if (ext) {
                    if (!ext->locals)
                        ext->locals = xr_map_new(current);
                    if (ext->locals)
                        xr_map_set(ext->locals, args[0], args[1]);
                }
            }
            return XR_NULL_VAL;
        }
        case 1: {
            if (argc < 1)
                return XR_NULL_VAL;
            XrMap *locals =
                current
                    ? (current->ext ? current->ext->locals : NULL)
                    : (vm_host.ops->main_locals ? vm_host.ops->main_locals(vm_host.host) : NULL);
            if (!locals)
                return XR_NULL_VAL;
            bool found = false;
            XrValue result = xr_map_get(locals, args[0], &found);
            return found ? result : XR_NULL_VAL;
        }
        case 2: {
            if (current) {
                XrCoroExt *ext = xr_coro_ensure_ext(current);
                if (ext) {
                    int old_count = atomic_fetch_add(&ext->lock_count, 1);
                    if (old_count == 0) {
                        XrWorker *worker = xr_current_worker();
                        ext->locked_worker = worker ? worker->p.id : 0;
                    }
                }
            }
            return XR_NULL_VAL;
        }
        case 3: {
            if (current && current->ext) {
                int old_count = atomic_fetch_sub(&current->ext->lock_count, 1);
                if (old_count <= 1) {
                    atomic_store(&current->ext->lock_count, 0);
                    current->ext->locked_worker = -1;
                }
            }
            return XR_NULL_VAL;
        }
        default:
            break;
    }

    if (sub_op < 100)
        return XR_NULL_VAL;
    int ctrl_sub = sub_op - 100;
    switch (ctrl_sub) {
        case CORO_CTRL_STATS:
            return aot_coro_stats(vm_host, current);
        case CORO_CTRL_LIST:
            return aot_coro_list(vm_host, current, args, argc);
        case CORO_CTRL_INFO:
            return argc > 0 ? aot_coro_info(vm_host, current, args[0]) : XR_NULL_VAL;
        case CORO_CTRL_DUMP:
            return aot_coro_dump(vm_host, args, argc);
        case CORO_CTRL_STALLED:
        case CORO_CTRL_DEADLOCKS:
            return aot_coro_empty_array_value(vm_host, current);
        case CORO_CTRL_TOP:
            return aot_coro_top(vm_host, current, args, argc);
        case CORO_CTRL_GROUP_BY:
            return aot_coro_group_by(vm_host, current, args, argc);
        case CORO_CTRL_WHEREIS: {
            if (argc < 1 || !XR_IS_STRING(args[0]))
                return xr_bool(false);
            XrCoroState *sched = aot_coro_sched(vm_host);
            if (!sched || !sched->coro_registry)
                return xr_bool(false);
            const char *name = xr_value_str_data(&args[0]);
            return xr_bool(xr_coro_registry_whereis(sched->coro_registry, name) != NULL);
        }
        case CORO_CTRL_MONITOR: {
            if (argc < 1 || !XR_IS_STRING(args[0]))
                return XR_NULL_VAL;
            XrCoroState *sched = aot_coro_sched(vm_host);
            if (!sched || !sched->coro_registry || !vm_host.ops->monitor)
                return XR_NULL_VAL;
            const char *name = xr_value_str_data(&args[0]);
            XrChannel *ch = vm_host.ops->monitor(vm_host.host, sched->coro_registry, name);
            return ch ? xr_value_from_channel(ch) : XR_NULL_VAL;
        }
        case CORO_CTRL_DEMONITOR: {
            if (argc < 2 || !XR_IS_STRING(args[0]) || !xr_value_is_channel(args[1]))
                return XR_NULL_VAL;
            XrCoroState *sched = aot_coro_sched(vm_host);
            if (!sched || !sched->coro_registry)
                return XR_NULL_VAL;
            const char *name = xr_value_str_data(&args[0]);
            XrCoroutine *coro = xr_coro_registry_whereis(sched->coro_registry, name);
            if (coro)
                xr_coro_demonitor(sched->coro_registry, coro, xr_value_to_channel(args[1]));
            return XR_NULL_VAL;
        }
        case CORO_CTRL_KILL: {
            if (argc < 1 || !XR_IS_STRING(args[0]))
                return xr_bool(false);
            XrCoroState *sched = aot_coro_sched(vm_host);
            if (!sched || !sched->coro_registry)
                return xr_bool(false);
            const char *name = xr_value_str_data(&args[0]);
            XrCoroutine *target = xr_coro_registry_whereis(sched->coro_registry, name);
            if (!target || xr_coro_flags_has(target, XR_CORO_FLG_DONE))
                return xr_bool(false);
            xr_coro_flags_set(target, XR_CORO_FLG_CANCEL_REQUESTED);
            xr_coro_request_yield(target);
            return xr_bool(true);
        }
        case CORO_CTRL_SELF: {
            const char *name = xr_coro_name(current);
            if (!name)
                return XR_NULL_VAL;
            return aot_vm_host_string_value(vm_host, name, strlen(name));
        }
        default:
            return XR_NULL_VAL;
    }
}

static XrRuntimeCore *aot_context_runtime_core(const XrAotContext *ctx);
static XrRuntime *aot_context_scheduler(const XrAotContext *ctx);

static XrEnumType *aot_builtin_enum_type(const XrAotContext *ctx, int builtin_index) {
    XrValue value = xr_aot_get_builtin(ctx, builtin_index);
    if (!XR_IS_PTR(value))
        return NULL;
    return (XrEnumType *) XR_TO_PTR(value);
}

static XrValue aot_builtin_enum_member(const XrAotContext *ctx, int builtin_index,
                                       uint32_t member_index) {
    XrEnumType *type = aot_builtin_enum_type(ctx, builtin_index);
    if (!type || member_index >= type->member_count || !type->members[member_index].instance)
        return xr_null();
    XrValue value = xr_null();
    value.tag = XR_AOT_VALUE_TAG_ENUM;
    value.ext = member_index;
    value.ptr = type->members[member_index].instance;
    return value;
}

static XrValue aot_builtin_adt_value(const XrAotContext *ctx, int builtin_index,
                                     uint32_t member_index, XrValue *args, int nargs) {
    XrEnumType *type = aot_builtin_enum_type(ctx, builtin_index);
    if (!type || !type->is_adt)
        return xr_null();
    XrInstance *inst = xr_enum_adt_construct_core(
        aot_context_runtime_core(ctx), ctx ? ctx->coro : NULL, type, member_index, args, nargs);
    return inst ? XR_FROM_PTR(inst) : xr_null();
}

static XrValue aot_recv_value(const XrAotContext *ctx, XrValue value) {
    XrValue args[1] = {value};
    return aot_builtin_adt_value(ctx, XR_GLOBAL_VAR_RECV, 0, args, 1);
}

static XrValue aot_recv_empty(const XrAotContext *ctx) {
    return aot_builtin_enum_member(ctx, XR_GLOBAL_VAR_RECV, 1);
}

static XrValue aot_recv_timeout(const XrAotContext *ctx) {
    return aot_builtin_enum_member(ctx, XR_GLOBAL_VAR_RECV, 2);
}

static XrValue aot_recv_closed(const XrAotContext *ctx) {
    return aot_builtin_enum_member(ctx, XR_GLOBAL_VAR_RECV, 3);
}

static XrValue aot_recv_result_from_block(const XrAotContext *ctx, XrCoroBlockResult block) {
    if (block.kind == XR_CORO_BLOCK_READY)
        return aot_recv_value(ctx, block.value);
    if (block.kind == XR_CORO_BLOCK_TIMEOUT)
        return aot_recv_timeout(ctx);
    if (block.kind == XR_CORO_BLOCK_CLOSED || block.kind == XR_CORO_BLOCK_NO_CORO)
        return aot_recv_closed(ctx);
    return xr_null();
}

static XrValue aot_send_result(const XrAotContext *ctx, uint32_t member_index) {
    return aot_builtin_enum_member(ctx, XR_GLOBAL_VAR_SEND_RESULT, member_index);
}

static XrValue aot_send_result_from_block(const XrAotContext *ctx, XrCoroBlockResult block) {
    if (block.kind == XR_CORO_BLOCK_READY)
        return aot_send_result(ctx, 0);
    if (block.kind == XR_CORO_BLOCK_TIMEOUT)
        return aot_send_result(ctx, 2);
    if (block.kind == XR_CORO_BLOCK_CLOSED || block.kind == XR_CORO_BLOCK_NO_CORO)
        return aot_send_result(ctx, 3);
    return aot_send_result(ctx, 1);
}

static XrValue aot_task_status_from_state(const XrAotContext *ctx, uint8_t state) {
    uint32_t member_index = 0;
    switch (state) {
        case XR_TASK_ACTIVE:
        case XR_TASK_COMPLETING:
            member_index = 1;  // Running
            break;
        case XR_TASK_COMPLETED:
            member_index = 2;  // Success
            break;
        case XR_TASK_FAILED:
            member_index = 3;  // Failed
            break;
        case XR_TASK_CANCELLING:
        case XR_TASK_CANCELLED:
        default:
            member_index = 4;  // Cancelled
            break;
    }
    return aot_builtin_enum_member(ctx, XR_GLOBAL_VAR_TASK_STATUS, member_index);
}

static XrValue aot_task_result_success(const XrAotContext *ctx, XrValue value) {
    XrValue args[1] = {value};
    return aot_builtin_adt_value(ctx, XR_GLOBAL_VAR_TASK_RESULT, 0, args, 1);
}

static XrValue aot_task_failure_exception(const XrAotContext *ctx, XrTask *task) {
    XrAotVmHost vm_host = aot_context_vm_host(ctx, task);
    XrCoroutine *coro = aot_context_coro(ctx, vm_host, task);
    XrRuntimeCore *core = aot_context_runtime_core(ctx);
    XrValue error = task ? task->error : xr_null();
    if (XR_IS_NULL(error) && aot_vm_host_available(vm_host) && vm_host.ops->exception_new)
        return vm_host.ops->exception_new(vm_host.host, XR_ERR_RUNTIME, "Task failed");
    if (XR_IS_NULL(error)) {
        XrString *msg = xr_string_intern_core(core, "Task failed", strlen("Task failed"), 0);
        return msg ? xr_string_value(msg) : xr_null();
    }
    if (coro && xr_value_needs_copy(error))
        error = xr_deep_copy_to_coro_core(core, error, coro);
    if (aot_vm_host_available(vm_host) && vm_host.ops->is_exception &&
        vm_host.ops->exception_from_value && !vm_host.ops->is_exception(vm_host.host, error)) {
        error = vm_host.ops->exception_from_value(vm_host.host, error);
    }
    return error;
}

static XrValue aot_task_result_failed(const XrAotContext *ctx, XrTask *task) {
    XrValue error = aot_task_failure_exception(ctx, task);
    XrValue args[1] = {error};
    return aot_builtin_adt_value(ctx, XR_GLOBAL_VAR_TASK_RESULT, 1, args, 1);
}

static XrValue aot_task_result_member(const XrAotContext *ctx, uint32_t member_index) {
    return aot_builtin_enum_member(ctx, XR_GLOBAL_VAR_TASK_RESULT, member_index);
}

static XrValue aot_task_result_from_terminal(const XrAotContext *ctx, XrTask *task) {
    uint8_t state =
        task ? atomic_load_explicit(&task->state, memory_order_acquire) : XR_TASK_CANCELLED;
    if (state == XR_TASK_COMPLETED) {
        XrAotVmHost vm_host = aot_context_vm_host(ctx, task);
        XrCoroutine *coro = aot_context_coro(ctx, vm_host, task);
        XrValue value =
            xr_coro_await_result_value(aot_context_runtime_core(ctx), coro, task, false);
        return aot_task_result_success(ctx, value);
    }
    if (state == XR_TASK_FAILED)
        return aot_task_result_failed(ctx, task);
    if (state == XR_TASK_CANCELLED || state == XR_TASK_CANCELLING)
        return aot_task_result_member(ctx, 2);
    return aot_task_result_member(ctx, 4);
}

static XrValue aot_task_result_from_block(const XrAotContext *ctx, XrTask *task,
                                          XrCoroBlockResult block, XrValue raw_value,
                                          bool timeout_enabled) {
    if (block.kind == XR_CORO_BLOCK_READY)
        return aot_task_result_success(ctx, raw_value);
    if (block.kind == XR_CORO_BLOCK_TIMEOUT)
        return timeout_enabled ? aot_task_result_member(ctx, 3)
                               : aot_task_result_from_terminal(ctx, task);
    if (block.kind == XR_CORO_BLOCK_CLOSED || block.kind == XR_CORO_BLOCK_NO_CORO)
        return aot_task_result_from_terminal(ctx, task);
    return aot_task_result_member(ctx, 4);
}

static XrAotResult aot_task_terminal_error(const XrAotContext *ctx, XrTask *task) {
    uint8_t state =
        task ? atomic_load_explicit(&task->state, memory_order_acquire) : XR_TASK_CANCELLED;
    XrValue error;
    if (state == XR_TASK_FAILED) {
        error = aot_task_failure_exception(ctx, task);
    } else {
        XrAotVmHost vm_host = aot_context_vm_host(ctx, task);
        if (aot_vm_host_available(vm_host) && vm_host.ops->exception_new) {
            error =
                vm_host.ops->exception_new(vm_host.host, XR_ERR_CORO_CANCELLED, "Task cancelled");
        } else {
            XrString *msg = xr_string_intern_core(aot_context_runtime_core(ctx), "Task cancelled",
                                                  strlen("Task cancelled"), 0);
            error = msg ? xr_string_value(msg) : xr_null();
        }
    }
    return xr_aot_error(error, false);
}

static XrAotResult aot_channel_closed_error(const XrAotContext *ctx) {
    XrAotVmHost vm_host = aot_context_vm_host(ctx, NULL);
    XrValue error;
    if (aot_vm_host_available(vm_host) && vm_host.ops->exception_new) {
        error = vm_host.ops->exception_new(vm_host.host, XR_ERR_CORO_DEAD, "Channel is closed");
    } else {
        XrString *msg = xr_string_intern_core(aot_context_runtime_core(ctx), "Channel is closed",
                                              strlen("Channel is closed"), 0);
        error = msg ? xr_string_value(msg) : xr_null();
    }
    return xr_aot_error(error, false);
}

static XrArray *aot_tasks_array_from_value(const XrAotContext *ctx, XrValue tasks_value) {
    if (!ctx || !ctx->coro)
        return NULL;
    if (xr_value_is_array(tasks_value))
        return xr_value_to_array(tasks_value);
    if (tasks_value.tag != XR_AOT_VALUE_TAG_ARRAY || !tasks_value.ptr)
        return NULL;

    /* An AOT array embeds the unified XrObjHeader and the shared
     * XR_ARRAY_ABI_FIELDS, so it is layout-compatible with the VM XrArray and is
     * read through it directly. Elements are copied into a coroutine-owned VM
     * array because the source is bump storage owned by the generated frame. */
    const XrArray *src = (const XrArray *) tasks_value.ptr;
    int64_t len = src->length;
    if (len < 0 || len > INT32_MAX)
        return NULL;
    if (src->elem_type >= XR_ELEM_COUNT)
        return NULL;

    XrArray *tasks = xr_array_with_capacity(ctx->coro, (int) len);
    if (!tasks)
        return NULL;

    for (int64_t i = 0; i < len; i++) {
        XrValue item =
            src->data ? xr_typed_get(src->data, (int32_t) i, src->elem_type) : XR_NULL_VAL;
        xr_array_push(tasks, item);
    }
    return tasks;
}

static bool aot_all_tasks_done(XrArray *tasks) {
    int count = xr_array_size(tasks);
    for (int j = 0; j < count; j++) {
        XrValue cv = xr_array_get(tasks, j);
        if (!xr_value_is_task(cv))
            continue;
        if (!xr_task_is_done(xr_value_to_task(cv)))
            return false;
    }
    return true;
}

static XrValue aot_collect_await_all_results(const XrAotContext *ctx, XrArray *tasks) {
    if (!ctx || !ctx->coro || !tasks)
        return XR_NULL_VAL;

    int count = xr_array_size(tasks);
    XrArray *results = xr_array_with_capacity(ctx->coro, count);
    if (!results)
        return XR_NULL_VAL;

    for (int j = 0; j < count; j++) {
        XrValue cv = xr_array_get(tasks, j);
        XrValue value = XR_NULL_VAL;
        if (xr_value_is_task(cv)) {
            XrTask *task = xr_value_to_task(cv);
            value =
                xr_coro_await_result_value(aot_context_runtime_core(ctx), ctx->coro, task, false);
        }
        xr_array_push(results, value);
    }
    return xr_value_from_array(results);
}

static XrAotResult aot_await_all_ready_result(const XrAotContext *ctx, XrArray *tasks,
                                              XrSlotRef out_slot) {
    if (ctx && ctx->coro)
        xr_task_finish_await_waiters(ctx->coro);
    XrValue results = aot_collect_await_all_results(ctx, tasks);
    return aot_store_slot_result(out_slot, results);
}

static XrAotResult aot_await_any_ready_result(const XrAotContext *ctx, XrArray *tasks,
                                              XrSlotRef out_slot, bool success_only,
                                              bool *found_pending) {
    int count = xr_array_size(tasks);
    int done_count = 0;
    int task_count = 0;
    if (found_pending)
        *found_pending = false;

    for (int j = 0; j < count; j++) {
        XrValue cv = xr_array_get(tasks, j);
        if (!xr_value_is_task(cv))
            continue;
        task_count++;

        XrTask *task = xr_value_to_task(cv);
        if (!xr_task_is_done(task)) {
            if (found_pending)
                *found_pending = true;
            continue;
        }

        done_count++;
        if (!success_only || XR_IS_NULL(task->error)) {
            if (ctx && ctx->coro)
                xr_task_finish_await_waiters(ctx->coro);
            XrValue value =
                xr_coro_await_result_value(aot_context_runtime_core(ctx), ctx->coro, task, false);
            return aot_store_slot_result(out_slot, value);
        }
    }

    if (task_count == 0 || (success_only && done_count == task_count)) {
        if (ctx && ctx->coro)
            xr_task_finish_await_waiters(ctx->coro);
        return aot_store_slot_result(out_slot, XR_NULL_VAL);
    }
    return xr_aot_result(XR_AOT_RUN_BLOCKED);
}

XrAotSpawnResult xr_aot_spawn(const XrAotContext *ctx, const XrAotCoroDesc *desc, void *frame,
                              int link_mode, bool fire_and_forget, const char *name) {
    XrAotSpawnResult result;
    result.task_value = XR_NULL_VAL;
    result.child = NULL;

    if (!ctx || !desc || !frame)
        return result;

    if (!ctx->runtime)
        return result;

    XrCoroutine *child = xr_coro_create_aot(ctx->runtime, desc, frame, name);
    if (!child)
        return result;

    XrRuntime *runtime = xr_aot_runtime_scheduler(ctx->runtime);
    if (!runtime) {
        xr_coro_destroy(child);
        return result;
    }

    XrTask *task = xr_task_create(runtime, ctx->coro, child);
    if (!task) {
        xr_coro_destroy(child);
        return result;
    }
    task->link_mode = (uint8_t) link_mode;

    if (fire_and_forget)
        child->gc_flags |= XR_CORO_GC_RECYCLABLE;

    XrCoroutine *parent = ctx->coro;
    if (parent && !xr_coro_set_pending_spawn(parent, child)) {
        xr_coro_destroy(child);
        return result;
    }

    XrScopeContext *scope =
        parent ? atomic_load_explicit(&parent->current_scope, memory_order_relaxed) : NULL;
    if (!scope && runtime)
        scope = runtime->current_scope;
    if (scope && parent) {
        if (!xr_coro_set_parent_scope(child, scope)) {
            (void) xr_coro_set_pending_spawn(parent, NULL);
            xr_coro_destroy(child);
            return result;
        }
        while (atomic_exchange_explicit(&scope->child_lock, true, memory_order_acquire)) {
        }
        if (!xr_coro_set_scope_sibling(child, scope->first_child)) {
            atomic_store_explicit(&scope->child_lock, false, memory_order_release);
            (void) xr_coro_set_pending_spawn(parent, NULL);
            xr_coro_destroy(child);
            return result;
        }
        scope->first_child = child;
        atomic_store_explicit(&scope->child_lock, false, memory_order_release);
        atomic_fetch_add_explicit(&scope->count, 1, memory_order_relaxed);
        if (link_mode == XR_LINK_NONE && scope->mode == XR_SCOPE_LINKED)
            task->link_mode = XR_LINK_LINKED;
    }

    if (task->link_mode == XR_LINK_LINKED && parent && parent->task && !xr_coro_parent_scope(child))
        xr_task_attach_child(parent->task, task);

    result.task_value = xr_value_from_task(task);
    result.child = child;
    if (!parent && runtime) {
        xr_runtime_spawn(runtime, child);
    }
    return result;
}

XrAotResult xr_aot_scope_enter(const XrAotContext *ctx, uint8_t scope_mode) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrCoroBlockResult block = xr_coro_scope_enter(ctx->coro, scope_mode);
    if (block.kind == XR_CORO_BLOCK_READY)
        return xr_aot_done(XR_NULL_VAL);
    return xr_aot_error(block.value, block.ok);
}

XrAotResult xr_aot_scope_exit(const XrAotContext *ctx, uint8_t scope_mode, XrValue *out_value) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrCoroBlockResult block = xr_coro_scope_exit(ctx->coro, scope_mode);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY) {
        if (out_value)
            *out_value = block.value;
        return xr_aot_done(block.value);
    }
    if (block.kind == XR_CORO_BLOCK_NO_CORO) {
        if (out_value)
            *out_value = XR_NULL_VAL;
        return xr_aot_done(XR_NULL_VAL);
    }
    return xr_aot_error(block.value, block.ok);
}

XrValue xr_aot_time_after(const XrAotContext *ctx, int64_t milliseconds) {
    if (!ctx)
        return XR_NULL_VAL;
    if (milliseconds < 0)
        milliseconds = 0;

    XrRuntimeCore *core = aot_context_runtime_core(ctx);
    XrRuntime *scheduler = aot_context_scheduler(ctx);
    XrAotVmHost vm_host = aot_context_vm_host(ctx, NULL);
    XrChannel *timer_ch =
        !ctx->runtime && aot_vm_host_available(vm_host) && vm_host.ops->new_timer_channel
            ? vm_host.ops->new_timer_channel(vm_host.host, milliseconds)
            : xr_channel_new_timer(core, scheduler, milliseconds);
    if (!timer_ch)
        return XR_NULL_VAL;

    XrWorker *worker = ctx->worker ? (XrWorker *) ctx->worker : xr_current_worker();
    if (worker && worker->p.timer_wheel)
        xr_channel_timer_arm(timer_ch, worker->p.timer_wheel);
    return xr_value_from_channel(timer_ch);
}

// Release a select-owned `after` timer channel. Emitted at the select merge for
// every case body (XI_CHAN_TIMER_DISPOSE), this drops the select handle
// reference the compiler omits across the select.block suspend, and cancels the
// still-armed wheel timer when on its owner worker. Mirrors the VM
// OP_CHAN_TIMER_DISPOSE handler; the underlying xr_channel_timer_dispose is
// shared with the VM backend.
void xr_aot_chan_timer_dispose(const XrAotContext *ctx, XrValue ch_value) {
    (void) ctx;
    if (xr_value_is_channel(ch_value))
        xr_channel_timer_dispose(xr_value_to_channel(ch_value));
}

XrAotResult xr_aot_select_block(const XrAotContext *ctx, const XrValue *channel_values,
                                int channel_count, int case_count) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrCoroBlockResult block =
        xr_coro_select_block(ctx->coro, channel_values, channel_count, NULL, case_count);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY || block.kind == XR_CORO_BLOCK_NO_CORO)
        return xr_aot_done(XR_NULL_VAL);
    return xr_aot_error(block.value, block.ok);
}

XrAotResult xr_aot_await_task(const XrAotContext *ctx, XrValue task_value, XrSlotRef out_slot,
                              int64_t timeout_ms, bool discard_result) {
    if (!ctx || !ctx->coro || !xr_value_is_task(task_value))
        return xr_aot_error(XR_NULL_VAL, false);
    XrTask *task = xr_value_to_task(task_value);
    XrCoroBlockResult block =
        xr_coro_await_task_slot(ctx->coro, task, out_slot, timeout_ms, discard_result);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY) {
        aot_detach_completed_executor(task);
        return xr_aot_result(XR_AOT_RUN_DONE);
    }
    if (block.kind == XR_CORO_BLOCK_CLOSED || block.kind == XR_CORO_BLOCK_NO_CORO) {
        aot_detach_completed_executor(task);
        return aot_task_terminal_error(ctx, task);
    }
    if (block.kind == XR_CORO_BLOCK_TIMEOUT)
        return aot_store_slot_result(out_slot, XR_NULL_VAL);
    return xr_aot_error(XR_NULL_VAL, false);
}

static XrTask *aot_resume_await_task(XrCoroWaitState *wait) {
    if (!wait)
        return NULL;
    XrTask *task = atomic_load_explicit(&wait->await_task, memory_order_acquire);
    if (task)
        return task;
    /* A wake/unregister race can clear await_task before AOT resumes; a
     * resolved token still records the task that published the result. */
    int state = atomic_load_explicit(&wait->await_token.state, memory_order_acquire);
    if (state == XR_AWAIT_WAIT_RESOLVED)
        return atomic_load_explicit(&wait->await_token.task, memory_order_acquire);
    return NULL;
}

XrAotResult xr_aot_await_task_resume(const XrAotContext *ctx, XrSlotRef out_slot,
                                     bool discard_result) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);
    XrCoroWaitState *wait = xr_coro_wait_state(ctx->coro);
    XrTask *task = aot_resume_await_task(wait);
    if (!task)
        return xr_aot_error(XR_NULL_VAL, false);
    XrCoroBlockResult block =
        xr_coro_await_task_resume_slot(ctx->coro, task, out_slot, discard_result);
    if (block.kind == XR_CORO_BLOCK_NOT_RESUMED)
        block = xr_coro_await_task_slot(ctx->coro, task, out_slot, -1, discard_result);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY) {
        aot_detach_completed_executor(task);
        return xr_aot_result(XR_AOT_RUN_DONE);
    }
    if (block.kind == XR_CORO_BLOCK_CLOSED || block.kind == XR_CORO_BLOCK_NO_CORO) {
        aot_detach_completed_executor(task);
        return aot_task_terminal_error(ctx, task);
    }
    if (block.kind == XR_CORO_BLOCK_TIMEOUT)
        return xr_aot_result(XR_AOT_RUN_DONE);
    return xr_aot_error(XR_NULL_VAL, false);
}

XrValue xr_aot_task_cancel(const XrAotContext *ctx, XrValue task_value) {
    if (!xr_value_is_task(task_value))
        return xr_null();
    XrTask *task = xr_value_to_task(task_value);
    XrCoroutine *coro = xr_task_executor_peek(task);
    if (!xr_task_is_done(task) && coro) {
        uint32_t before_cancel_flags = xr_coro_flags_load(coro);
        xr_coro_cancel(coro);
        xr_task_cancel(task);
        xr_coro_wake_waiter_runtime(aot_context_scheduler(ctx), coro);
        aot_task_recycle_cancelled_executor(task, coro, before_cancel_flags);
    } else if (!xr_task_is_done(task)) {
        xr_task_cancel(task);
    }
    return xr_null();
}

XrValue xr_aot_task_done(const XrAotContext *ctx, XrValue task_value) {
    (void) ctx;
    if (!xr_value_is_task(task_value))
        return xr_bool(false);
    return xr_bool(xr_task_is_done(xr_value_to_task(task_value)));
}

XrValue xr_aot_task_status(const XrAotContext *ctx, XrValue task_value) {
    if (!xr_value_is_task(task_value))
        return aot_task_status_from_state(ctx, XR_TASK_CANCELLED);
    XrTask *task = xr_value_to_task(task_value);
    uint8_t state = atomic_load_explicit(&task->state, memory_order_acquire);
    return aot_task_status_from_state(ctx, state);
}

XrValue xr_aot_task_poll(const XrAotContext *ctx, XrValue task_value) {
    if (!xr_value_is_task(task_value))
        return aot_task_result_member(ctx, 4);
    XrTask *task = xr_value_to_task(task_value);
    return xr_task_is_done(task) ? aot_task_result_from_terminal(ctx, task)
                                 : aot_task_result_member(ctx, 4);
}

XrAotResult xr_aot_task_await_result(const XrAotContext *ctx, XrValue task_value,
                                     XrSlotRef result_slot, int64_t timeout_ms,
                                     bool timeout_enabled) {
    if (!ctx || !ctx->coro || !xr_value_is_task(task_value))
        return xr_aot_error(XR_NULL_VAL, false);

    if (!timeout_enabled)
        timeout_ms = -1;
    else if (timeout_ms < 0)
        timeout_ms = 0;

    XrTask *task = xr_value_to_task(task_value);
    XrValue raw_value = XR_NULL_VAL;
    XrSlotRef raw_slot = xr_slot_xvalue_ptr(&raw_value);
    XrCoroBlockResult block = xr_coro_await_task_slot(ctx->coro, task, raw_slot, timeout_ms, false);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_ERROR)
        return xr_aot_error(XR_NULL_VAL, false);

    if (block.kind == XR_CORO_BLOCK_READY)
        aot_detach_completed_executor(task);

    XrValue result = aot_task_result_from_block(ctx, task, block, raw_value, timeout_enabled);
    return aot_store_slot_result(result_slot, result);
}

XrAotResult xr_aot_task_await_result_resume(const XrAotContext *ctx, XrSlotRef result_slot,
                                            bool timeout_enabled) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrCoroWaitState *wait = xr_coro_wait_state(ctx->coro);
    XrTask *task = aot_resume_await_task(wait);
    if (!task)
        return xr_aot_error(XR_NULL_VAL, false);

    XrValue raw_value = XR_NULL_VAL;
    XrSlotRef raw_slot = xr_slot_xvalue_ptr(&raw_value);
    XrCoroBlockResult block = xr_coro_await_task_resume_slot(ctx->coro, task, raw_slot, false);
    if (block.kind == XR_CORO_BLOCK_NOT_RESUMED)
        block = xr_coro_await_task_slot(ctx->coro, task, raw_slot, -1, false);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_ERROR)
        return xr_aot_error(XR_NULL_VAL, false);

    if (block.kind == XR_CORO_BLOCK_READY)
        aot_detach_completed_executor(task);

    XrValue result = aot_task_result_from_block(ctx, task, block, raw_value, timeout_enabled);
    return aot_store_slot_result(result_slot, result);
}

XrAotResult xr_aot_await_all_tasks(const XrAotContext *ctx, XrValue tasks_value,
                                   XrSlotRef out_slot) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrArray *tasks = aot_tasks_array_from_value(ctx, tasks_value);
    if (!tasks)
        return xr_aot_error(XR_NULL_VAL, false);

    if (aot_all_tasks_done(tasks))
        return aot_await_all_ready_result(ctx, tasks, out_slot);

    XrCoroBlockResult block = xr_coro_await_all_tasks(ctx->coro, tasks);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY)
        return aot_await_all_ready_result(ctx, tasks, out_slot);
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_await_all_tasks_resume(const XrAotContext *ctx, XrValue tasks_value,
                                          XrSlotRef out_slot) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrArray *tasks = aot_tasks_array_from_value(ctx, tasks_value);
    if (!tasks)
        return xr_aot_error(XR_NULL_VAL, false);
    if (aot_all_tasks_done(tasks))
        return aot_await_all_ready_result(ctx, tasks, out_slot);

    XrCoroBlockResult block = xr_coro_await_all_tasks(ctx->coro, tasks);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY)
        return aot_await_all_ready_result(ctx, tasks, out_slot);
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_await_any_task(const XrAotContext *ctx, XrValue tasks_value, XrSlotRef out_slot,
                                  bool success_only) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrArray *tasks = aot_tasks_array_from_value(ctx, tasks_value);
    if (!tasks)
        return xr_aot_error(XR_NULL_VAL, false);

    bool found_pending = false;
    XrAotResult ready =
        aot_await_any_ready_result(ctx, tasks, out_slot, success_only, &found_pending);
    if (ready.kind != XR_AOT_RUN_BLOCKED)
        return ready;
    if (!found_pending)
        return aot_store_slot_result(out_slot, XR_NULL_VAL);

    XrCoroBlockResult block = xr_coro_await_any_task(ctx->coro, tasks, success_only);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY)
        return aot_store_slot_result(out_slot, block.value);
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_await_any_task_resume(const XrAotContext *ctx, XrSlotRef out_slot) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);
    xr_task_finish_await_waiters(ctx->coro);
    return aot_store_slot_result(out_slot, ctx->coro->result);
}

static XrRuntimeCore *aot_context_runtime_core(const XrAotContext *ctx) {
    if (ctx && ctx->runtime)
        return xr_aot_runtime_core(ctx->runtime);
    XrAotVmHost vm_host = aot_context_vm_host(ctx, NULL);
    if (aot_vm_host_available(vm_host))
        return aot_vm_host_runtime_core(vm_host);
    if (ctx && ctx->coro)
        return ctx->coro->core;
    return NULL;
}

static XrRuntime *aot_context_scheduler(const XrAotContext *ctx) {
    if (ctx && ctx->runtime)
        return xr_aot_runtime_scheduler(ctx->runtime);
    XrAotVmHost vm_host = aot_context_vm_host(ctx, NULL);
    if (aot_vm_host_available(vm_host))
        return aot_vm_host_scheduler(vm_host);
    if (ctx && ctx->coro)
        return (XrRuntime *) ctx->coro->scheduler;
    return NULL;
}

XrValue xr_aot_channel_new(const XrAotContext *ctx, int64_t buffer_size) {
    if (!ctx)
        return XR_NULL_VAL;
    if (buffer_size < 0)
        buffer_size = 0;
    XrAotVmHost vm_host = aot_context_vm_host(ctx, NULL);
    XrChannel *ch =
        ctx->vm_host && !ctx->runtime && aot_vm_host_available(vm_host) && vm_host.ops->new_channel
            ? vm_host.ops->new_channel(vm_host.host, (uint32_t) buffer_size)
            : xr_channel_new(aot_context_runtime_core(ctx), aot_context_scheduler(ctx),
                             (uint32_t) buffer_size);
    return ch ? xr_value_from_channel(ch) : XR_NULL_VAL;
}

XrValue xr_aot_chan_try_send(const XrAotContext *ctx, XrValue channel_value, XrValue send_value) {
    if (!ctx || !xr_value_is_channel(channel_value))
        return aot_send_result(ctx, 3);
    XrChannel *ch = xr_value_to_channel(channel_value);
    if (xr_channel_is_closed(ch))
        return aot_send_result(ctx, 3);
    XrRuntimeCore *core = aot_context_runtime_core(ctx);
    if (!core)
        core = ch->core;
    return aot_send_result(ctx, xr_chan_try_send_core(core, ch, send_value) ? 0 : 1);
}

XrValue xr_aot_chan_try_send_ready(const XrAotContext *ctx, XrValue channel_value,
                                   XrValue send_value) {
    if (!ctx || !xr_value_is_channel(channel_value))
        return xr_bool(false);
    XrChannel *ch = xr_value_to_channel(channel_value);
    if (xr_channel_is_closed(ch))
        return xr_bool(false);
    XrRuntimeCore *core = aot_context_runtime_core(ctx);
    if (!core)
        core = ch->core;
    return xr_bool(xr_chan_try_send_core(core, ch, send_value));
}

static bool aot_context_from_channel_value(XrValue channel_value, XrAotContext *out) {
    if (!out || !xr_value_is_channel(channel_value))
        return false;
    XrChannel *ch = xr_value_to_channel(channel_value);
    out->runtime = NULL;
    out->vm_host_ops = NULL;
    out->vm_host = ch ? ch->vm_host_isolate : NULL;
    out->coro = NULL;
    out->worker = NULL;
    return true;
}

XrValue xr_aot_chan_try_send_sync(XrValue channel_value, XrValue send_value) {
    XrAotContext ctx = {0};
    if (!aot_context_from_channel_value(channel_value, &ctx))
        return XR_NULL_VAL;
    return xr_aot_chan_try_send(&ctx, channel_value, send_value);
}

XrValue xr_aot_chan_try_recv(const XrAotContext *ctx, XrValue channel_value) {
    if (!ctx || !xr_value_is_channel(channel_value))
        return aot_recv_closed(ctx);

    XrChannel *ch = xr_value_to_channel(channel_value);
    XrValue recv_value = XR_NULL_VAL;
    XrRuntimeCore *core = aot_context_runtime_core(ctx);
    if (!core)
        core = ch->core;
    bool ok = xr_chan_try_recv_core(core, ch, &recv_value, ctx->coro);
    if (ok)
        return aot_recv_value(ctx, recv_value);
    return xr_channel_is_closed(ch) ? aot_recv_closed(ctx) : aot_recv_empty(ctx);
}

XrValue xr_aot_chan_try_recv_sync(XrValue channel_value) {
    XrAotContext ctx = {0};
    if (!aot_context_from_channel_value(channel_value, &ctx))
        return XR_NULL_VAL;
    return xr_aot_chan_try_recv(&ctx, channel_value);
}

XrAotResult xr_aot_poll_yield(const XrAotContext *ctx) {
    if (!ctx || !ctx->coro)
        return xr_aot_done(XR_NULL_VAL);
    if (xr_coro_consume_reds(ctx->coro, 1) > 0)
        return xr_aot_done(XR_NULL_VAL);
    if (xr_coro_flags_has(ctx->coro, XR_CORO_FLG_CANCEL_REQUESTED | XR_CORO_FLG_CANCELLED))
        return xr_aot_result(XR_AOT_RUN_CANCELLED);
    xr_coro_set_reds(ctx->coro, XR_CORO_REDUCTIONS);
    return xr_aot_yielded();
}

bool xr_aot_send_is_sent(XrValue send_value) {
    if (!XR_IS_INSTANCE(send_value))
        return false;
    XrInstance *inst = xr_value_to_instance(send_value);
    if (!inst || !inst->klass)
        return false;
    if (inst->klass->builtin_kind == XR_BK_ENUM_VALUE) {
        XrEnumValue *variant = (XrEnumValue *) inst;
        return variant->parent_type && variant->parent_type->name &&
               strcmp(variant->parent_type->name, "SendResult") == 0 && variant->member_index == 0;
    }
    if (inst->klass->builtin_kind != XR_BK_ADT_ENUM)
        return false;
    XrValue tag = inst->fields[0];
    if (!XR_IS_INSTANCE(tag))
        return false;
    XrEnumValue *variant = (XrEnumValue *) XR_TO_INSTANCE(tag);
    return variant->parent_type && variant->parent_type->name &&
           strcmp(variant->parent_type->name, "SendResult") == 0 && variant->member_index == 0;
}

bool xr_aot_recv_is_value(XrValue recv_value) {
    if (!XR_IS_INSTANCE(recv_value))
        return false;
    XrInstance *inst = xr_value_to_instance(recv_value);
    if (!inst || !inst->klass)
        return false;
    if (inst->klass->builtin_kind == XR_BK_ENUM_VALUE) {
        XrEnumValue *variant = (XrEnumValue *) inst;
        return variant->parent_type && variant->parent_type->name &&
               strcmp(variant->parent_type->name, "Recv") == 0 && variant->member_index == 0;
    }
    if (inst->klass->builtin_kind != XR_BK_ADT_ENUM)
        return false;
    XrValue tag = inst->fields[0];
    if (!XR_IS_INSTANCE(tag))
        return false;
    XrEnumValue *variant = (XrEnumValue *) XR_TO_INSTANCE(tag);
    return variant->parent_type && variant->parent_type->name &&
           strcmp(variant->parent_type->name, "Recv") == 0 && variant->member_index == 0;
}

XrValue xr_aot_recv_payload(XrValue recv_value) {
    return xr_aot_recv_is_value(recv_value) ? xr_aot_runtime_adt_payload(recv_value, 0)
                                            : XR_NULL_VAL;
}

XrValue xr_aot_chan_close(const XrAotContext *ctx, XrValue channel_value) {
    (void) ctx;
    if (!xr_value_is_channel(channel_value))
        return XR_NULL_VAL;
    XrChannel *ch = xr_value_to_channel(channel_value);
    xr_channel_close(ch);
    return XR_NULL_VAL;
}

XrValue xr_aot_chan_close_sync(XrValue channel_value) {
    XrAotContext ctx = {0};
    if (!aot_context_from_channel_value(channel_value, &ctx))
        return XR_NULL_VAL;
    return xr_aot_chan_close(&ctx, channel_value);
}

XrValue xr_aot_chan_length(const XrAotContext *ctx, XrValue channel_value) {
    (void) ctx;
    if (!xr_value_is_channel(channel_value))
        return XR_FROM_INT(0);
    return XR_FROM_INT((int64_t) xr_value_to_channel(channel_value)->buf_count);
}

XrValue xr_aot_chan_capacity(const XrAotContext *ctx, XrValue channel_value) {
    (void) ctx;
    if (!xr_value_is_channel(channel_value))
        return XR_FROM_INT(0);
    return XR_FROM_INT((int64_t) xr_value_to_channel(channel_value)->buf_size);
}

XrValue xr_aot_chan_is_closed(const XrAotContext *ctx, XrValue channel_value) {
    if (!ctx || !xr_value_is_channel(channel_value))
        return xr_bool(false);
    return xr_bool(xr_channel_is_closed(xr_value_to_channel(channel_value)));
}

XrValue xr_aot_chan_is_closed_sync(XrValue channel_value) {
    if (!xr_value_is_channel(channel_value))
        return xr_bool(false);
    return xr_bool(xr_channel_is_closed(xr_value_to_channel(channel_value)));
}

static uint32_t aot_work_queue_sanitize_shards(int64_t value) {
    if (value <= 0)
        return XR_WORK_QUEUE_DEFAULT_SHARDS;
    if (value > XR_WORK_QUEUE_MAX_SHARDS)
        return XR_WORK_QUEUE_MAX_SHARDS;
    return (uint32_t) value;
}

static uint32_t aot_work_queue_sanitize_capacity(int64_t value) {
    if (value <= 0)
        return XR_WORK_QUEUE_DEFAULT_CAPACITY;
    if (value > XR_WORK_QUEUE_MAX_CAPACITY)
        return XR_WORK_QUEUE_MAX_CAPACITY;
    return (uint32_t) value;
}

XrValue xr_aot_work_queue_new(const XrAotContext *ctx, int64_t shard_count,
                              int64_t shard_capacity) {
    if (!ctx)
        return XR_NULL_VAL;
    XrRuntimeCore *core = aot_context_runtime_core(ctx);
    XrRuntime *scheduler = aot_context_scheduler(ctx);
    XrWorkQueue *q = xr_work_queue_new(core, scheduler, aot_work_queue_sanitize_shards(shard_count),
                                       aot_work_queue_sanitize_capacity(shard_capacity));
    return q ? xr_value_from_work_queue(q) : XR_NULL_VAL;
}

XrValue xr_aot_work_queue_push(const XrAotContext *ctx, XrValue queue_value, XrValue value,
                               int64_t shard_hint) {
    if (!xr_value_is_work_queue(queue_value))
        return xr_bool(false);
    XrWorkQueue *q = xr_value_to_work_queue(queue_value);
    XrRuntimeCore *core = aot_context_runtime_core(ctx);
    if (!core)
        core = q->core;
    return xr_bool(xr_work_queue_push_core(core, q, value, shard_hint));
}

XrValue xr_aot_work_queue_push_sync(XrValue queue_value, XrValue value, int64_t shard_hint) {
    if (!xr_value_is_work_queue(queue_value))
        return xr_bool(false);
    XrWorkQueue *q = xr_value_to_work_queue(queue_value);
    return xr_bool(xr_work_queue_push_core(q ? q->core : NULL, q, value, shard_hint));
}

bool xr_aot_work_queue_try_pop(const XrAotContext *ctx, XrValue queue_value, int64_t worker_hint,
                               XrValue *out_value) {
    if (out_value)
        *out_value = XR_NULL_VAL;
    if (!xr_value_is_work_queue(queue_value))
        return false;
    XrWorkQueue *q = xr_value_to_work_queue(queue_value);
    XrRuntimeCore *core = aot_context_runtime_core(ctx);
    if (!core)
        core = q->core;
    XrCoroutine *coro = ctx && ctx->coro ? ctx->coro : NULL;
    if (!core)
        return false;

    bool ok = false;
    XrValue value = xr_work_queue_try_pop_for_coro_core(core, q, worker_hint, coro, &ok);
    if (ok && out_value)
        *out_value = value;
    return ok;
}

bool xr_aot_work_queue_try_pop_sync(XrValue queue_value, int64_t worker_hint, XrValue *out_value) {
    if (out_value)
        *out_value = XR_NULL_VAL;
    if (!xr_value_is_work_queue(queue_value))
        return false;
    XrWorkQueue *q = xr_value_to_work_queue(queue_value);
    bool ok = false;
    XrValue value =
        xr_work_queue_try_pop_for_coro_core(q ? q->core : NULL, q, worker_hint, NULL, &ok);
    if (ok && out_value)
        *out_value = value;
    return ok;
}

XrAotResult xr_aot_work_queue_pop(const XrAotContext *ctx, XrValue queue_value, int64_t worker_hint,
                                  XrSlotRef out_slot) {
    if (!ctx || !ctx->coro || !xr_value_is_work_queue(queue_value))
        return xr_aot_error(XR_NULL_VAL, false);
    XrValue value = XR_NULL_VAL;
    XrWorkQueue *q = xr_value_to_work_queue(queue_value);
    XrRuntimeCore *core = aot_context_runtime_core(ctx);
    if (!core)
        core = q->core;
    switch (xr_work_queue_pop_for_coro_core(core, q, ctx->coro, worker_hint, &value)) {
        case XR_WORK_QUEUE_POP_DONE:
            if (out_slot.kind == XR_SLOT_NONE)
                return xr_aot_done(value);
            return aot_store_slot_result(out_slot, value);
        case XR_WORK_QUEUE_POP_BLOCKED:
            return xr_aot_blocked();
        case XR_WORK_QUEUE_POP_WOULD_BLOCK:
        case XR_WORK_QUEUE_POP_ERROR:
        default:
            return xr_aot_error(XR_NULL_VAL, false);
    }
}

XrAotResult xr_aot_work_queue_pop_resume(const XrAotContext *ctx, XrSlotRef out_slot) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);
    XrValue value = XR_NULL_VAL;
    XrRuntimeCore *core = aot_context_runtime_core(ctx);
    if (!core)
        core = ctx->coro->core;
    switch (xr_work_queue_pop_resume_for_coro_core(core, ctx->coro, &value)) {
        case XR_WORK_QUEUE_POP_DONE:
            if (out_slot.kind == XR_SLOT_NONE)
                return xr_aot_done(value);
            return aot_store_slot_result(out_slot, value);
        case XR_WORK_QUEUE_POP_BLOCKED:
            return xr_aot_blocked();
        case XR_WORK_QUEUE_POP_WOULD_BLOCK:
        case XR_WORK_QUEUE_POP_ERROR:
        default:
            return xr_aot_error(XR_NULL_VAL, false);
    }
}

XrValue xr_aot_work_queue_close(const XrAotContext *ctx, XrValue queue_value) {
    (void) ctx;
    if (!xr_value_is_work_queue(queue_value))
        return XR_NULL_VAL;
    xr_work_queue_close(xr_value_to_work_queue(queue_value));
    return XR_NULL_VAL;
}

XrValue xr_aot_work_queue_close_sync(XrValue queue_value) {
    if (!xr_value_is_work_queue(queue_value))
        return XR_NULL_VAL;
    xr_work_queue_close(xr_value_to_work_queue(queue_value));
    return XR_NULL_VAL;
}

XrValue xr_aot_work_queue_length(const XrAotContext *ctx, XrValue queue_value) {
    (void) ctx;
    if (!xr_value_is_work_queue(queue_value))
        return XR_FROM_INT(0);
    return XR_FROM_INT((int64_t) xr_work_queue_length(xr_value_to_work_queue(queue_value)));
}

XrValue xr_aot_work_queue_shard_count(const XrAotContext *ctx, XrValue queue_value) {
    (void) ctx;
    if (!xr_value_is_work_queue(queue_value))
        return XR_FROM_INT(0);
    return XR_FROM_INT((int64_t) xr_value_to_work_queue(queue_value)->shard_count);
}

XrValue xr_aot_work_queue_is_closed(const XrAotContext *ctx, XrValue queue_value) {
    (void) ctx;
    if (!xr_value_is_work_queue(queue_value))
        return xr_bool(false);
    return xr_bool(xr_work_queue_is_closed(xr_value_to_work_queue(queue_value)));
}

XrValue xr_aot_work_queue_is_closed_sync(XrValue queue_value) {
    if (!xr_value_is_work_queue(queue_value))
        return xr_bool(false);
    return xr_bool(xr_work_queue_is_closed(xr_value_to_work_queue(queue_value)));
}

static bool aot_context_from_result_group_value(XrValue group_value, XrAotContext *out) {
    if (!out || !xr_value_is_result_group(group_value))
        return false;
    XrResultGroup *g = xr_value_to_result_group(group_value);
    if (!g)
        return false;
    out->runtime = NULL;
    out->vm_host_ops = NULL;
    out->vm_host = NULL;
    out->coro = NULL;
    out->worker = NULL;
    return true;
}

static uint32_t aot_result_group_sanitize_batch(int64_t value) {
    if (value <= 0)
        return XR_RESULT_GROUP_DEFAULT_BATCH;
    if (value > XR_RESULT_GROUP_MAX_BATCH)
        return XR_RESULT_GROUP_MAX_BATCH;
    return (uint32_t) value;
}

XrValue xr_aot_result_group_new(const XrAotContext *ctx, int64_t batch_size) {
    if (!ctx)
        return XR_NULL_VAL;
    XrRuntimeCore *core = aot_context_runtime_core(ctx);
    XrRuntime *scheduler = aot_context_scheduler(ctx);
    XrResultGroup *g =
        xr_result_group_new(core, scheduler, aot_result_group_sanitize_batch(batch_size));
    return g ? xr_value_from_result_group(g) : XR_NULL_VAL;
}

XrValue xr_aot_result_group_add(const XrAotContext *ctx, XrValue group_value, int64_t value) {
    (void) ctx;
    if (!xr_value_is_result_group(group_value))
        return xr_bool(false);
    return xr_bool(xr_result_group_add(xr_value_to_result_group(group_value), value));
}

XrValue xr_aot_result_group_add_sync(XrValue group_value, int64_t value) {
    XrAotContext ctx = {0};
    if (!aot_context_from_result_group_value(group_value, &ctx))
        return xr_bool(false);
    return xr_aot_result_group_add(&ctx, group_value, value);
}

XrValue xr_aot_result_group_flush(const XrAotContext *ctx, XrValue group_value) {
    (void) ctx;
    if (!xr_value_is_result_group(group_value))
        return XR_NULL_VAL;
    xr_result_group_flush(xr_value_to_result_group(group_value));
    return XR_NULL_VAL;
}

XrValue xr_aot_result_group_flush_sync(XrValue group_value) {
    XrAotContext ctx = {0};
    if (!aot_context_from_result_group_value(group_value, &ctx))
        return XR_NULL_VAL;
    return xr_aot_result_group_flush(&ctx, group_value);
}

bool xr_aot_result_group_try_recv(const XrAotContext *ctx, XrValue group_value,
                                  XrValue *out_value) {
    (void) ctx;
    if (out_value)
        *out_value = XR_NULL_VAL;
    if (!xr_value_is_result_group(group_value))
        return false;
    XrResultGroup *g = xr_value_to_result_group(group_value);
    int64_t value = 0;
    bool ok = g && xr_result_group_try_recv(g, &value);
    if (ok && out_value)
        *out_value = XR_FROM_INT(value);
    return ok;
}

bool xr_aot_result_group_try_recv_sync(XrValue group_value, XrValue *out_value) {
    if (out_value)
        *out_value = XR_NULL_VAL;
    XrAotContext ctx = {0};
    if (!aot_context_from_result_group_value(group_value, &ctx))
        return false;
    return xr_aot_result_group_try_recv(&ctx, group_value, out_value);
}

XrAotResult xr_aot_result_group_recv(const XrAotContext *ctx, XrValue group_value,
                                     XrSlotRef out_slot) {
    if (!ctx || !ctx->coro || !xr_value_is_result_group(group_value))
        return xr_aot_error(XR_NULL_VAL, false);
    XrValue value = XR_NULL_VAL;
    XrResultGroup *g = xr_value_to_result_group(group_value);
    switch (xr_result_group_recv_for_coro(g, ctx->coro, &value)) {
        case XR_RESULT_GROUP_RECV_DONE:
            if (out_slot.kind == XR_SLOT_NONE)
                return xr_aot_done(value);
            return aot_store_slot_result(out_slot, value);
        case XR_RESULT_GROUP_RECV_BLOCKED:
            return xr_aot_blocked();
        case XR_RESULT_GROUP_RECV_ERROR:
        default:
            return xr_aot_error(XR_NULL_VAL, false);
    }
}

XrAotResult xr_aot_result_group_recv_resume(const XrAotContext *ctx, XrSlotRef out_slot) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);
    XrValue value = XR_NULL_VAL;
    switch (xr_result_group_recv_resume_for_coro(ctx->coro, &value)) {
        case XR_RESULT_GROUP_RECV_DONE:
            if (out_slot.kind == XR_SLOT_NONE)
                return xr_aot_done(value);
            return aot_store_slot_result(out_slot, value);
        case XR_RESULT_GROUP_RECV_BLOCKED:
            return xr_aot_blocked();
        case XR_RESULT_GROUP_RECV_ERROR:
        default:
            return xr_aot_error(XR_NULL_VAL, false);
    }
}

XrValue xr_aot_result_group_close(const XrAotContext *ctx, XrValue group_value) {
    (void) ctx;
    if (!xr_value_is_result_group(group_value))
        return XR_NULL_VAL;
    xr_result_group_close(xr_value_to_result_group(group_value));
    return XR_NULL_VAL;
}

XrValue xr_aot_result_group_close_sync(XrValue group_value) {
    XrAotContext ctx = {0};
    if (!aot_context_from_result_group_value(group_value, &ctx))
        return XR_NULL_VAL;
    return xr_aot_result_group_close(&ctx, group_value);
}

XrValue xr_aot_result_group_length(const XrAotContext *ctx, XrValue group_value) {
    (void) ctx;
    if (!xr_value_is_result_group(group_value))
        return XR_FROM_INT(0);
    return XR_FROM_INT((int64_t) xr_result_group_length(xr_value_to_result_group(group_value)));
}

XrValue xr_aot_result_group_pending_count(const XrAotContext *ctx, XrValue group_value) {
    (void) ctx;
    if (!xr_value_is_result_group(group_value))
        return XR_FROM_INT(0);
    return XR_FROM_INT(
        (int64_t) xr_result_group_pending_count(xr_value_to_result_group(group_value)));
}

XrValue xr_aot_result_group_batch_size(const XrAotContext *ctx, XrValue group_value) {
    (void) ctx;
    if (!xr_value_is_result_group(group_value))
        return XR_FROM_INT(0);
    XrResultGroup *g = xr_value_to_result_group(group_value);
    return XR_FROM_INT(g ? (int64_t) g->batch_size : 0);
}

XrValue xr_aot_result_group_is_closed(const XrAotContext *ctx, XrValue group_value) {
    (void) ctx;
    if (!xr_value_is_result_group(group_value))
        return xr_bool(true);
    return xr_bool(xr_result_group_is_closed(xr_value_to_result_group(group_value)));
}

XrValue xr_aot_result_group_is_closed_sync(XrValue group_value) {
    XrAotContext ctx = {0};
    if (!aot_context_from_result_group_value(group_value, &ctx))
        return xr_bool(true);
    return xr_aot_result_group_is_closed(&ctx, group_value);
}

XrValue xr_aot_tuple_get(const XrAotContext *ctx, XrValue tuple_value, uint16_t index) {
    (void) ctx;
    if (!xr_value_is_tuple(tuple_value))
        return XR_NULL_VAL;
    return xr_tuple_get(xr_value_to_tuple(tuple_value), index);
}

XrAotResult xr_aot_chan_send(const XrAotContext *ctx, XrValue channel_value, XrValue send_value,
                             XrSlotRef result_slot, int64_t timeout_ms) {
    if (!ctx || !ctx->coro || !xr_value_is_channel(channel_value))
        return xr_aot_error(XR_NULL_VAL, false);

    XrChannel *ch = xr_value_to_channel(channel_value);
    XrCoroBlockResult block = xr_coro_chan_send(ctx->coro, ch, send_value, result_slot, timeout_ms);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY || block.kind == XR_CORO_BLOCK_TIMEOUT ||
        block.kind == XR_CORO_BLOCK_CLOSED || block.kind == XR_CORO_BLOCK_NO_CORO) {
        if (timeout_ms >= 0) {
            XrValue result = aot_send_result_from_block(ctx, block);
            return aot_store_slot_result(result_slot, result);
        }
        if (block.kind == XR_CORO_BLOCK_READY)
            return xr_aot_done(XR_NULL_VAL);
        if (block.kind == XR_CORO_BLOCK_CLOSED || block.kind == XR_CORO_BLOCK_NO_CORO)
            return aot_channel_closed_error(ctx);
    }
    return xr_aot_error(XR_NULL_VAL, false);
}

static XrAotResult aot_chan_send_scalar(const XrAotContext *ctx, XrValue channel_value,
                                        XrValue send_value) {
    if (!ctx || !ctx->coro || !xr_value_is_channel(channel_value))
        return xr_aot_error(XR_NULL_VAL, false);

    XrChannel *ch = xr_value_to_channel(channel_value);
    XrChanResult chan_result = xr_channel_send(ch, send_value, ctx->coro, -1);
    if (chan_result == XR_CHAN_OK)
        return xr_aot_done(XR_NULL_VAL);
    if (chan_result == XR_CHAN_BLOCK)
        return xr_aot_blocked();
    if (chan_result == XR_CHAN_CLOSED || chan_result == XR_CHAN_NO_CORO)
        return aot_channel_closed_error(ctx);
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_chan_send_i64(const XrAotContext *ctx, XrValue channel_value,
                                 int64_t send_value) {
    return aot_chan_send_scalar(ctx, channel_value, XR_FROM_INT(send_value));
}

XrAotResult xr_aot_chan_send_f64(const XrAotContext *ctx, XrValue channel_value,
                                 double send_value) {
    return aot_chan_send_scalar(ctx, channel_value, XR_FROM_FLOAT(send_value));
}

XrAotResult xr_aot_chan_send_resume(const XrAotContext *ctx, XrSlotRef result_slot,
                                    bool result_value) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);
    XrCoroBlockResult block = xr_coro_chan_send_resume(ctx->coro, result_slot);
    if (block.kind == XR_CORO_BLOCK_READY || block.kind == XR_CORO_BLOCK_TIMEOUT ||
        block.kind == XR_CORO_BLOCK_CLOSED) {
        if (!result_value && block.kind == XR_CORO_BLOCK_READY)
            return xr_aot_done(XR_NULL_VAL);
        if (result_value) {
            XrValue result = aot_send_result_from_block(ctx, block);
            return aot_store_slot_result(result_slot, result);
        }
        if (block.kind == XR_CORO_BLOCK_CLOSED)
            return aot_channel_closed_error(ctx);
    }
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_chan_recv_slot(const XrAotContext *ctx, XrValue channel_value,
                                  XrSlotRef out_slot, int64_t timeout_ms) {
    if (!ctx || !ctx->coro || !xr_value_is_channel(channel_value))
        return xr_aot_error(XR_NULL_VAL, false);

    XrChannel *ch = xr_value_to_channel(channel_value);
    XrCoroBlockResult block =
        xr_coro_chan_recv(ctx->coro, ch, out_slot, xr_slot_none(), timeout_ms, false);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY || block.kind == XR_CORO_BLOCK_CLOSED ||
        block.kind == XR_CORO_BLOCK_TIMEOUT || block.kind == XR_CORO_BLOCK_NO_CORO) {
        XrValue result = aot_recv_result_from_block(ctx, block);
        return aot_store_slot_result(out_slot, result);
    }
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_chan_recv_slot_resume(const XrAotContext *ctx, XrSlotRef out_slot,
                                         bool result_value) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrSlotRef value_slot = out_slot;
    if (value_slot.kind == XR_SLOT_NONE)
        value_slot = ctx->coro->ext ? ctx->coro->ext->recv_slot_ref : xr_slot_none();
    XrCoroBlockResult block = xr_coro_chan_recv_resume(ctx->coro, value_slot, xr_slot_none());
    if (block.kind == XR_CORO_BLOCK_READY || block.kind == XR_CORO_BLOCK_CLOSED ||
        block.kind == XR_CORO_BLOCK_TIMEOUT || block.kind == XR_CORO_BLOCK_NO_CORO) {
        if (!result_value)
            return xr_aot_done(block.value);
        XrSlotRef result_slot = out_slot.kind == XR_SLOT_NONE ? value_slot : out_slot;
        XrValue result = aot_recv_result_from_block(ctx, block);
        return aot_store_slot_result(result_slot, result);
    }
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_chan_recv_pair(const XrAotContext *ctx, XrValue channel_value,
                                  XrSlotRef value_slot, XrSlotRef ok_slot, int64_t timeout_ms) {
    if (!ctx || !ctx->coro || !xr_value_is_channel(channel_value))
        return xr_aot_error(XR_NULL_VAL, false);

    XrChannel *ch = xr_value_to_channel(channel_value);
    XrCoroBlockResult block =
        xr_coro_chan_recv(ctx->coro, ch, value_slot, ok_slot, timeout_ms, true);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY || block.kind == XR_CORO_BLOCK_CLOSED ||
        block.kind == XR_CORO_BLOCK_TIMEOUT || block.kind == XR_CORO_BLOCK_NO_CORO)
        return xr_aot_done(block.value);
    return xr_aot_error(XR_NULL_VAL, false);
}

static XrAotResult aot_chan_recv_pair_scalar(const XrAotContext *ctx, XrValue channel_value,
                                             XrSlotRef value_slot, XrSlotRef ok_slot,
                                             uint16_t scalar_rep) {
    if (!ctx || !ctx->coro || !xr_value_is_channel(channel_value))
        return xr_aot_error(XR_NULL_VAL, false);

    XrChannel *ch = xr_value_to_channel(channel_value);
    XrValue recv_val;
    XrChanResult chan_result =
        xr_channel_recv_slot(ch, &recv_val, ctx->coro, -1, value_slot, ok_slot, true);
    if (chan_result == XR_CHAN_OK) {
        bool stored = scalar_rep == XR_REP_F64
                          ? aot_slot_store_f64_fast(value_slot, XR_TO_FLOAT(recv_val))
                          : aot_slot_store_i64_fast(value_slot, XR_TO_INT(recv_val));
        if (!stored || !aot_slot_store_bool_fast(ok_slot, true))
            return xr_aot_error(XR_NULL_VAL, false);
        return xr_aot_done(recv_val);
    }
    if (chan_result == XR_CHAN_CLOSED || chan_result == XR_CHAN_NO_CORO) {
        if (!aot_slot_store_scalar_null_fast(value_slot) ||
            !aot_slot_store_bool_fast(ok_slot, false))
            return xr_aot_error(XR_NULL_VAL, false);
        return xr_aot_done(xr_null());
    }
    if (chan_result == XR_CHAN_BLOCK)
        return xr_aot_blocked();
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_chan_recv_pair_i64(const XrAotContext *ctx, XrValue channel_value,
                                      XrSlotRef value_slot, XrSlotRef ok_slot) {
    return aot_chan_recv_pair_scalar(ctx, channel_value, value_slot, ok_slot, XR_REP_I64);
}

XrAotResult xr_aot_chan_recv_pair_f64(const XrAotContext *ctx, XrValue channel_value,
                                      XrSlotRef value_slot, XrSlotRef ok_slot) {
    return aot_chan_recv_pair_scalar(ctx, channel_value, value_slot, ok_slot, XR_REP_F64);
}

XrAotResult xr_aot_chan_recv_pair_resume(const XrAotContext *ctx, XrSlotRef value_slot,
                                         XrSlotRef ok_slot) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrCoroBlockResult block = xr_coro_chan_recv_resume(ctx->coro, value_slot, ok_slot);
    if (block.kind == XR_CORO_BLOCK_READY || block.kind == XR_CORO_BLOCK_CLOSED ||
        block.kind == XR_CORO_BLOCK_TIMEOUT || block.kind == XR_CORO_BLOCK_NO_CORO)
        return xr_aot_done(block.value);
    return xr_aot_error(XR_NULL_VAL, false);
}
