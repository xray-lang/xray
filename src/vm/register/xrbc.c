/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc.c - Scalar register interpreter with movable, indexed frames.
 *
 * KEY CONCEPT:
 *   VM calls stay in one C dispatch loop. Frame and register locations are
 *   indices; no pointer into either growing array survives a call.
 */

#include "xrbc.h"
#include "base/xchecks.h"
#include "base/xmalloc.h"

typedef struct {
    size_t function, pc, base;
    uint32_t destination;
} XrRbcFrame;

typedef struct {
    uint64_t *slots;
    size_t slot_capacity;
    XrRbcFrame *frames;
    size_t frame_capacity, depth;
} XrRbcStack;

struct XrRbcOutcome {
    XrRbcStatus status;
    uint64_t bits;
    XrtExecutionFailure failure;
    XrtLocationOwner *locations;
};

struct XrRbcExecution {
    const XrRbcModule *module;
    XrRbcStack stack;
    XrRbcOutcome *pending;
    bool consumed;
};

static bool xr_rbc_grow(void **storage, size_t *capacity, size_t count, size_t item_size) {
    if (count <= *capacity)
        return true;
    if (count > SIZE_MAX / item_size)
        return false;
    size_t next = *capacity ? *capacity : 16;
    while (next < count) {
        if (next > SIZE_MAX / 2) {
            next = count;
            break;
        }
        next *= 2;
    }
    if (next > SIZE_MAX / item_size)
        next = count;
    void *grown = xr_realloc(*storage, next * item_size);
    if (!grown)
        return false;
    *storage = grown;
    *capacity = next;
    return true;
}

static bool xr_rbc_reserve(XrRbcStack *stack, size_t slots, size_t frames) {
    // Use real void* objects, not a type-punned pointer-to-pointer.
    void *storage = stack->slots;
    if (!xr_rbc_grow(&storage, &stack->slot_capacity, slots, sizeof(uint64_t)))
        return false;
    stack->slots = storage;
    storage = stack->frames;
    if (!xr_rbc_grow(&storage, &stack->frame_capacity, frames, sizeof(XrRbcFrame)))
        return false;
    stack->frames = storage;
    return true;
}

static bool xr_rbc_function_valid(const XrRbcFunction *function) {
    return function->code && function->code_count && function->register_count &&
           function->register_count <= XR_RBC_REGISTER_COUNT_MAX &&
           function->parameter_count <= function->register_count &&
           (!function->constant_count || function->constants);
}

static int64_t xr_rbc_signed(uint64_t bits) {
    // Avoid implementation-defined unsigned-to-signed conversion.
    return bits <= INT64_MAX ? (int64_t) bits : -1 - (int64_t) ~bits;
}

static uint64_t xr_rbc_shift_right(uint64_t bits, uint64_t amount) {
    unsigned shift = (unsigned) (amount & 63);
    uint64_t result = bits >> shift;
    if (shift && (bits >> 63))
        result |= UINT64_MAX << (64 - shift);
    return result;
}

static XrRbcStatus xr_rbc_call(const XrRbcModule *module, XrRbcStack *stack, uint32_t destination,
                               uint32_t target) {
    if (target >= module->function_count || !xr_rbc_function_valid(&module->functions[target]))
        return XR_RBC_INVALID;
    const XrRbcFunction *callee = &module->functions[target];
    XrRbcFrame caller = stack->frames[stack->depth - 1];
    if ((size_t) destination + 1 + callee->parameter_count !=
        module->functions[caller.function].register_count)
        return XR_RBC_INVALID;
    if (caller.base > SIZE_MAX - destination - 1 || stack->depth == SIZE_MAX)
        return XR_RBC_OUT_OF_MEMORY;
    size_t base = caller.base + destination + 1;
    if (base > SIZE_MAX - callee->register_count ||
        !xr_rbc_reserve(stack, base + callee->register_count, stack->depth + 1))
        return XR_RBC_OUT_OF_MEMORY;
    // Only arguments are live on entry; clearing the rest prevents stale
    // bits from an earlier invocation from becoming observable.
    memset(stack->slots + base + callee->parameter_count, 0,
           (callee->register_count - callee->parameter_count) * sizeof(uint64_t));
    stack->frames[stack->depth++] = (XrRbcFrame) {target, 0, base, destination};
    return XR_RBC_OK;
}

static XrRbcStatus xr_rbc_execute(const XrRbcModule *module, XrRbcStack *stack,
    const XrtLocationOwner *locations, uint64_t *result) {
    const XrRbcFunction *function;
    size_t base, pc;
    uint32_t instruction, op, a, b, c;
    XrRbcStatus status;
    // Dispatch mechanics follow the existing register interpreter: each
    // handler owns its next fetch on compilers supporting labels as values.
#include "xrbc_dispatch.inc.c"
}

XR_FUNC XrRbcStatus xr_rbc_module_prepare(const XrRbcModule *module, uint32_t entry,
    const uint64_t *arguments, size_t argument_count, XrtLocationOwner *locations,
    XrRbcExecution **output) {
    if (!output || *output || !locations || !module || !module->functions ||
        entry >= module->function_count || (argument_count && !arguments) ||
        (module->native_count && !module->natives) || module->native_count > 65536)
        return XR_RBC_INVALID;
    const XrRbcFunction *function = &module->functions[entry];
    if (!xr_rbc_function_valid(function) || argument_count != function->parameter_count)
        return XR_RBC_INVALID;
    for (size_t i = 0; i < module->native_count; ++i)
        if (module->natives[i] != xrt_print_i64)
            return XR_RBC_INVALID;
    XrRbcExecution *execution = xr_calloc(1, sizeof(*execution));
    if (!execution) return XR_RBC_OUT_OF_MEMORY;
    execution->module = module;
    execution->pending = xr_calloc(1, sizeof(*execution->pending));
    if (!execution->pending ||
        !xr_rbc_reserve(&execution->stack, function->register_count, 1) ||
        !xrt_locations_retain(locations)) {
        xr_rbc_execution_free(&execution);
        return XR_RBC_OUT_OF_MEMORY;
    }
    execution->pending->locations = locations;
    execution->pending->status = XR_RBC_INVALID;
    memset(execution->stack.slots, 0, function->register_count * sizeof(uint64_t));
    if (argument_count)
        memcpy(execution->stack.slots, arguments, argument_count * sizeof(uint64_t));
    execution->stack.frames[0] = (XrRbcFrame) {entry, 0, 0, 0};
    execution->stack.depth = 1;
    *output = execution;
    return XR_RBC_OK;
}

static void xr_rbc_root(void *context) {
    XrRbcExecution *execution = context;
    execution->pending->status = xr_rbc_execute(execution->module, &execution->stack,
        execution->pending->locations, &execution->pending->bits);
}

static void xr_rbc_stack_free(XrRbcStack *stack) {
    xr_free(stack->frames);
    xr_free(stack->slots);
    *stack = (XrRbcStack) {0};
}

XR_FUNC XrRbcOutcome *xr_rbc_execution_run(XrRbcExecution *execution) {
    if (!execution || execution->consumed) return NULL;
    execution->consumed = true;
    XrRbcOutcome *outcome = execution->pending;
    XrtExecutionStatus status = xrt_run_embedded(outcome->locations, &outcome->failure,
        xr_rbc_root, execution);
    if (status == XRT_EXECUTION_FAILURE && outcome->failure.kind == XRT_FAILURE_OUTPUT)
        outcome->status = XR_RBC_OUTPUT_FAILURE;
    else {
        // No arithmetic panic producer is admitted by this interpreter yet.
        if (status != XRT_EXECUTION_COMPLETED) outcome->status = XR_RBC_INVALID;
        xrt_locations_release(outcome->locations);
        outcome->locations = NULL;
    }
    // The common boundary has restored its caller before reclaiming the sole
    // heap stack descriptor, including allocations made by skipped VM frames.
    xr_rbc_stack_free(&execution->stack);
    execution->pending = NULL;
    return outcome;
}

XR_FUNC void xr_rbc_execution_free(XrRbcExecution **execution) {
    if (!execution || !*execution) return;
    XrRbcExecution *owned = *execution;
    *execution = NULL;
    xr_rbc_stack_free(&owned->stack);
    xr_rbc_outcome_free(&owned->pending);
    xr_free(owned);
}

XR_FUNC XrRbcStatus xr_rbc_outcome_status(const XrRbcOutcome *outcome) {
    return outcome ? outcome->status : XR_RBC_INVALID;
}

XR_FUNC bool xr_rbc_outcome_bits(const XrRbcOutcome *outcome, uint64_t *output) {
    if (!outcome || outcome->status != XR_RBC_OK || !output) return false;
    *output = outcome->bits;
    return true;
}

XR_FUNC bool xr_rbc_outcome_output_failure(const XrRbcOutcome *outcome,
    XrtOutputFailure *facts, const XrtLocationOwner **borrowed_locations) {
    if (!outcome || outcome->status != XR_RBC_OUTPUT_FAILURE || !facts || !borrowed_locations)
        return false;
    *facts = outcome->failure.detail.output;
    *borrowed_locations = outcome->locations;
    return true;
}

XR_FUNC void xr_rbc_outcome_free(XrRbcOutcome **outcome) {
    if (!outcome || !*outcome) return;
    XrRbcOutcome *owned = *outcome;
    *outcome = NULL;
    xrt_locations_release(owned->locations);
    xr_free(owned);
}

XR_FUNC const char *xr_rbc_dispatch_name(void) {
#if defined(__clang__) || defined(__GNUC__)
    return "computed-goto";
#else
    return "switch";
#endif
}
