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
#include "xaot_await.h"
#include "xaot_task.h"

#include <stdatomic.h>
#include <stdint.h>
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
#include "../runtime/object/xstring.h"
#include "../runtime/object/xtuple.h"
#include "../runtime/xisolate_internal.h"
#include "../os/os_time.h"
#include "xblock.h"
#include "xchannel_ops.h"
#include "xcoroutine.h"
#include "xcoro_pool.h"
#include "xtask.h"
#include "xworker.h"
#include "xwork_queue.h"

typedef struct XrAotCoroState {
    const XrAotCoroDesc *desc;
    void *frame;
} XrAotCoroState;

enum {
    XR_AOT_VALUE_TAG_ARRAY = 15,
};

typedef struct XrAotArrayView {
    int64_t len;
    int64_t cap;
    void *data;
    uint8_t elem_type;
    uint8_t elem_size;
} XrAotArrayView;

static XrAotCoroState *aot_state_from_coro(XrCoroutine *coro) {
    if (!coro || !coro->backend_state)
        return NULL;
    return (XrAotCoroState *) coro->backend_state;
}

static void aot_release_frame(const XrAotCoroDesc *desc, void *frame, XrCoroGC *gc) {
    if (!frame)
        return;
    if (desc && desc->release_frame) {
        desc->release_frame(frame, gc);
        return;
    }
    xr_aot_frame_free(frame);
}

static void aot_release_state(XrCoroutine *coro) {
    XrAotCoroState *state = aot_state_from_coro(coro);
    if (!state)
        return;

    aot_release_frame(state->desc, state->frame, coro ? coro->coro_gc : NULL);
    xr_free(state);
    coro->backend_state = NULL;
    coro->backend = NULL;
}

static const char *aot_backend_debug_name(const XrCoroutine *coro) {
    const XrAotCoroState *state = coro ? (const XrAotCoroState *) coro->backend_state : NULL;
    if (state && state->desc && state->desc->name)
        return state->desc->name;
    return "aot";
}

static void aot_backend_debug_snapshot(const XrCoroutine *coro, XrCoroDebugSnapshot *snapshot) {
    if (!snapshot)
        return;
    snapshot->backend_name = "aot";
    snapshot->function_name = aot_backend_debug_name(coro);
    snapshot->frame_count = coro && coro->backend_state ? 1 : 0;
    snapshot->in_c_frame = 0;
}

static void aot_backend_trace_roots(XrCoroutine *coro, void *visitor) {
    XrAotCoroState *state = aot_state_from_coro(coro);
    if (state && state->desc && state->desc->trace_roots)
        state->desc->trace_roots(state->frame, visitor);
}

static void aot_mark_running(XrCoroutine *coro) {
    xr_coro_flags_swap(coro, XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED,
                       XR_CORO_FLG_RUNNING | XR_CORO_FLG_STARTED);
}

static XrCoroRunResult aot_map_result(XrCoroutine *coro, XrAotResult result) {
    switch (result.kind) {
        case XR_AOT_RUN_DONE:
            coro->result = result.value;
            return xr_coro_run_done(result.value);
        case XR_AOT_RUN_BLOCKED:
            (void) xr_coro_finalize_blocked_suspend(coro);
            return xr_coro_run_result(XR_CORO_RUN_BLOCKED);
        case XR_AOT_RUN_YIELD:
            return xr_coro_run_result(XR_CORO_RUN_YIELD);
        case XR_AOT_RUN_SPAWN_CHILD:
            return xr_coro_run_spawn_child(result.child);
        case XR_AOT_RUN_CANCELLED:
            return xr_coro_run_result(XR_CORO_RUN_CANCELLED);
        case XR_AOT_RUN_ERROR:
        default:
            return xr_coro_run_error(result.error, result.error_is_value);
    }
}

static XrCoroRunResult aot_backend_resume(XrCoroutine *coro, const XrCoroEvent *event,
                                          const XrCoroRunContext *run_ctx) {
    XrAotCoroState *state = aot_state_from_coro(coro);
    if (!coro || !state || !state->desc || !state->desc->resume)
        return xr_coro_run_error(XR_NULL_VAL, false);
    if (event && event->kind == XR_CORO_EVENT_CANCEL)
        return xr_coro_run_result(XR_CORO_RUN_CANCELLED);

    aot_mark_running(coro);

    XrAotContext ctx;
    ctx.coro = coro;
    ctx.isolate = run_ctx && run_ctx->isolate ? run_ctx->isolate : coro->isolate;
    ctx.worker = run_ctx ? (void *) run_ctx->worker : NULL;

    XrayIsolate *previous_isolate = xray_isolate_current();
    if (ctx.isolate)
        xray_isolate_enter(ctx.isolate);
    XrAotResult result = state->desc->resume(state->frame, &ctx);
    if (previous_isolate)
        xray_isolate_enter(previous_isolate);
    else
        xray_isolate_exit();
    return aot_map_result(coro, result);
}

static const XrCoroBackendVTable aot_backend_vtable = {
    .kind = XR_CORO_BACKEND_AOT,
    .resume = aot_backend_resume,
    .trace_roots = aot_backend_trace_roots,
    .prepare_recycle = NULL,
    .reset_reusable = NULL,
    .on_safepoint = NULL,
    .detach_worker_state = NULL,
    .is_try_mode = NULL,
    .setup_yield_continuation = NULL,
    .has_continuation = NULL,
    .call_closure = NULL,
    .ensure_state = NULL,
    .prepare_execution_state = NULL,
    .reset_execution_state = NULL,
    .clear_entry_state = NULL,
    .reset_entry_state_no_free = NULL,
    .bind_closure_entry = NULL,
    .bind_cfunc_entry = NULL,
    .release = aot_release_state,
    .destroy = aot_release_state,
    .debug_name = aot_backend_debug_name,
    .debug_snapshot = aot_backend_debug_snapshot,
};

void *xr_aot_frame_alloc(size_t size) {
    if (size == 0)
        return NULL;
    return xr_malloc(size);
}

void xr_aot_frame_free(void *frame) {
    xr_free(frame);
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
    if (!ctx || !ctx->isolate || index < 0 || index >= XR_GLOBALS_MAX)
        return XR_NULL_VAL;
    return ctx->isolate->vm.builtins[index];
}

XrValue xr_aot_load_builtin_field(const XrAotContext *ctx, int32_t index, const char *field) {
    if (!field)
        return XR_NULL_VAL;

    XrValue builtin = xr_aot_get_builtin(ctx, index);
    if (XR_IS_PTR(builtin) && XR_HEAP_TYPE(builtin) == XR_TINSTANCE && builtin.ptr) {
        XrGCHeader *gc = (XrGCHeader *) builtin.ptr;
        if (XR_GC_GET_TYPE(gc) == XR_TINSTANCE) {
            XrInstance *inst = xr_value_to_instance(builtin);
            if (inst && inst->klass && inst->klass->builtin_kind == XR_BK_ENUM_TYPE) {
                XrEnumType *enum_type = (XrEnumType *) inst;
                for (uint32_t i = 0; i < enum_type->member_count; i++) {
                    if (enum_type->members[i].name &&
                        strcmp(enum_type->members[i].name, field) == 0 &&
                        enum_type->members[i].instance) {
                        return XR_FROM_PTR(enum_type->members[i].instance);
                    }
                }
                return XR_NULL_VAL;
            }
        }
    }

    if (index != XR_GLOBAL_VAR_PROCESS)
        return XR_NULL_VAL;

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
    if (!XR_IS_PTR(value) || XR_HEAP_TYPE(value) != XR_TINSTANCE || !value.ptr)
        return false;
    XrGCHeader *gc = (XrGCHeader *) value.ptr;
    return XR_GC_GET_TYPE(gc) == XR_TINSTANCE;
}

bool xr_aot_runtime_enum_value_info(XrValue value, const char **enum_name, const char **member_name,
                                    uint32_t *member_index, bool *is_adt, int *payload_count) {
    if (!aot_value_is_runtime_instance(value))
        return false;
    XrInstance *inst = xr_value_to_instance(value);
    if (!inst || !inst->klass || inst->klass->builtin_kind != XR_BK_ENUM_VALUE)
        return false;

    XrEnumValue *enum_value = (XrEnumValue *) inst;
    XrEnumType *parent = enum_value->parent_type;
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
    return XR_FROM_INT((int64_t) (xr_time_monotonic_ns() / 1000000ULL));
}

XrValue xr_aot_time_nanos(void) {
    return XR_FROM_INT((int64_t) xr_time_monotonic_ns());
}

XrValue xr_aot_time_micros(void) {
    return XR_FROM_INT((int64_t) (xr_time_monotonic_ns() / 1000ULL));
}

XrValue xr_aot_time_clock(void) {
    return XR_FROM_INT((int64_t) (xr_time_process_cpu_ns() / 1000000ULL));
}

XrCoroutine *xr_coro_create_aot(XrayIsolate *X, const XrAotCoroDesc *desc, void *frame,
                                const char *name) {
    if (!X || !desc || !desc->resume || !frame) {
        aot_release_frame(desc, frame, NULL);
        return NULL;
    }

    XrAotCoroState *state = (XrAotCoroState *) xr_calloc(1, sizeof(XrAotCoroState));
    if (!state) {
        aot_release_frame(desc, frame, NULL);
        return NULL;
    }
    state->desc = desc;
    state->frame = frame;

    XrCoroutine *coro = xr_coro_create_empty(X, name ? name : desc->name);
    if (!coro) {
        aot_release_frame(desc, frame, NULL);
        xr_free(state);
        return NULL;
    }

    xr_coro_attach_backend(coro, &aot_backend_vtable, state);
    return coro;
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
        case XR_SLOT_JIT_SUSPEND:
            break;
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
        case XR_SLOT_JIT_SUSPEND:
            break;
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
        case XR_SLOT_JIT_SUSPEND:
            break;
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
        case XR_SLOT_JIT_SUSPEND:
            break;
    }
    return xr_slot_store_value(slot, XR_NULL_VAL);
}

static XrayIsolate *aot_context_isolate(const XrAotContext *ctx, const XrTask *task) {
    if (ctx && ctx->isolate)
        return ctx->isolate;
    XrayIsolate *isolate = xray_isolate_get_current();
    if (isolate)
        return isolate;
    if (task) {
        XrCoroutine *exec = xr_task_executor_peek(task);
        if (exec)
            return exec->isolate;
    }
    return NULL;
}

static XrCoroutine *aot_context_coro(const XrAotContext *ctx, XrayIsolate *isolate,
                                     const XrTask *task) {
    if (ctx && ctx->coro)
        return ctx->coro;
    XrCoroutine *coro = isolate ? xr_current_coro(isolate) : NULL;
    if (coro)
        return coro;
    return task ? xr_task_executor_peek(task) : NULL;
}

static XrEnumType *aot_builtin_enum_type(const XrAotContext *ctx, int builtin_index) {
    XrayIsolate *isolate = aot_context_isolate(ctx, NULL);
    if (!isolate || builtin_index < 0 || builtin_index >= XR_GLOBALS_MAX)
        return NULL;
    XrValue value = isolate->vm.builtins[builtin_index];
    if (!XR_IS_PTR(value))
        return NULL;
    return (XrEnumType *) XR_TO_PTR(value);
}

static XrValue aot_builtin_enum_member(const XrAotContext *ctx, int builtin_index,
                                       uint32_t member_index) {
    XrEnumType *type = aot_builtin_enum_type(ctx, builtin_index);
    if (!type || member_index >= type->member_count || !type->members[member_index].instance)
        return xr_null();
    return XR_FROM_PTR(type->members[member_index].instance);
}

static XrValue aot_builtin_adt_value(const XrAotContext *ctx, int builtin_index,
                                     uint32_t member_index, XrValue *args, int nargs) {
    XrEnumType *type = aot_builtin_enum_type(ctx, builtin_index);
    if (!type || !type->is_adt)
        return xr_null();
    XrInstance *inst =
        xr_enum_adt_construct(aot_context_isolate(ctx, NULL), type, member_index, args, nargs);
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
    XrayIsolate *isolate = aot_context_isolate(ctx, task);
    XrCoroutine *coro = aot_context_coro(ctx, isolate, task);
    XrValue error = task ? task->error : xr_null();
    if (XR_IS_NULL(error))
        return xr_exception_new(isolate, XR_ERR_RUNTIME, "Task failed");
    if (coro && isolate && xr_value_needs_copy(error))
        error = xr_deep_copy_to_coro(isolate, error, coro);
    if (isolate && !xr_value_is_exception(isolate, error))
        error = xr_exception_from_value(isolate, error);
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
        XrayIsolate *isolate = aot_context_isolate(ctx, task);
        XrCoroutine *coro = aot_context_coro(ctx, isolate, task);
        XrValue value = xr_coro_await_result_value(isolate, coro, task, false);
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
        error = xr_exception_new(aot_context_isolate(ctx, task), XR_ERR_CORO_CANCELLED,
                                 "Task cancelled");
    }
    return xr_aot_error(error, false);
}

static XrAotResult aot_channel_closed_error(const XrAotContext *ctx) {
    XrValue error =
        xr_exception_new(aot_context_isolate(ctx, NULL), XR_ERR_CORO_DEAD, "Channel is closed");
    return xr_aot_error(error, false);
}

static XrArray *aot_tasks_array_from_value(const XrAotContext *ctx, XrValue tasks_value) {
    if (!ctx || !ctx->coro)
        return NULL;
    if (xr_value_is_array(tasks_value))
        return xr_value_to_array(tasks_value);
    if (tasks_value.tag != XR_AOT_VALUE_TAG_ARRAY || !tasks_value.ptr)
        return NULL;

    XrAotArrayView *view = (XrAotArrayView *) tasks_value.ptr;
    if (view->len < 0 || view->len > INT32_MAX)
        return NULL;
    if (view->elem_type >= XR_ELEM_COUNT)
        return NULL;

    XrArray *tasks = xr_array_with_capacity(ctx->coro, (int) view->len);
    if (!tasks)
        return NULL;

    for (int64_t i = 0; i < view->len; i++) {
        XrValue item = XR_NULL_VAL;
        if (view->data)
            item = xr_typed_get(view->data, (int32_t) i, view->elem_type);
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
            value = xr_coro_await_result_value(ctx->isolate, ctx->coro, task, false);
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
            XrValue value = xr_coro_await_result_value(ctx->isolate, ctx->coro, task, false);
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

XrValue xr_aot_run_main(XrayIsolate *X, const XrAotCoroDesc *desc, void *frame) {
    XrCoroutine *main_coro = xr_coro_create_aot(X, desc, frame, desc ? desc->name : "main");
    if (!main_coro)
        return XR_NULL_VAL;
    xr_main_thread_run(X, main_coro);
    XrValue result = main_coro->result;
    xr_coro_destroy(main_coro);
    return result;
}

XrAotSpawnResult xr_aot_spawn(const XrAotContext *ctx, const XrAotCoroDesc *desc, void *frame,
                              int link_mode, bool fire_and_forget, const char *name) {
    XrAotSpawnResult result;
    result.task_value = XR_NULL_VAL;
    result.child = NULL;

    if (!ctx || !ctx->isolate || !desc || !frame)
        return result;

    XrCoroutine *child = xr_coro_create_aot(ctx->isolate, desc, frame, name);
    if (!child)
        return result;

    XrRuntime *runtime = (XrRuntime *) ctx->isolate->vm.runtime;
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

XrAotResult xr_aot_sleep(const XrAotContext *ctx, int64_t milliseconds) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);
    XrCoroBlockResult block = xr_coro_sleep(ctx->coro, milliseconds);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY)
        return xr_aot_result(XR_AOT_RUN_DONE);
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_scope_enter(const XrAotContext *ctx, uint8_t scope_mode) {
    if (!ctx || !ctx->isolate || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrCoroBlockResult block = xr_coro_scope_enter(ctx->isolate, ctx->coro, scope_mode);
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
    if (!ctx || !ctx->isolate)
        return XR_NULL_VAL;
    if (milliseconds < 0)
        milliseconds = 0;

    XrChannel *timer_ch = xr_channel_new_timer(ctx->isolate, milliseconds);
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
// shared with the VM/JIT backends.
void xr_aot_chan_timer_dispose(const XrAotContext *ctx, XrValue ch_value) {
    (void) ctx;
    if (xr_value_is_channel(ch_value))
        xr_channel_timer_dispose(xr_value_to_channel(ch_value));
}

XrAotResult xr_aot_select_block(const XrAotContext *ctx, const XrValue *channel_values,
                                int channel_count, int case_count) {
    if (!ctx || !ctx->isolate || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrCoroBlockResult block = xr_coro_select_block(ctx->isolate, ctx->coro, channel_values,
                                                   channel_count, NULL, case_count);
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
    XrCoroBlockResult block = xr_coro_await_task_slot(ctx->isolate, ctx->coro, task, out_slot,
                                                      timeout_ms, discard_result);
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

XrAotResult xr_aot_await_task_resume(const XrAotContext *ctx, XrSlotRef out_slot,
                                     bool discard_result) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);
    XrCoroWaitState *wait = xr_coro_wait_state(ctx->coro);
    XrTask *task = wait ? atomic_load_explicit(&wait->await_task, memory_order_acquire) : NULL;
    if (!task)
        return xr_aot_error(XR_NULL_VAL, false);
    XrCoroBlockResult block =
        xr_coro_await_task_resume_slot(ctx->isolate, ctx->coro, task, out_slot, discard_result);
    if (block.kind == XR_CORO_BLOCK_NOT_RESUMED)
        block =
            xr_coro_await_task_slot(ctx->isolate, ctx->coro, task, out_slot, -1, discard_result);
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
    if (coro && !xr_task_is_done(task)) {
        xr_coro_cancel(coro);
        xr_task_cancel(task);
        XrayIsolate *isolate = ctx ? ctx->isolate : NULL;
        if (!isolate)
            isolate = coro->isolate;
        if (isolate)
            xr_coro_wake_waiter(isolate, coro);
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
    XrCoroBlockResult block =
        xr_coro_await_task_slot(ctx->isolate, ctx->coro, task, result_slot, timeout_ms, false);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_ERROR)
        return xr_aot_error(XR_NULL_VAL, false);

    if (block.kind == XR_CORO_BLOCK_READY)
        aot_detach_completed_executor(task);

    XrValue result = aot_task_result_from_block(ctx, task, block, block.value, timeout_enabled);
    return aot_store_slot_result(result_slot, result);
}

XrAotResult xr_aot_task_await_result_resume(const XrAotContext *ctx, XrSlotRef result_slot,
                                            bool timeout_enabled) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrCoroWaitState *wait = xr_coro_wait_state(ctx->coro);
    XrTask *task = wait ? atomic_load_explicit(&wait->await_task, memory_order_acquire) : NULL;
    if (!task)
        return xr_aot_error(XR_NULL_VAL, false);

    XrCoroBlockResult block =
        xr_coro_await_task_resume_slot(ctx->isolate, ctx->coro, task, result_slot, false);
    if (block.kind == XR_CORO_BLOCK_NOT_RESUMED)
        block = xr_coro_await_task_slot(ctx->isolate, ctx->coro, task, result_slot, -1, false);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_ERROR)
        return xr_aot_error(XR_NULL_VAL, false);

    if (block.kind == XR_CORO_BLOCK_READY)
        aot_detach_completed_executor(task);

    XrValue result = aot_task_result_from_block(ctx, task, block, block.value, timeout_enabled);
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

XrValue xr_aot_channel_new(const XrAotContext *ctx, int64_t buffer_size) {
    if (!ctx || !ctx->isolate)
        return XR_NULL_VAL;
    if (buffer_size < 0)
        buffer_size = 0;
    XrChannel *ch = xr_channel_new(ctx->isolate, (uint32_t) buffer_size);
    return ch ? xr_value_from_channel(ch) : XR_NULL_VAL;
}

XrValue xr_aot_chan_try_send(const XrAotContext *ctx, XrValue channel_value, XrValue send_value) {
    if (!ctx || !ctx->isolate || !xr_value_is_channel(channel_value))
        return aot_send_result(ctx, 3);
    XrChannel *ch = xr_value_to_channel(channel_value);
    if (xr_channel_is_closed(ch))
        return aot_send_result(ctx, 3);
    return aot_send_result(ctx, xr_chan_try_send(ctx->isolate, ch, send_value) ? 0 : 1);
}

XrValue xr_aot_chan_try_send_ready(const XrAotContext *ctx, XrValue channel_value,
                                   XrValue send_value) {
    if (!ctx || !ctx->isolate || !xr_value_is_channel(channel_value))
        return xr_bool(false);
    XrChannel *ch = xr_value_to_channel(channel_value);
    if (xr_channel_is_closed(ch))
        return xr_bool(false);
    return xr_bool(xr_chan_try_send(ctx->isolate, ch, send_value));
}

XrValue xr_aot_chan_try_recv(const XrAotContext *ctx, XrValue channel_value) {
    if (!ctx || !ctx->isolate || !xr_value_is_channel(channel_value))
        return aot_recv_closed(ctx);

    XrChannel *ch = xr_value_to_channel(channel_value);
    XrValue recv_value = XR_NULL_VAL;
    bool ok = xr_chan_try_recv(ctx->isolate, ch, &recv_value, ctx->coro);
    if (ok)
        return aot_recv_value(ctx, recv_value);
    return xr_channel_is_closed(ch) ? aot_recv_closed(ctx) : aot_recv_empty(ctx);
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
    if (!ctx || !ctx->isolate || !xr_value_is_channel(channel_value))
        return XR_NULL_VAL;
    XrChannel *ch = xr_value_to_channel(channel_value);
    xr_channel_close(ch);
    return XR_NULL_VAL;
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

static XrayIsolate *aot_work_queue_isolate(const XrAotContext *ctx, XrWorkQueue *q) {
    if (ctx && ctx->isolate)
        return ctx->isolate;
    return q ? q->isolate : NULL;
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
    if (!ctx || !ctx->isolate)
        return XR_NULL_VAL;
    XrWorkQueue *q = xr_work_queue_new(ctx->isolate, aot_work_queue_sanitize_shards(shard_count),
                                       aot_work_queue_sanitize_capacity(shard_capacity));
    return q ? xr_value_from_work_queue(q) : XR_NULL_VAL;
}

XrValue xr_aot_work_queue_push(const XrAotContext *ctx, XrValue queue_value, XrValue value,
                               int64_t shard_hint) {
    if (!xr_value_is_work_queue(queue_value))
        return xr_bool(false);
    XrWorkQueue *q = xr_value_to_work_queue(queue_value);
    XrayIsolate *isolate = aot_work_queue_isolate(ctx, q);
    if (!isolate)
        return xr_bool(false);
    return xr_bool(xr_work_queue_push(isolate, q, value, shard_hint));
}

XrAotResult xr_aot_work_queue_pop(const XrAotContext *ctx, XrValue queue_value, int64_t worker_hint,
                                  XrSlotRef out_slot) {
    if (!ctx || !ctx->isolate || !ctx->coro || !xr_value_is_work_queue(queue_value))
        return xr_aot_error(XR_NULL_VAL, false);
    XrValue value = XR_NULL_VAL;
    XrWorkQueue *q = xr_value_to_work_queue(queue_value);
    switch (xr_work_queue_pop_for_coro(ctx->isolate, q, ctx->coro, worker_hint, &value)) {
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
    if (!ctx || !ctx->isolate || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);
    XrValue value = XR_NULL_VAL;
    switch (xr_work_queue_pop_resume_for_coro(ctx->isolate, ctx->coro, &value)) {
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

XrValue xr_aot_work_queue_is_closed(const XrAotContext *ctx, XrValue queue_value) {
    (void) ctx;
    if (!xr_value_is_work_queue(queue_value))
        return xr_bool(false);
    return xr_bool(xr_work_queue_is_closed(xr_value_to_work_queue(queue_value)));
}

XrValue xr_aot_tuple_get(const XrAotContext *ctx, XrValue tuple_value, uint16_t index) {
    (void) ctx;
    if (!xr_value_is_tuple(tuple_value))
        return XR_NULL_VAL;
    return xr_tuple_get(xr_value_to_tuple(tuple_value), index);
}

XrAotResult xr_aot_chan_send(const XrAotContext *ctx, XrValue channel_value, XrValue send_value,
                             XrSlotRef result_slot, int64_t timeout_ms) {
    if (!ctx || !ctx->isolate || !ctx->coro || !xr_value_is_channel(channel_value))
        return xr_aot_error(XR_NULL_VAL, false);

    XrChannel *ch = xr_value_to_channel(channel_value);
    XrCoroBlockResult block =
        xr_coro_chan_send(ctx->isolate, ctx->coro, ch, send_value, result_slot, timeout_ms);
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
    if (!ctx || !ctx->isolate || !ctx->coro || !xr_value_is_channel(channel_value))
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
    if (!ctx || !ctx->isolate || !ctx->coro || !xr_value_is_channel(channel_value))
        return xr_aot_error(XR_NULL_VAL, false);

    XrChannel *ch = xr_value_to_channel(channel_value);
    XrCoroBlockResult block =
        xr_coro_chan_recv(ctx->isolate, ctx->coro, ch, out_slot, xr_slot_none(), timeout_ms, false);
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
    if (!ctx || !ctx->isolate || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrSlotRef value_slot = out_slot;
    if (value_slot.kind == XR_SLOT_NONE)
        value_slot = ctx->coro->ext ? ctx->coro->ext->recv_slot_ref : xr_slot_none();
    XrCoroBlockResult block =
        xr_coro_chan_recv_resume(ctx->isolate, ctx->coro, value_slot, xr_slot_none());
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
    if (!ctx || !ctx->isolate || !ctx->coro || !xr_value_is_channel(channel_value))
        return xr_aot_error(XR_NULL_VAL, false);

    XrChannel *ch = xr_value_to_channel(channel_value);
    XrCoroBlockResult block =
        xr_coro_chan_recv(ctx->isolate, ctx->coro, ch, value_slot, ok_slot, timeout_ms, true);
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
    if (!ctx || !ctx->isolate || !ctx->coro || !xr_value_is_channel(channel_value))
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
    if (!ctx || !ctx->isolate || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrCoroBlockResult block =
        xr_coro_chan_recv_resume(ctx->isolate, ctx->coro, value_slot, ok_slot);
    if (block.kind == XR_CORO_BLOCK_READY || block.kind == XR_CORO_BLOCK_CLOSED ||
        block.kind == XR_CORO_BLOCK_TIMEOUT || block.kind == XR_CORO_BLOCK_NO_CORO)
        return xr_aot_done(block.value);
    return xr_aot_error(XR_NULL_VAL, false);
}
