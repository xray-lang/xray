/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_parallel_ops.c - VM hosted structured parallel batch operations
 *
 * This is the VM backend for stdlib `parallel.forEach` intrinsic calls after
 * lowering to XI_PAR_FOR. It deliberately stays behind an internal opcode:
 * users see only the stdlib API, while the VM can submit a small set of lane
 * coroutines to the existing runtime scheduler.
 */

#include "xvm_dispatch_helpers.h"
#include "xvm_coro_api.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdatomic.h>

#include "../base/xmalloc.h"
#include "../coro/xcountdown_latch.h"
#include "../coro/xworker.h"
#include "../coro/xyieldable.h"
#include "../runtime/closure/xclosure.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/mem/xobj_header.h"
#include "../runtime/object/xarray.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/xshared.h"

enum {
    XR_VM_PAR_MAX_LANES = 256,
    XR_VM_PAR_FLAG_INCLUSIVE_END = 1 << 0,
    XR_VM_PAR_FLAG_RANGE_BODY = 1 << 1,
};

typedef struct XrVmParBatch XrVmParBatch;

typedef struct XrVmParLane {
    XrVmParBatch *batch;
    int lane_id;
    int64_t iter;
    int64_t limit;
    XrValue call_args[3];
} XrVmParLane;

struct XrVmParBatch {
    XrRuntimeCore *core;
    XrRuntime *runtime;
    XrCountdownLatch *latch;
    XrClosure *closure; /* Borrowed from the parent frame; parent blocks until latch completion. */
    XrArray *output;    /* Borrowed typed result array for map lanes; parent waits before use. */
    int64_t start;
    int lane_count;
    bool range_body;
    bool map_body;
    _Atomic bool failed;
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

static int vm_par_resolve_lanes(XrRuntime *runtime, int64_t item_count, int64_t requested_workers) {
    if (!runtime || item_count <= 1 || requested_workers == 1)
        return 0;
    if (xr_runtime_deterministic_mode(runtime))
        return 0;
    int runtime_workers = runtime->worker_count;
    if (runtime_workers <= 1)
        return 0;

    int64_t lanes = requested_workers == 0 ? (int64_t) runtime_workers : requested_workers;
    if (lanes > item_count)
        lanes = item_count;
    if (lanes > runtime_workers)
        lanes = runtime_workers;
    if (lanes > XR_VM_PAR_MAX_LANES)
        lanes = XR_VM_PAR_MAX_LANES;
    return lanes > 1 ? (int) lanes : 0;
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
    vm_par_latch_release(batch);
    xr_free(batch->coros);
    xr_free(batch->lanes);
    xr_free(batch);
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

static XrCFuncResult vm_par_lane_call_next(XrVMRuntime *isolate, XrVmParLane *lane,
                                           XrValue *result);

static XrCFuncResult vm_par_lane_continue(XrVMRuntime *isolate, int status, XrValue resume_value,
                                          void *ctx, XrValue *result) {
    XrVmParLane *lane = (XrVmParLane *) ctx;
    if (!lane || !lane->batch)
        return XR_CFUNC_ERROR;
    if (status == XR_RESUME_CLOSURE_ERROR || status == XR_RESUME_ERROR ||
        status == XR_RESUME_CANCELLED) {
        atomic_store_explicit(&lane->batch->failed, true, memory_order_release);
        vm_par_lane_done(lane);
        if (result)
            *result = xr_null();
        return XR_CFUNC_DONE;
    }
    XrVmParBatch *batch = lane->batch;
    if (batch->map_body) {
        XrArray *output = batch->output;
        int64_t out_index = lane->iter - batch->start;
        if (!output || output->elem_type == XR_ELEM_ANY || out_index < 0 ||
            out_index >= output->length || out_index > INT32_MAX ||
            (output->elem_type == XR_ELEM_CHAR && !XR_IS_CHAR(resume_value))) {
            atomic_store_explicit(&batch->failed, true, memory_order_release);
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
        vm_par_lane_done(lane);
        if (result)
            *result = xr_null();
        return XR_CFUNC_DONE;
    }

    lane->call_args[0] = xr_int(lane->iter);
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
                                      XrArray *output, int64_t start, int64_t end_excl,
                                      int lane_count, bool range_body, bool map_body) {
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
    batch->output = output;
    batch->start = start;
    batch->lane_count = lane_count;
    batch->range_body = range_body;
    batch->map_body = map_body;
    atomic_store_explicit(&batch->failed, false, memory_order_relaxed);

    batch->lanes = (XrVmParLane *) xr_calloc((size_t) lane_count, sizeof(XrVmParLane));
    batch->coros = (XrCoroutine **) xr_calloc((size_t) lane_count, sizeof(XrCoroutine *));
    batch->latch = xr_countdown_latch_new(core, runtime, lane_count);
    if (!batch->lanes || !batch->coros || !batch->latch) {
        vm_par_batch_free(batch);
        return NULL;
    }

    int64_t base = item_count / lane_count;
    int64_t rem = item_count % lane_count;
    int64_t cursor = start;
    for (int lane = 0; lane < lane_count; lane++) {
        int64_t lane_items = base + (lane < rem ? 1 : 0);
        XrVmParLane *slot = &batch->lanes[lane];
        slot->batch = batch;
        slot->lane_id = lane;
        slot->iter = cursor;
        slot->limit = cursor + lane_items;
        cursor += lane_items;

        XrValue args[2] = {vm_par_batch_to_value(batch), xr_int(lane)};
        batch->coros[lane] =
            xr_coro_create_vm_cfunc(isolate, vm_par_lane_entry, args, 2,
                                    map_body ? "parallel.map.lane" : "parallel.forEach.lane");
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
                                              XrVmParBatch *batch, bool resume) {
    (void) isolate;
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
        VM_THROW(frame, pc, XR_ERR_RUNTIME, "parallel.forEach wait failed");
    }
    bool failed = atomic_load_explicit(&batch->failed, memory_order_acquire);
    XrArray *map_output = (!failed && batch->map_body) ? batch->output : NULL;
    vm_par_batch_free(batch);
    *batch_slot = xr_null();
    if (failed) {
        VM_THROW(frame, pc, XR_ERR_RUNTIME, "parallel lane failed");
    }
    if (map_output)
        XR_ARRAY_MARK_MUTATED(map_output);
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
    XrValue *batch_slot = &base[arg_base + 4];

    XrVmParBatch *resume_batch = vm_par_batch_from_value(*batch_slot);
    if (resume_batch)
        return vm_par_wait_for_batch(isolate, vm_ctx, frame, pc, handled_slot, batch_slot,
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
    int lane_count = vm_par_resolve_lanes(runtime, item_count, workers);
    if (lane_count <= 1)
        return XR_DISP_NEXT;

    XrClosure *closure = (XrClosure *) XR_TO_PTR(*closure_slot);
    if (!vm_par_closure_safe_to_share(closure))
        return XR_DISP_NEXT;

    XrVmParBatch *batch =
        vm_par_batch_new(isolate, runtime, closure, NULL, start, end_excl, lane_count,
                         (flags & XR_VM_PAR_FLAG_RANGE_BODY) != 0, false);
    if (!batch) {
        VM_THROW(frame, pc, XR_ERR_OUT_OF_MEMORY, "parallel.forEach batch allocation failed");
    }

    *batch_slot = vm_par_batch_to_value(batch);
    xr_runtime_spawn_batch(runtime, batch->coros, batch->lane_count);
    return vm_par_wait_for_batch(isolate, vm_ctx, frame, pc, handled_slot, batch_slot, batch,
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
    XrValue *batch_slot = &base[arg_base + 5];

    XrVmParBatch *resume_batch = vm_par_batch_from_value(*batch_slot);
    if (resume_batch)
        return vm_par_wait_for_batch(isolate, vm_ctx, frame, pc, handled_slot, batch_slot,
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
    int lane_count = vm_par_resolve_lanes(runtime, item_count, workers);
    if (lane_count <= 1)
        return XR_DISP_NEXT;

    XrClosure *closure = (XrClosure *) XR_TO_PTR(*closure_slot);
    if (!vm_par_closure_safe_to_share(closure))
        return XR_DISP_NEXT;

    XrVmParBatch *batch = vm_par_batch_new(isolate, runtime, closure, output, start, end_excl,
                                           lane_count, false, true);
    if (!batch) {
        VM_THROW(frame, pc, XR_ERR_OUT_OF_MEMORY, "parallel.map batch allocation failed");
    }

    *batch_slot = vm_par_batch_to_value(batch);
    xr_runtime_spawn_batch(runtime, batch->coros, batch->lane_count);
    return vm_par_wait_for_batch(isolate, vm_ctx, frame, pc, handled_slot, batch_slot, batch,
                                 false);
}
