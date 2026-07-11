/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_parallel_ops.c - VM hosted structured parallel batch operations
 *
 * This is the VM backend for stdlib `parallel.*` intrinsic calls after
 * lowering to XI_PAR_*. It deliberately stays behind internal opcodes:
 * users see only the stdlib API, while the VM can submit a small set of lane
 * coroutines to the existing runtime scheduler.
 */

#include "xvm_dispatch_helpers.h"
#include "xvm_coro_api.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#include "../base/xmalloc.h"
#include "../coro/xdeep_copy.h"
#include "../coro/xcountdown_latch.h"
#include "../coro/xparallel_executor.h"
#include "../coro/xworker.h"
#include "../coro/xyieldable.h"
#include "../runtime/closure/xclosure.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/mem/xobj_header.h"
#include "../runtime/object/xpanic_info.h"
#include "../runtime/object/xarray.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/xshared.h"

enum {
    XR_VM_PAR_MAX_LANES = XR_PARALLEL_EXECUTOR_MAX_LANES,
    XR_VM_PAR_FLAG_INCLUSIVE_END = 1 << 0,
    XR_VM_PAR_FLAG_RANGE_BODY = 1 << 1,
    XR_VM_PAR_FLAG_PLAN_STATE = 1 << 2,
};

typedef struct XrVmParBatch XrVmParBatch;

typedef enum XrVmParLanePhase {
    XR_VM_PAR_LANE_BODY = 0,
    XR_VM_PAR_LANE_COMBINE = 1,
} XrVmParLanePhase;

typedef struct XrVmParLane {
    XrVmParBatch *batch;
    int lane_id;
    int64_t iter;
    int64_t limit;
    XrValue call_args[4];
    XrValue partial;
    XrValue pending_value;
    bool partial_init;
    XrVmParLanePhase phase;
} XrVmParLane;

struct XrVmParBatch {
    XrRuntimeCore *core;
    XrRuntime *runtime;
    XrCountdownLatch *latch;
    XrClosure *closure; /* Borrowed body closure; parent blocks until latch completion. */
    XrClosure *combine; /* Borrowed reducer closure for per-lane partials. */
    XrArray *output;    /* Borrowed typed result/partial array; parent waits before use. */
    XrArray *states;    /* Borrowed Plan lane-state array; parent waits before reuse. */
    int64_t start;
    int lane_count;
    bool range_body;
    bool plan_state;
    bool map_body;
    bool reduce_body;
    _Atomic bool failed;
    _Atomic bool error_recorded;
    bool first_error_is_value;
    XrValue first_error;
    XrVmParLane *lanes;
    XrCoroutine **coros;
};

static inline XrValue vm_par_batch_to_value(XrVmParBatch *batch) {
    return xr_int((int64_t) (intptr_t) batch);
}

static inline XrVmParBatch *vm_par_batch_from_value(XrValue value) {
    if (!XR_IS_INT(value) || XR_TO_INT(value) == 0)
        return NULL;
    return (XrVmParBatch *) (intptr_t) XR_TO_INT(value);
}

static bool vm_par_closure_safe_to_share(const XrClosure *closure) {
    if (!closure)
        return false;
    for (uint16_t i = 0; i < closure->upval_count; i++) {
        XrValue uv = closure->upvals[i];
        if (!XR_IS_PTR(uv))
            continue;
        XrObjHeader *h = (XrObjHeader *) XR_VALUE_GCPTR(uv);
        if (!h)
            continue;
        int32_t rc = atomic_load_explicit(&h->refcount, memory_order_relaxed);
        if (rc < 0)
            continue; /* shared / managed / immortal: atomic or no-op RC */
        return false; /* private heap object: keep this call on the sequential fallback path */
    }
    return true;
}

static XrArrayElemType vm_par_scalar_elem_type(XrValue value) {
    if (XR_IS_INT(value))
        return XR_ELEM_I64;
    if (XR_IS_FLOAT(value))
        return XR_ELEM_F64;
    if (XR_IS_BOOL(value))
        return XR_ELEM_BOOL;
    if (XR_IS_CHAR(value))
        return XR_ELEM_CHAR;
    return XR_ELEM_ANY;
}

static bool vm_par_value_fits_elem_type(XrValue value, XrArrayElemType elem_type) {
    switch (elem_type) {
        case XR_ELEM_I64:
            return XR_IS_INT(value);
        case XR_ELEM_F64:
            return XR_IS_FLOAT(value) || XR_IS_INT(value);
        case XR_ELEM_BOOL:
            return XR_IS_BOOL(value);
        case XR_ELEM_CHAR:
            return XR_IS_CHAR(value);
        default:
            return false;
    }
}

static void vm_par_latch_release(XrVmParBatch *batch) {
    if (!batch || !batch->latch)
        return;
    XrObjHeader *hdr = &batch->latch->hdr;
    if (xr_shared_decref(hdr) == 0)
        xr_shared_destroy_core(batch->core, hdr);
    batch->latch = NULL;
}

static void vm_par_batch_free(XrVmParBatch *batch) {
    if (!batch)
        return;
    if (!XR_IS_NULL(batch->first_error)) {
        xr_chan_transit_release_core(batch->core, batch->first_error);
        batch->first_error = xr_null();
    }
    vm_par_latch_release(batch);
    xr_free(batch->coros);
    xr_free(batch->lanes);
    xr_free(batch);
}

static XrValue vm_par_take_pending_value_error(XrVMRuntime *isolate) {
    XrVMContext *ctx = isolate ? xr_vm_current_ctx(isolate) : NULL;
    if (!ctx || XR_IS_NULL(ctx->pending_error))
        return xr_null();
    XrValue error = ctx->pending_error;
    ctx->pending_error = xr_null();
    return error;
}

static void vm_par_batch_record_failure(XrVMRuntime *isolate, XrVmParBatch *batch, XrValue error,
                                        bool is_value_error, const char *fallback_message) {
    if (!batch)
        return;

    bool expected = false;
    if (atomic_compare_exchange_strong_explicit(&batch->error_recorded, &expected, true,
                                                memory_order_acq_rel, memory_order_acquire)) {
        XrValue exc = error;
        if (XR_IS_NULL(exc)) {
            is_value_error = false;
            exc = (isolate && fallback_message)
                      ? xr_panic_info_new(isolate, XR_ERR_RUNTIME, fallback_message)
                      : xr_null();
        } else if (!is_value_error && isolate && !xr_value_is_panic_info(isolate, exc)) {
            exc = xr_panic_info_from_value(isolate, exc);
        }
        batch->first_error_is_value = is_value_error;
        if (!XR_IS_NULL(exc))
            batch->first_error = xr_deep_copy_to_transit_core(batch->core, exc);
    }

    atomic_store_explicit(&batch->failed, true, memory_order_release);
}

static void vm_par_batch_cancel_unspawned(XrVmParBatch *batch) {
    if (!batch || !batch->coros)
        return;
    for (int i = 0; i < batch->lane_count; i++) {
        XrCoroutine *coro = batch->coros[i];
        if (coro)
            xr_coro_discard_uninitialized(coro);
        batch->coros[i] = NULL;
    }
}

static void vm_par_lane_done(XrVmParLane *lane) {
    if (!lane || !lane->batch || !lane->batch->latch)
        return;
    (void) xr_countdown_latch_done(lane->batch->latch, 1);
}

static bool vm_par_lane_store_partial(XrVmParLane *lane) {
    if (!lane || !lane->batch || !lane->batch->output || !lane->partial_init)
        return false;
    XrArray *output = lane->batch->output;
    if (lane->lane_id < 0 || lane->lane_id >= output->length)
        return false;
    XrArrayElemType elem_type = (XrArrayElemType) output->elem_type;
    if (elem_type == XR_ELEM_ANY) {
        ((XrValue *) output->data)[lane->lane_id] =
            xr_deep_copy_to_transit_core(lane->batch->core, lane->partial);
        return true;
    }
    if (!vm_par_value_fits_elem_type(lane->partial, elem_type))
        return false;
    xr_array_set_element(output, lane->lane_id, lane->partial);
    return true;
}

static XrCFuncResult vm_par_lane_call_next(XrVMRuntime *isolate, XrVmParLane *lane,
                                           XrValue *result);

static XrCFuncResult vm_par_lane_continue(XrVMRuntime *isolate, int status, XrValue resume_value,
                                          void *ctx, XrValue *result) {
    XrVmParLane *lane = (XrVmParLane *) ctx;
    if (!lane || !lane->batch)
        return XR_CFUNC_ERROR;
    if (status == XR_RESUME_CLOSURE_ERROR || status == XR_RESUME_ERROR ||
        status == XR_RESUME_CANCELLED) {
        const char *fallback =
            status == XR_RESUME_CANCELLED ? "parallel lane cancelled" : "parallel lane failed";
        vm_par_batch_record_failure(isolate, lane->batch, resume_value, false, fallback);
        vm_par_lane_done(lane);
        if (result)
            *result = xr_null();
        return XR_CFUNC_DONE;
    }
    XrVmParBatch *batch = lane->batch;
    XrValue pending_value_error = vm_par_take_pending_value_error(isolate);
    if (!XR_IS_NULL(pending_value_error)) {
        vm_par_batch_record_failure(isolate, batch, pending_value_error, true,
                                    "parallel lane failed");
        vm_par_lane_done(lane);
        if (result)
            *result = xr_null();
        return XR_CFUNC_DONE;
    }
    if (batch->reduce_body) {
        if (lane->phase == XR_VM_PAR_LANE_COMBINE) {
            lane->partial = resume_value;
            lane->partial_init = true;
            lane->pending_value = xr_null();
            lane->phase = XR_VM_PAR_LANE_BODY;
            lane->iter++;
            return vm_par_lane_call_next(isolate, lane, result);
        }

        if (batch->range_body || !lane->partial_init) {
            lane->partial = resume_value;
            lane->partial_init = true;
            if (batch->range_body)
                lane->iter = lane->limit;
            else
                lane->iter++;
            return vm_par_lane_call_next(isolate, lane, result);
        }

        if (!batch->combine) {
            vm_par_batch_record_failure(isolate, batch, xr_null(), false,
                                        "parallel.reduce missing combine closure");
            vm_par_lane_done(lane);
            if (result)
                *result = xr_null();
            return XR_CFUNC_DONE;
        }
        lane->pending_value = resume_value;
        lane->phase = XR_VM_PAR_LANE_COMBINE;
        lane->call_args[0] = lane->partial;
        lane->call_args[1] = lane->pending_value;
        return xr_call_closure(isolate, batch->combine, lane->call_args, 2, vm_par_lane_continue,
                               lane, result);
    }
    if (batch->map_body) {
        XrArray *output = batch->output;
        int64_t out_index = lane->iter - batch->start;
        if (!output || output->elem_type == XR_ELEM_ANY || out_index < 0 ||
            out_index >= output->length || out_index > INT32_MAX ||
            (output->elem_type == XR_ELEM_CHAR && !XR_IS_CHAR(resume_value))) {
            vm_par_batch_record_failure(isolate, batch, xr_null(), false,
                                        "parallel.map lane result store failed");
            vm_par_lane_done(lane);
            if (result)
                *result = xr_null();
            return XR_CFUNC_DONE;
        }
        xr_array_set_element(output, (int32_t) out_index, resume_value);
    }
    if (lane->batch->range_body)
        lane->iter = lane->limit;
    else
        lane->iter++;
    return vm_par_lane_call_next(isolate, lane, result);
}

static XrCFuncResult vm_par_lane_call_next(XrVMRuntime *isolate, XrVmParLane *lane,
                                           XrValue *result) {
    if (!lane || !lane->batch || !lane->batch->closure)
        return XR_CFUNC_ERROR;
    XrVmParBatch *batch = lane->batch;
    if (atomic_load_explicit(&batch->failed, memory_order_acquire) || lane->iter >= lane->limit) {
        if (batch->reduce_body && !atomic_load_explicit(&batch->failed, memory_order_acquire) &&
            !vm_par_lane_store_partial(lane)) {
            vm_par_batch_record_failure(isolate, batch, xr_null(), false,
                                        "parallel.reduce partial store failed");
        }
        vm_par_lane_done(lane);
        if (result)
            *result = xr_null();
        return XR_CFUNC_DONE;
    }

    lane->call_args[0] = xr_int(lane->iter);
    if (batch->plan_state) {
        if (!batch->states || lane->lane_id < 0 || lane->lane_id >= batch->states->length) {
            vm_par_batch_record_failure(isolate, batch, xr_null(), false,
                                        "parallel lane state unavailable");
            vm_par_lane_done(lane);
            if (result)
                *result = xr_null();
            return XR_CFUNC_DONE;
        }
        lane->call_args[0] = xr_array_get_element(batch->states, lane->lane_id);
        lane->call_args[1] = xr_int(lane->iter);
        lane->call_args[2] = xr_int(lane->lane_id);
        return xr_call_closure(isolate, batch->closure, lane->call_args, 3, vm_par_lane_continue,
                               lane, result);
    }
    if (batch->range_body) {
        lane->call_args[1] = xr_int(lane->limit);
        lane->call_args[2] = xr_int(lane->lane_id);
        return xr_call_closure(isolate, batch->closure, lane->call_args, 3, vm_par_lane_continue,
                               lane, result);
    }
    lane->call_args[1] = xr_int(lane->lane_id);
    return xr_call_closure(isolate, batch->closure, lane->call_args, 2, vm_par_lane_continue, lane,
                           result);
}

static XrCFuncResult vm_par_lane_entry(XrVMRuntime *isolate, XrValue *args, int nargs,
                                       XrValue *result) {
    if (!args || nargs < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return XR_CFUNC_ERROR;
    XrVmParBatch *batch = (XrVmParBatch *) (intptr_t) XR_TO_INT(args[0]);
    int64_t lane_index = XR_TO_INT(args[1]);
    if (!batch || lane_index < 0 || lane_index >= batch->lane_count)
        return XR_CFUNC_ERROR;
    return vm_par_lane_call_next(isolate, &batch->lanes[lane_index], result);
}

static XrVmParBatch *vm_par_batch_new(XrVMRuntime *isolate, XrRuntime *runtime, XrClosure *closure,
                                      XrClosure *combine, XrArray *output, XrArray *states,
                                      int64_t start, int64_t end_excl, int lane_count,
                                      bool range_body, bool plan_state, bool map_body,
                                      bool reduce_body) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    if (!core || !runtime || !closure || lane_count <= 1)
        return NULL;
    int64_t item_count = end_excl - start;
    if (item_count <= 1)
        return NULL;

    XrVmParBatch *batch = (XrVmParBatch *) xr_calloc(1, sizeof(XrVmParBatch));
    if (!batch)
        return NULL;
    batch->core = core;
    batch->runtime = runtime;
    batch->closure = closure;
    batch->combine = combine;
    batch->output = output;
    batch->states = states;
    batch->start = start;
    batch->lane_count = lane_count;
    batch->range_body = range_body;
    batch->plan_state = plan_state;
    batch->map_body = map_body;
    batch->reduce_body = reduce_body;
    atomic_store_explicit(&batch->failed, false, memory_order_relaxed);
    atomic_store_explicit(&batch->error_recorded, false, memory_order_relaxed);
    batch->first_error_is_value = false;
    batch->first_error = xr_null();

    batch->lanes = (XrVmParLane *) xr_calloc((size_t) lane_count, sizeof(XrVmParLane));
    batch->coros = (XrCoroutine **) xr_calloc((size_t) lane_count, sizeof(XrCoroutine *));
    batch->latch = xr_countdown_latch_new(core, runtime, lane_count);
    if (!batch->lanes || !batch->coros || !batch->latch) {
        vm_par_batch_free(batch);
        return NULL;
    }

    for (int lane = 0; lane < lane_count; lane++) {
        int64_t lane_begin = 0;
        int64_t lane_end = 0;
        if (!xr_parallel_lane_bounds(start, end_excl, lane_count, lane, &lane_begin, &lane_end)) {
            vm_par_batch_cancel_unspawned(batch);
            vm_par_batch_free(batch);
            return NULL;
        }
        XrVmParLane *slot = &batch->lanes[lane];
        slot->batch = batch;
        slot->lane_id = lane;
        slot->iter = lane_begin;
        slot->limit = lane_end;
        slot->partial = xr_null();
        slot->pending_value = xr_null();
        slot->partial_init = false;
        slot->phase = XR_VM_PAR_LANE_BODY;

        XrValue args[2] = {vm_par_batch_to_value(batch), xr_int(lane)};
        batch->coros[lane] = xr_coro_create_vm_cfunc(
            isolate, vm_par_lane_entry, args, 2,
            reduce_body ? "parallel.reduce.lane"
                        : (map_body ? "parallel.map.lane" : "parallel.forEach.lane"));
        if (!batch->coros[lane]) {
            vm_par_batch_cancel_unspawned(batch);
            vm_par_batch_free(batch);
            return NULL;
        }
    }
    return batch;
}

static XrDispatchAction vm_par_wait_for_batch(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                              XrBcCallFrame *frame, XrInstruction *pc,
                                              XrValue *handled_slot, XrValue *batch_slot,
                                              XrValue *materialized_output_slot,
                                              XrVmParBatch *batch, bool resume) {
    XrCoroutine *current = vm_get_coro(vm_ctx);
    if (!current) {
        vm_par_batch_free(batch);
        *batch_slot = xr_null();
        *handled_slot = xr_bool(false);
        return XR_DISP_NEXT;
    }

    bool ok = false;
    XrCountdownLatchWaitStatus wait =
        resume ? xr_countdown_latch_wait_resume_for_coro(current, &ok)
               : xr_countdown_latch_wait_for_coro(batch->latch, current, &ok);
    if (wait == XR_COUNTDOWN_LATCH_WAIT_BLOCKED) {
        vm_suspend_replay_current(frame, pc);
        return XR_DISP_BLOCKED;
    }
    if (wait != XR_COUNTDOWN_LATCH_WAIT_DONE || !ok) {
        vm_par_batch_free(batch);
        *batch_slot = xr_null();
        VM_THROW(frame, pc, XR_ERR_RUNTIME, "parallel batch wait failed");
    }
    bool failed = atomic_load_explicit(&batch->failed, memory_order_acquire);
    bool failed_with_value_error = failed && batch->first_error_is_value;
    XrValue lane_error = xr_null();
    if (failed && !XR_IS_NULL(batch->first_error)) {
        lane_error = xr_deep_copy_to_coro(isolate, batch->first_error, current);
        xr_chan_transit_release_core(batch->core, batch->first_error);
        batch->first_error = xr_null();
    }
    XrArray *mutated_output =
        (!failed && (batch->map_body || batch->reduce_body)) ? batch->output : NULL;
    if (!failed && materialized_output_slot && batch->reduce_body && batch->output &&
        batch->output->elem_type == XR_ELEM_ANY) {
        XrArray *shared_partials = batch->output;
        XrArray *private_partials = xr_array_with_capacity(current, shared_partials->length);
        if (!private_partials || private_partials->capacity < shared_partials->length) {
            vm_par_batch_free(batch);
            *batch_slot = xr_null();
            VM_THROW(frame, pc, XR_ERR_OUT_OF_MEMORY,
                     "parallel.reduce partial materialization failed");
        }
        private_partials->length = shared_partials->length;
        if (private_partials->length > 0 && private_partials->data) {
            memset(private_partials->data, 0, (size_t) private_partials->length * sizeof(XrValue));
        }
        for (int32_t idx = 0; idx < shared_partials->length; idx++) {
            XrValue transit = xr_array_get_element(shared_partials, idx);
            XrValue private_value = xr_deep_copy_to_coro(isolate, transit, current);
            ((XrValue *) private_partials->data)[idx] = private_value;
            XR_ARRAY_MARK_REFS(private_partials, private_value);
        }
        *materialized_output_slot = xr_value_from_array(private_partials);
        mutated_output = private_partials;
        if (XR_OBJ_IS_SHARED(&shared_partials->hdr) && xr_shared_decref(&shared_partials->hdr) == 0)
            xr_shared_destroy_core(batch->core, &shared_partials->hdr);
        batch->output = NULL;
    }
    vm_par_batch_free(batch);
    *batch_slot = xr_null();
    if (failed) {
        if (failed_with_value_error && !XR_IS_NULL(lane_error)) {
            vm_ctx->pending_error = lane_error;
            *handled_slot = xr_bool(true);
            return XR_DISP_NEXT;
        }
        if (!XR_IS_NULL(lane_error)) {
            frame->pc = pc;
            xr_vm_unwind_with_trace(isolate, lane_error);
            return XR_DISP_RAISE;
        }
        VM_THROW(frame, pc, XR_ERR_RUNTIME, "parallel lane failed");
    }
    if (mutated_output)
        XR_ARRAY_MARK_MUTATED(mutated_output);
    *handled_slot = xr_bool(true);
    return XR_DISP_NEXT;
}

XR_FUNC XrDispatchAction vm_par_for_dispatch(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                             XrInstruction instr, XrValue *base,
                                             XrBcCallFrame *frame, XrInstruction *pc) {
    if (!isolate || !vm_ctx || !base || !frame) {
        return XR_DISP_FATAL;
    }

    int out = GETARG_A(instr);
    int arg_base = GETARG_B(instr);
    int flags = GETARG_C(instr);
    XrValue *handled_slot = &base[out];
    XrValue *start_slot = &base[arg_base + 0];
    XrValue *end_slot = &base[arg_base + 1];
    XrValue *workers_slot = &base[arg_base + 2];
    XrValue *closure_slot = &base[arg_base + 3];
    bool plan_state = (flags & XR_VM_PAR_FLAG_PLAN_STATE) != 0;
    XrValue *states_slot = plan_state ? &base[arg_base + 4] : NULL;
    XrValue *batch_slot = &base[arg_base + (plan_state ? 5 : 4)];

    XrVmParBatch *resume_batch = vm_par_batch_from_value(*batch_slot);
    if (resume_batch)
        return vm_par_wait_for_batch(isolate, vm_ctx, frame, pc, handled_slot, batch_slot, NULL,
                                     resume_batch, true);

    *handled_slot = xr_bool(false);
    if (!XR_IS_INT(*workers_slot))
        return XR_DISP_NEXT;
    int64_t workers = XR_TO_INT(*workers_slot);
    if (workers < 0) {
        VM_THROW(frame, pc, XR_ERR_RUNTIME, "parallel.Options.workers must be >= 0, got %" PRId64,
                 workers);
    }
    if (!XR_IS_INT(*start_slot) || !XR_IS_INT(*end_slot) || !XR_IS_PTR(*closure_slot) ||
        XR_HEAP_TYPE(*closure_slot) != XR_TFUNCTION) {
        return XR_DISP_NEXT;
    }

    int64_t start = XR_TO_INT(*start_slot);
    int64_t end_excl = XR_TO_INT(*end_slot);
    if ((flags & XR_VM_PAR_FLAG_INCLUSIVE_END) != 0) {
        if (end_excl == INT64_MAX)
            return XR_DISP_NEXT;
        end_excl++;
    }
    int64_t item_count = end_excl - start;
    if (item_count <= 0) {
        *handled_slot = xr_bool(true);
        return XR_DISP_NEXT;
    }

    XrRuntime *runtime = xr_isolate_get_scheduler_runtime(isolate);
    int lane_count =
        xr_parallel_resolve_lane_count(runtime, item_count, workers, XR_VM_PAR_MAX_LANES);
    if (lane_count <= 1)
        return XR_DISP_NEXT;

    XrClosure *closure = (XrClosure *) XR_TO_PTR(*closure_slot);
    if (!vm_par_closure_safe_to_share(closure))
        return XR_DISP_NEXT;
    XrArray *states = NULL;
    if (plan_state) {
        if (!states_slot || !XR_IS_ARRAY(*states_slot))
            return XR_DISP_NEXT;
        states = XR_TO_ARRAY(*states_slot);
        if (!states || states->length < lane_count)
            return XR_DISP_NEXT;
    }

    XrVmParBatch *batch =
        vm_par_batch_new(isolate, runtime, closure, NULL, NULL, states, start, end_excl, lane_count,
                         (flags & XR_VM_PAR_FLAG_RANGE_BODY) != 0, plan_state, false, false);
    if (!batch) {
        VM_THROW(frame, pc, XR_ERR_OUT_OF_MEMORY, "parallel.forEach batch allocation failed");
    }

    *batch_slot = vm_par_batch_to_value(batch);
    xr_runtime_spawn_batch(runtime, batch->coros, batch->lane_count);
    return vm_par_wait_for_batch(isolate, vm_ctx, frame, pc, handled_slot, batch_slot, NULL, batch,
                                 false);
}

XR_FUNC XrDispatchAction vm_par_map_dispatch(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                             XrInstruction instr, XrValue *base,
                                             XrBcCallFrame *frame, XrInstruction *pc) {
    if (!isolate || !vm_ctx || !base || !frame) {
        return XR_DISP_FATAL;
    }

    int out = GETARG_A(instr);
    int arg_base = GETARG_B(instr);
    int flags = GETARG_C(instr);
    XrValue *handled_slot = &base[out];
    XrValue *start_slot = &base[arg_base + 0];
    XrValue *end_slot = &base[arg_base + 1];
    XrValue *workers_slot = &base[arg_base + 2];
    XrValue *closure_slot = &base[arg_base + 3];
    XrValue *output_slot = &base[arg_base + 4];
    bool plan_state = (flags & XR_VM_PAR_FLAG_PLAN_STATE) != 0;
    XrValue *states_slot = plan_state ? &base[arg_base + 5] : NULL;
    XrValue *batch_slot = &base[arg_base + (plan_state ? 6 : 5)];

    XrVmParBatch *resume_batch = vm_par_batch_from_value(*batch_slot);
    if (resume_batch)
        return vm_par_wait_for_batch(isolate, vm_ctx, frame, pc, handled_slot, batch_slot, NULL,
                                     resume_batch, true);

    *handled_slot = xr_bool(false);
    if (!XR_IS_INT(*workers_slot))
        return XR_DISP_NEXT;
    int64_t workers = XR_TO_INT(*workers_slot);
    if (workers < 0) {
        VM_THROW(frame, pc, XR_ERR_RUNTIME, "parallel.Options.workers must be >= 0, got %" PRId64,
                 workers);
    }
    if (!XR_IS_INT(*start_slot) || !XR_IS_INT(*end_slot) || !XR_IS_PTR(*closure_slot) ||
        XR_HEAP_TYPE(*closure_slot) != XR_TFUNCTION || !XR_IS_ARRAY(*output_slot)) {
        return XR_DISP_NEXT;
    }

    XrArray *output = XR_TO_ARRAY(*output_slot);
    if (!output || output->elem_type == XR_ELEM_ANY)
        return XR_DISP_NEXT;

    int64_t start = XR_TO_INT(*start_slot);
    int64_t end_excl = XR_TO_INT(*end_slot);
    if ((flags & XR_VM_PAR_FLAG_INCLUSIVE_END) != 0) {
        if (end_excl == INT64_MAX)
            return XR_DISP_NEXT;
        end_excl++;
    }
    int64_t item_count = end_excl - start;
    if (item_count <= 0) {
        *handled_slot = xr_bool(true);
        return XR_DISP_NEXT;
    }
    if (item_count > output->length || item_count > INT32_MAX)
        return XR_DISP_NEXT;

    XrRuntime *runtime = xr_isolate_get_scheduler_runtime(isolate);
    int lane_count =
        xr_parallel_resolve_lane_count(runtime, item_count, workers, XR_VM_PAR_MAX_LANES);
    if (lane_count <= 1)
        return XR_DISP_NEXT;

    XrClosure *closure = (XrClosure *) XR_TO_PTR(*closure_slot);
    if (!vm_par_closure_safe_to_share(closure))
        return XR_DISP_NEXT;
    XrArray *states = NULL;
    if (plan_state) {
        if (!states_slot || !XR_IS_ARRAY(*states_slot))
            return XR_DISP_NEXT;
        states = XR_TO_ARRAY(*states_slot);
        if (!states || states->length < lane_count)
            return XR_DISP_NEXT;
    }

    XrVmParBatch *batch = vm_par_batch_new(isolate, runtime, closure, NULL, output, states, start,
                                           end_excl, lane_count, false, plan_state, true, false);
    if (!batch) {
        VM_THROW(frame, pc, XR_ERR_OUT_OF_MEMORY, "parallel.map batch allocation failed");
    }

    *batch_slot = vm_par_batch_to_value(batch);
    xr_runtime_spawn_batch(runtime, batch->coros, batch->lane_count);
    return vm_par_wait_for_batch(isolate, vm_ctx, frame, pc, handled_slot, batch_slot, NULL, batch,
                                 false);
}

XR_FUNC XrDispatchAction vm_par_reduce_dispatch(XrVMRuntime *isolate, XrVMContext *vm_ctx,
                                                XrInstruction instr, XrValue *base,
                                                XrBcCallFrame *frame, XrInstruction *pc) {
    if (!isolate || !vm_ctx || !base || !frame) {
        return XR_DISP_FATAL;
    }

    int out = GETARG_A(instr);
    int arg_base = GETARG_B(instr);
    int flags = GETARG_C(instr);
    XrValue *handled_slot = &base[out];
    XrValue *start_slot = &base[arg_base + 0];
    XrValue *end_slot = &base[arg_base + 1];
    XrValue *workers_slot = &base[arg_base + 2];
    XrValue *initial_slot = &base[arg_base + 3];
    XrValue *body_slot = &base[arg_base + 4];
    XrValue *combine_slot = &base[arg_base + 5];
    bool plan_state = (flags & XR_VM_PAR_FLAG_PLAN_STATE) != 0;
    XrValue *states_slot = plan_state ? &base[arg_base + 6] : NULL;
    XrValue *batch_slot = &base[arg_base + (plan_state ? 7 : 6)];
    XrValue *partials_slot = &base[arg_base + (plan_state ? 8 : 7)];

    XrVmParBatch *resume_batch = vm_par_batch_from_value(*batch_slot);
    if (resume_batch)
        return vm_par_wait_for_batch(isolate, vm_ctx, frame, pc, handled_slot, batch_slot,
                                     partials_slot, resume_batch, true);

    *handled_slot = xr_bool(false);
    if (!XR_IS_INT(*workers_slot))
        return XR_DISP_NEXT;
    int64_t workers = XR_TO_INT(*workers_slot);
    if (workers < 0) {
        VM_THROW(frame, pc, XR_ERR_RUNTIME, "parallel.Options.workers must be >= 0, got %" PRId64,
                 workers);
    }
    if (!XR_IS_INT(*start_slot) || !XR_IS_INT(*end_slot) || !XR_IS_PTR(*body_slot) ||
        XR_HEAP_TYPE(*body_slot) != XR_TFUNCTION || !XR_IS_PTR(*combine_slot) ||
        XR_HEAP_TYPE(*combine_slot) != XR_TFUNCTION) {
        return XR_DISP_NEXT;
    }

    XrArrayElemType elem_type = vm_par_scalar_elem_type(*initial_slot);

    int64_t start = XR_TO_INT(*start_slot);
    int64_t end_excl = XR_TO_INT(*end_slot);
    if ((flags & XR_VM_PAR_FLAG_INCLUSIVE_END) != 0) {
        if (end_excl == INT64_MAX)
            return XR_DISP_NEXT;
        end_excl++;
    }
    int64_t item_count = end_excl - start;
    if (item_count <= 0) {
        *handled_slot = xr_bool(true);
        return XR_DISP_NEXT;
    }

    XrRuntime *runtime = xr_isolate_get_scheduler_runtime(isolate);
    int lane_count =
        xr_parallel_resolve_lane_count(runtime, item_count, workers, XR_VM_PAR_MAX_LANES);
    if (lane_count <= 1)
        return XR_DISP_NEXT;

    XrClosure *body = (XrClosure *) XR_TO_PTR(*body_slot);
    XrClosure *combine = (XrClosure *) XR_TO_PTR(*combine_slot);
    if (!vm_par_closure_safe_to_share(body) || !vm_par_closure_safe_to_share(combine))
        return XR_DISP_NEXT;
    XrArray *states = NULL;
    if (plan_state) {
        if (!states_slot || !XR_IS_ARRAY(*states_slot))
            return XR_DISP_NEXT;
        states = XR_TO_ARRAY(*states_slot);
        if (!states || states->length < lane_count)
            return XR_DISP_NEXT;
    }

    XrCoroutine *current = vm_get_coro(vm_ctx);
    if (!current)
        return XR_DISP_NEXT;
    XrArray *partials =
        elem_type == XR_ELEM_ANY
            ? xr_array_new_shared_core(xr_isolate_get_runtime_core(isolate), lane_count)
            : xr_array_with_capacity_typed(current, lane_count, elem_type);
    if (!partials || partials->capacity < lane_count) {
        VM_THROW(frame, pc, XR_ERR_OUT_OF_MEMORY, "parallel.reduce partial allocation failed");
    }
    partials->length = lane_count;
    if (elem_type == XR_ELEM_ANY) {
        if (partials->data)
            memset(partials->data, 0, (size_t) lane_count * sizeof(XrValue));
        partials->contains_refs = 1;
    }
    *partials_slot = xr_value_from_array(partials);

    XrVmParBatch *batch = vm_par_batch_new(
        isolate, runtime, body, combine, partials, states, start, end_excl, lane_count,
        (flags & XR_VM_PAR_FLAG_RANGE_BODY) != 0, plan_state, false, true);
    if (!batch) {
        VM_THROW(frame, pc, XR_ERR_OUT_OF_MEMORY, "parallel.reduce batch allocation failed");
    }

    *batch_slot = vm_par_batch_to_value(batch);
    xr_runtime_spawn_batch(runtime, batch->coros, batch->lane_count);
    return vm_par_wait_for_batch(isolate, vm_ctx, frame, pc, handled_slot, batch_slot,
                                 partials_slot, batch, false);
}
