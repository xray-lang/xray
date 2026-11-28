/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_ctfe.inc.c - Private compiler evaluation using register VM semantics.
 */

typedef struct RbcCtfeRun {
    XrRbcModule module;
    XrRbcStack stack;
    const XrtHostExecutionControl *control;
    uint64_t fuel;
    size_t memory_bytes;
    XrRbcRegister result;
    XrRbcCtfeStatus status;
    uint32_t source_id;
} RbcCtfeRun;

static bool rbc_ctfe_fail(RbcCtfeRun *run, XrRbcCtfeStatus status, uint32_t source_id) {
    run->status = status;
    run->source_id = source_id;
    return false;
}

static bool rbc_ctfe_current(RbcCtfeRun *run, uint32_t source_id) {
    return !run->control || run->control->current(run->control->context) ||
        rbc_ctfe_fail(run, XR_RBC_CTFE_CANCELLED, source_id);
}

static bool rbc_ctfe_charge(RbcCtfeRun *run, uint32_t source_id) {
    if (!rbc_ctfe_current(run, source_id)) return false;
    if (!run->fuel) return rbc_ctfe_fail(run, XR_RBC_CTFE_FUEL_LIMIT, source_id);
    --run->fuel;
    return true;
}

static bool rbc_ctfe_fits(size_t bytes, size_t slots, size_t frames) {
    return slots <= bytes / sizeof(XrRbcRegister) &&
        frames <= (bytes - slots * sizeof(XrRbcRegister)) / sizeof(XrRbcFrame);
}

static size_t rbc_ctfe_capacity(size_t current, size_t wanted) {
    if (wanted <= current) return current;
    return current <= SIZE_MAX / 2 && current * 2 > wanted ? current * 2 : wanted;
}

static bool rbc_ctfe_reserve(RbcCtfeRun *run, size_t slots, size_t frames, uint32_t source_id) {
    XrRbcStack *stack = &run->stack;
    if (slots <= stack->slot_capacity && frames <= stack->frame_capacity) return true;
    size_t slot_capacity = rbc_ctfe_capacity(stack->slot_capacity, slots);
    size_t frame_capacity = rbc_ctfe_capacity(stack->frame_capacity, frames);
    if (!rbc_ctfe_fits(run->memory_bytes, slot_capacity, frame_capacity)) {
        if (slots < stack->slot_capacity) slots = stack->slot_capacity;
        if (frames < stack->frame_capacity) frames = stack->frame_capacity;
        if (!rbc_ctfe_fits(run->memory_bytes, slots, frames))
            return rbc_ctfe_fail(run, XR_RBC_CTFE_MEMORY_LIMIT, source_id);
        slot_capacity = slots;
        frame_capacity = frames;
    }
    if (slot_capacity != stack->slot_capacity) {
        XrRbcRegister *memory = xr_realloc(stack->slots, slot_capacity * sizeof(*stack->slots));
        if (!memory) return rbc_ctfe_fail(run, XR_RBC_CTFE_OUT_OF_MEMORY, source_id);
        stack->slots = memory;
        stack->slot_capacity = slot_capacity;
    }
    if (frame_capacity != stack->frame_capacity) {
        XrRbcFrame *memory = xr_realloc(stack->frames, frame_capacity * sizeof(*stack->frames));
        if (!memory) return rbc_ctfe_fail(run, XR_RBC_CTFE_OUT_OF_MEMORY, source_id);
        stack->frames = memory;
        stack->frame_capacity = frame_capacity;
    }
    return true;
}

static bool rbc_ctfe_call_reserve(RbcCtfeRun *run, size_t base, const XrRbcFunction *callee,
    uint32_t source_id) {
    const XrRbcStack *stack = &run->stack;
    if (base > SIZE_MAX - callee->register_count || stack->depth == SIZE_MAX)
        return rbc_ctfe_fail(run, XR_RBC_CTFE_MEMORY_LIMIT, source_id);
    return rbc_ctfe_reserve(run, base + callee->register_count, stack->depth + 1, source_id);
}

static void rbc_ctfe_clear(XrRbcRegister *registers, const XrRbcFunction *function) {
    uint32_t parameters = function->parameter_count;
    memset(registers + parameters, 0, (function->register_count - parameters) * sizeof(*registers));
}

static bool rbc_ctfe_opcode(uint32_t op) {
    switch (op) {
    case XR_RBC_MOV: case XR_RBC_LOADK: case XR_RBC_LOADI:
    case XR_RBC_ADD: case XR_RBC_SUB: case XR_RBC_MUL: case XR_RBC_NEG:
    case XR_RBC_DIV: case XR_RBC_REM: case XR_RBC_REMI:
    case XR_RBC_AND: case XR_RBC_OR: case XR_RBC_XOR: case XR_RBC_SHL: case XR_RBC_SHR: case XR_RBC_USHR:
    case XR_RBC_ADDI: case XR_RBC_SUBI: case XR_RBC_MULI: case XR_RBC_ZEXT: case XR_RBC_SEXT:
    case XR_RBC_LT: case XR_RBC_LE: case XR_RBC_EQ: case XR_RBC_LTU: case XR_RBC_LEU:
    case XR_RBC_LTI: case XR_RBC_LEI: case XR_RBC_GTI: case XR_RBC_GEI: case XR_RBC_EQI:
    case XR_RBC_JMP: case XR_RBC_CALL: case XR_RBC_RET: case XR_RBC_RET0: case XR_RBC_ARITHMETIC_PANIC:
        return true;
    default:
        return false;
    }
}

static bool rbc_ctfe_function(const XrRbcFunction *function) {
    if (function->result_kind != XR_RBC_BITS || function->cell_count || function->spill_count ||
        function->state_access_count || function->array_access_count || function->array_operand_count ||
        function->aggregate_value_count || function->aggregate_access_count || function->aggregate_operand_count ||
        function->aggregate_size || function->aggregate_parameter_count || function->aggregate_result_type ||
        function->aggregate_argument_count || function->callable || function->register_callable_count ||
        function->capture_count || function->state_capture || function->cleanup_only ||
        function->environment_count || function->environment_size) return false;
    if (!xr_rbc_function_valid(function)) return false;
    for (uint32_t index = 0; function->register_kinds && index < function->register_count; ++index)
        if (function->register_kinds[index] != XR_RBC_BITS &&
            function->register_kinds[index] != XR_RBC_CALL_WINDOW) return false;
    return true;
}

static bool rbc_ctfe_admit(RbcCtfeRun *run, const XrRbcCtfeRequest *request) {
    size_t words = request->code_words;
    for (size_t index = 0; index < request->function_count; ++index) {
        const XrRbcFunction *function = &request->functions[index];
        if (!function->code || !function->code_count)
            return rbc_ctfe_fail(run, XR_RBC_CTFE_INVALID, 0);
        if (function->code_count > words) return rbc_ctfe_fail(run, XR_RBC_CTFE_CODE_LIMIT, 0);
        words -= function->code_count;
    }
    if (!xr_rbc_source_maps_valid(&run->module, request->locations))
        return rbc_ctfe_fail(run, XR_RBC_CTFE_INVALID, 0);
    for (size_t index = 0; index < request->function_count; ++index) {
        const XrRbcFunction *function = &request->functions[index];
        for (size_t pc = 0; pc < function->code_count; ++pc) {
            uint32_t op = function->code[pc] & 255;
            if (!rbc_ctfe_opcode(op))
                return rbc_ctfe_fail(run, op < XR_RBC_OP_COUNT ? XR_RBC_CTFE_UNSUPPORTED : XR_RBC_CTFE_INVALID,
                    function->source_ids ? function->source_ids[pc] : 0);
        }
        if (!rbc_ctfe_function(function))
            return rbc_ctfe_fail(run, XR_RBC_CTFE_UNSUPPORTED, function->source_ids ? function->source_ids[0] : 0);
    }
    XrRbcStatus status = xr_rbc_module_admit(&run->module, request->locations);
    if (status != XR_RBC_OK)
        return rbc_ctfe_fail(run, status == XR_RBC_OUT_OF_MEMORY ? XR_RBC_CTFE_OUT_OF_MEMORY : XR_RBC_CTFE_INVALID, 0);
    return true;
}

static XrRbcStatus xr_rbc_execute_ctfe(RbcCtfeRun *run) {
    const XrRbcModule *module = &run->module;
    XrRbcStack *stack = &run->stack;
    XrRbcRegister *result = &run->result;
    const XrRbcFunction *function;
    const uint32_t *code;
    const uint64_t *constants;
    XrRbcFrame *frame;
    XrRbcRegister *registers;
    size_t pc;
    uint32_t instruction, op;
#define XR_RBC_CTFE_METERED
#include "xrbc_dispatch.inc.c"
#undef XR_RBC_CTFE_METERED
}

static void rbc_ctfe_root(void *context) {
    RbcCtfeRun *run = context;
    if (xr_rbc_execute_ctfe(run) != XR_RBC_OK && run->status == XR_RBC_CTFE_OK)
        run->status = XR_RBC_CTFE_INVALID;
}

static void rbc_ctfe_execute(RbcCtfeRun *run, const XrRbcCtfeRequest *request, uint32_t *panic_code) {
    const XrRbcFunction *entry = &request->functions[request->entry];
    uint32_t source_id = entry->source_ids ? entry->source_ids[0] : 0;
    if (!rbc_ctfe_charge(run, source_id) || !rbc_ctfe_reserve(run, entry->register_count, 1, source_id)) return;
    XrRbcStack *stack = &run->stack;
    stack->frames[0] = (XrRbcFrame){.function = request->entry};
    stack->depth = 1;
    rbc_ctfe_clear(stack->slots, entry);
    for (size_t index = 0; index < request->argument_count; ++index)
        stack->slots[index].bits = request->arguments[index];
    XrtExecutionFailure failure = {0};
    XrtExecutionStatus status = xrt_run_embedded(request->locations, &failure, rbc_ctfe_root, run);
    if (status == XRT_EXECUTION_FAILURE && failure.kind == XRT_FAILURE_TYPED_PANIC) {
        *panic_code = failure.detail.panic.code;
        rbc_ctfe_fail(run, XR_RBC_CTFE_PANIC, failure.detail.panic.source_id);
    } else if (status != XRT_EXECUTION_COMPLETED) {
        rbc_ctfe_fail(run, XR_RBC_CTFE_INVALID, 0);
    } else if (run->status == XR_RBC_CTFE_OK) {
        rbc_ctfe_current(run, source_id);
    }
    xrt_execution_failure_drop(&failure);
}

XR_FUNC XrRbcCtfeStatus xr_rbc_ctfe_evaluate(const XrRbcCtfeRequest *request, XrRbcCtfeOutcome *output) {
    if (!request || !output || output->bits || output->source_id || output->panic_code || output->locations ||
        !request->functions || request->function_count > UINT16_MAX + 1u || request->entry >= request->function_count ||
        !request->locations || (request->argument_count && !request->arguments) ||
        (request->control && !request->control->current) ||
        request->argument_count != request->functions[request->entry].parameter_count) return XR_RBC_CTFE_INVALID;
    if (!xrt_locations_retain(request->locations)) return XR_RBC_CTFE_OUT_OF_MEMORY;
    RbcCtfeRun run = {.module = {.functions = request->functions, .function_count = request->function_count},
        .memory_bytes = request->memory_bytes, .fuel = request->fuel, .control = request->control};
    uint32_t panic_code = 0;
    if (rbc_ctfe_current(&run, 0) && rbc_ctfe_admit(&run, request))
        rbc_ctfe_execute(&run, request, &panic_code);
    xr_rbc_stack_free(&run.stack);
    if (run.status == XR_RBC_CTFE_OK) output->bits = run.result.bits;
    if (run.source_id) {
        output->source_id = run.source_id;
        output->locations = request->locations;
    } else {
        xrt_locations_release(request->locations);
    }
    output->panic_code = panic_code;
    return run.status;
}

XR_FUNC void xr_rbc_ctfe_outcome_free(XrRbcCtfeOutcome *outcome) {
    if (!outcome) return;
    xrt_locations_release(outcome->locations);
    *outcome = (XrRbcCtfeOutcome){0};
}
