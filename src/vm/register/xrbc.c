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

typedef union {
    uint64_t bits;
    XrtString *string;
} XrRbcRegister;

typedef struct {
    size_t function, pc, base;
    uint32_t destination;
    size_t cell_base;
    uint32_t initialized_cells;
} XrRbcFrame;

typedef struct {
    XrRbcRegister *slots;
    size_t slot_capacity;
    XrRbcFrame *frames;
    size_t frame_capacity, depth;
    XrRbcRegister *cells;
    size_t cell_capacity, cell_count;
} XrRbcStack;

struct XrRbcOutcome {
    XrRbcStatus status;
    XrRbcRegister value;
    XrRbcRegisterKind result_kind;
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

static bool xr_rbc_reserve(XrRbcStack *stack, size_t slots, size_t frames, size_t cells) {
    // Use real void* objects, not a type-punned pointer-to-pointer.
    void *storage = stack->slots;
    if (!xr_rbc_grow(&storage, &stack->slot_capacity, slots, sizeof(*stack->slots)))
        return false;
    stack->slots = storage;
    storage = stack->frames;
    if (!xr_rbc_grow(&storage, &stack->frame_capacity, frames, sizeof(XrRbcFrame)))
        return false;
    stack->frames = storage;
    storage = stack->cells;
    if (!xr_rbc_grow(&storage, &stack->cell_capacity, cells, sizeof(*stack->cells)))
        return false;
    stack->cells = storage;
    return true;
}

static bool xr_rbc_function_valid(const XrRbcFunction *function) {
    if (!(function->code && function->code_count && function->register_count &&
           function->register_count <= XR_RBC_REGISTER_COUNT_MAX &&
           function->parameter_count <= function->register_count &&
           function->cell_count <= 65536 &&
           (!function->constant_count || function->constants))) return false;
    if ((function->register_kinds ? function->register_kind_count != function->register_count :
         function->register_kind_count != 0) ||
        (function->cell_kinds ? function->cell_kind_count != function->cell_count :
         function->cell_kind_count != 0) ||
        (function->result_kind != XR_RBC_BITS && function->result_kind != XR_RBC_STRING)) return false;
    for (uint32_t i = 0; function->register_kinds && i < function->register_count; ++i)
        if ((unsigned)function->register_kinds[i] > XR_RBC_CALL_WINDOW ||
            (i < function->parameter_count && function->register_kinds[i] == XR_RBC_CALL_WINDOW)) return false;
    for (uint32_t i = 0; function->cell_kinds && i < function->cell_count; ++i)
        if (function->cell_kinds[i] != XR_RBC_BITS && function->cell_kinds[i] != XR_RBC_STRING) return false;
    return true;
}

static bool xr_rbc_instruction_valid(const XrRbcModule *module,
    const XrRbcFunction *function, size_t pc) {
    uint32_t word = function->code[pc], op = word & 255;
    uint32_t a = (word >> 8) & 255, b = (word >> 16) & 255, c = word >> 24;
    uint32_t index = word >> 16, registers = function->register_count;
    size_t remaining = function->code_count - pc;
    bool comparison = op == XR_RBC_LT || op == XR_RBC_LE || op == XR_RBC_EQ;
    switch (op) {
    case XR_RBC_MOV: case XR_RBC_NEG: case XR_RBC_CELL_READ: case XR_RBC_CELL_WRITE:
    case XR_RBC_STRING_I64: case XR_RBC_STRING_RETAIN: case XR_RBC_STRING_TAKE:
    case XR_RBC_LT: case XR_RBC_LE: case XR_RBC_EQ:
        if (a >= registers || b >= registers || c) return false;
        if (comparison && (remaining <= 2 || (function->code[pc + 1] & 255) != XR_RBC_JMP))
            return false;
        break;
    case XR_RBC_STRING_CONCAT:
    case XR_RBC_ADD: case XR_RBC_SUB: case XR_RBC_MUL:
    case XR_RBC_DIV: case XR_RBC_REM: case XR_RBC_AND: case XR_RBC_OR:
    case XR_RBC_XOR: case XR_RBC_SHL: case XR_RBC_SHR: case XR_RBC_USHR:
        if (a >= registers || b >= registers || c >= registers) return false;
        break;
    case XR_RBC_LOADK:
        if (a >= registers || index >= function->constant_count) return false;
        break;
    case XR_RBC_LOADI:
        if (a >= registers) return false;
        break;
    case XR_RBC_PRINT_LITERAL:
        if (a || index >= module->literal_count) return false;
        break;
    case XR_RBC_STRING_LITERAL:
        if (a >= registers || index >= module->literal_count) return false;
        break;
    case XR_RBC_STRING_CELL_NEW:
        if (a >= registers || b >= registers || c >= function->cell_count) return false;
        break;
    case XR_RBC_STRING_DROP: case XR_RBC_STRING_PRINT: case XR_RBC_STRING_CELL_DROP:
        if (a >= registers || (word >> 16)) return false;
        break;
    case XR_RBC_CELL_NEW:
        if (a >= registers || index >= function->cell_count) return false;
        break;
    case XR_RBC_JMP: {
        uint32_t bits = word >> 8;
        int32_t offset = bits <= XR_RBC_JUMP_MAX ? (int32_t) bits : (int32_t) bits - 16777216;
        size_t next = pc + 1;
        if (offset < 0) return (size_t) -offset <= next;
        return (size_t) offset < function->code_count - next;
    }
    case XR_RBC_CALL:
        if (a >= registers || index >= module->function_count ||
            a + 1 + module->functions[index].parameter_count != registers) return false;
        break;
    case XR_RBC_CALLN:
        if (index >= module->native_count ||
            a + (module->natives[index] == XR_RBC_NATIVE_PRINT_I64 ||
                module->natives[index] == XR_RBC_NATIVE_UTC_OFFSET_AT ? 2u : 1u) >= registers)
            return false;
        break;
    case XR_RBC_RET:
        return a < registers && !(word >> 16);
    case XR_RBC_RET0:
        return word == XR_RBC_RET0;
    default:
        return false;
    }
    // Every possible successor must remain within the validated function,
    // even when the instruction or comparison's false edge is unreachable.
    return remaining > (comparison ? 2u : 1u);
}

#include "xrbc_string_admission.inc.c"

static bool xr_rbc_module_valid(const XrRbcModule *module) {
    for (size_t i = 0; i < module->literal_count; ++i)
        if (module->literals[i].length == SIZE_MAX ||
            (module->literals[i].length && !module->literals[i].bytes)) return false;
    // Validate descriptors before using any call target's parameter count.
    for (size_t f = 0; f < module->function_count; ++f)
        if (!xr_rbc_function_valid(&module->functions[f])) return false;
    for (size_t f = 0; f < module->function_count; ++f) {
        const XrRbcFunction *function = &module->functions[f];
        for (size_t pc = 0; pc < function->code_count; ++pc)
            if (!xr_rbc_instruction_valid(module, function, pc)) return false;
        if (!xr_rbc_strings_valid(module, function)) return false;
    }
    return true;
}

static bool xr_rbc_source_maps_valid(const XrRbcModule *module, const XrtLocationOwner *locations) {
    // An unreachable callee must not hide invalid IDs until after guest effects.
    size_t count = xrt_locations_count(locations);
    for (size_t f = 0; f < module->function_count; ++f) {
        const XrRbcFunction *function = &module->functions[f];
        if (!function->source_ids) {
            if (function->source_id_count) return false;
            continue;
        }
        if (!function->source_id_count || function->source_id_count != function->code_count)
            return false;
        for (size_t pc = 0; pc < function->source_id_count; ++pc)
            if (function->source_ids[pc] > count) return false;
    }
    return true;
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
    const XrRbcFunction *callee = &module->functions[target];
    XrRbcFrame caller = stack->frames[stack->depth - 1];
    if (caller.base > SIZE_MAX - destination - 1 || stack->depth == SIZE_MAX)
        return XR_RBC_OUT_OF_MEMORY;
    size_t base = caller.base + destination + 1;
    size_t cell_base = stack->cell_count;
    if (base > SIZE_MAX - callee->register_count ||
        cell_base > SIZE_MAX - callee->cell_count ||
        !xr_rbc_reserve(stack, base + callee->register_count, stack->depth + 1,
            cell_base + callee->cell_count))
        return XR_RBC_OUT_OF_MEMORY;
    // Only arguments are live on entry; clearing the rest prevents stale
    // bits from an earlier invocation from becoming observable.
    memset(stack->slots + base + callee->parameter_count, 0,
           (callee->register_count - callee->parameter_count) * sizeof(*stack->slots));
    stack->frames[stack->depth++] = (XrRbcFrame) {target, 0, base, destination, cell_base, 0};
    stack->cell_count = cell_base + callee->cell_count;
    return XR_RBC_OK;
}

static XrRbcRegister *xr_rbc_cell(XrRbcStack *stack, const XrRbcModule *module,
    uint64_t handle, XrRbcRegisterKind expected) {
    if (!handle || handle > stack->cell_count) return NULL;
    size_t index = (size_t)handle - 1;
    // Reserved capacity does not establish a cell's lifetime. Only an active
    // owner's initialized prefix can authorize a read or write.
    for (size_t depth = stack->depth; depth; --depth) {
        const XrRbcFrame *frame = &stack->frames[depth - 1];
        if (index >= frame->cell_base && index - frame->cell_base < frame->initialized_cells) {
            const XrRbcFunction *owner = &module->functions[frame->function];
            XrRbcRegisterKind actual = owner->cell_kinds ? owner->cell_kinds[index - frame->cell_base] : XR_RBC_BITS;
            if (actual != expected) return NULL;
            return &stack->cells[index];
        }
    }
    return NULL;
}

static XrRbcStatus xr_rbc_execute(const XrRbcModule *module, XrRbcStack *stack,
    const XrtLocationOwner *locations, XrRbcRegister *result) {
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
        (module->native_count && !module->natives) || module->native_count > 65536 ||
        (module->literal_count && !module->literals) || module->literal_count > 65536)
        return XR_RBC_INVALID;
    const XrRbcFunction *function = &module->functions[entry];
    if (argument_count != function->parameter_count || !xr_rbc_module_valid(module) ||
        !xr_rbc_source_maps_valid(module, locations))
        return XR_RBC_INVALID;
    for (size_t p = 0; function->register_kinds && p < argument_count; ++p)
        if (function->register_kinds[p] != XR_RBC_BITS) return XR_RBC_INVALID;
    for (size_t i = 0; i < module->native_count; ++i)
        if ((unsigned)module->natives[i] >= XR_RBC_NATIVE_COUNT)
            return XR_RBC_INVALID;
    XrRbcExecution *execution = xr_calloc(1, sizeof(*execution));
    if (!execution) return XR_RBC_OUT_OF_MEMORY;
    execution->module = module;
    execution->pending = xr_calloc(1, sizeof(*execution->pending));
    if (!execution->pending ||
        !xr_rbc_reserve(&execution->stack, function->register_count, 1, function->cell_count) ||
        !xrt_locations_retain(locations)) {
        xr_rbc_execution_free(&execution);
        return XR_RBC_OUT_OF_MEMORY;
    }
    execution->pending->locations = locations;
    execution->pending->status = XR_RBC_INVALID;
    execution->pending->result_kind = function->result_kind;
    memset(execution->stack.slots, 0, function->register_count * sizeof(*execution->stack.slots));
    if (argument_count)
        for (size_t i = 0; i < argument_count; ++i) execution->stack.slots[i].bits = arguments[i];
    execution->stack.frames[0] = (XrRbcFrame) {entry, 0, 0, 0, 0, 0};
    execution->stack.depth = 1;
    execution->stack.cell_count = function->cell_count;
    *output = execution;
    return XR_RBC_OK;
}

static void xr_rbc_root(void *context) {
    XrRbcExecution *execution = context;
    execution->pending->status = xr_rbc_execute(execution->module, &execution->stack,
        execution->pending->locations, &execution->pending->value);
    if (execution->pending->status == XR_RBC_OK && execution->pending->result_kind == XR_RBC_STRING)
        execution->pending->value.string = xrt_string_root_take(execution->pending->value.string);
}

static void xr_rbc_stack_free(XrRbcStack *stack) {
    xr_free(stack->cells);
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
    else if (status == XRT_EXECUTION_FAILURE && outcome->failure.kind == XRT_FAILURE_ARITHMETIC_PANIC)
        outcome->status = XR_RBC_ARITHMETIC_PANIC;
    else if (status == XRT_EXECUTION_FAILURE && outcome->failure.kind == XRT_FAILURE_HOST_CLOCK)
        outcome->status = XR_RBC_HOST_CLOCK_FAILURE;
    else if (status == XRT_EXECUTION_FAILURE && outcome->failure.kind == XRT_FAILURE_VALUE)
        outcome->status = XR_RBC_VALUE_FAILURE;
    else {
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
    if (!outcome || outcome->status != XR_RBC_OK || !output || outcome->result_kind == XR_RBC_STRING) return false;
    *output = outcome->value.bits;
    return true;
}
XR_FUNC bool xr_rbc_outcome_string(const XrRbcOutcome *outcome, const XrtString **borrowed) {
    if (!outcome || outcome->status != XR_RBC_OK || outcome->result_kind != XR_RBC_STRING ||
        !outcome->value.string || !borrowed) return false;
    *borrowed = outcome->value.string;
    return true;
}
XR_FUNC bool xr_rbc_outcome_take_string(XrRbcOutcome *outcome, XrtString **output) {
    if (!outcome || outcome->status != XR_RBC_OK || outcome->result_kind != XR_RBC_STRING ||
        !outcome->value.string || !output || *output) return false;
    *output = outcome->value.string;
    outcome->value.string = NULL;
    return true;
}
XR_FUNC bool xr_rbc_outcome_value_failure(const XrRbcOutcome *outcome,
    XrXirValueStatus *status, uint32_t *source_id, const XrtLocationOwner **locations) {
    if (!outcome || outcome->status != XR_RBC_VALUE_FAILURE || !status || !source_id || !locations) return false;
    *status = outcome->failure.detail.value.status;
    *source_id = outcome->failure.detail.value.source_id;
    *locations = outcome->locations;
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

XR_FUNC bool xr_rbc_outcome_arithmetic_panic(const XrRbcOutcome *outcome,
    XrtArithmeticPanic *facts, const XrtLocationOwner **borrowed_locations) {
    if (!outcome || outcome->status != XR_RBC_ARITHMETIC_PANIC || !facts || !borrowed_locations)
        return false;
    *facts = outcome->failure.detail.panic;
    *borrowed_locations = outcome->locations;
    return true;
}

XR_FUNC bool xr_rbc_outcome_host_clock_failure(const XrRbcOutcome *outcome,
    XrtHostClockFailure *facts, const XrtLocationOwner **borrowed_locations) {
    if (!outcome || outcome->status != XR_RBC_HOST_CLOCK_FAILURE || !facts || !borrowed_locations)
        return false;
    *facts = outcome->failure.detail.clock;
    *borrowed_locations = outcome->locations;
    return true;
}

XR_FUNC void xr_rbc_outcome_free(XrRbcOutcome **outcome) {
    if (!outcome || !*outcome) return;
    XrRbcOutcome *owned = *outcome;
    *outcome = NULL;
    if (owned->result_kind == XR_RBC_STRING) xrt_string_owned_drop(&owned->value.string);
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
