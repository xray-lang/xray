/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_aggregate_call.inc.c - By-value transfers between indexed activations
 */
static bool xr_rbc_callable_scalar_valid(XrRbcCallableValue value, uint64_t bits) {
    if (value.kind != XR_RBC_CALLABLE_SCALAR) return true;
    return (value.type != XR_POD_BOOL || bits <= 1) && (value.type != XR_POD_U8 || bits <= UINT8_MAX);
}

static bool xr_rbc_callable_arguments_valid(const XrRbcModule *module, const XrRbcFunction *callee,
    const XrRbcRegister *arguments) {
    if (!callee->callable) return true;
    XrRbcCallableType type = module->callable_types[callee->callable - 1];
    for (uint32_t p = 0; p < type.parameter_count; ++p) {
        XrRbcCallableValue parameter = type.parameters[p].value;
        if (!xr_rbc_callable_scalar_valid(parameter, arguments[p].bits)) return false;
    }
    return true;
}

static XR_NOINLINE XrRbcStatus xr_rbc_aggregate_control(const XrRbcModule *module, XrRbcStack *stack,
    const XrRbcFunction *function, uint32_t index, uint32_t source, size_t pc) {
    (void)source;
    XrRbcAggregateAccess access = function->aggregate_accesses[index];
    size_t parent = stack->depth - 1;
    XrRbcFrame *frame = &stack->frames[parent];
    if (access.operation == XR_RBC_FUNCTION_RETURN) {
        XrRbcRegister result = access.count ? stack->slots[frame->base + access.source] : (XrRbcRegister){0};
        if (!xr_rbc_callable_scalar_valid(module->callable_types[access.slot - 1].result, result.bits))
            return XR_RBC_INVALID;
        if (parent) stack->slots[stack->frames[parent - 1].base + frame->destination] = result;
        else stack->slots[0] = result;
        stack->cell_count = frame->cell_base;
        stack->spill_count = frame->spill_base;
        --stack->depth;
        return XR_RBC_OK;
    }
    if (access.operation == XR_RBC_FUNCTION_REFERENCE) {
        stack->slots[frame->base + access.result.index].bits = (uint64_t)access.callee + 1;
        frame->pc = pc;
        return XR_RBC_OK;
    }
    if (access.operation == XR_RBC_AGGREGATE_CALL || access.operation == XR_RBC_FUNCTION_CALL) {
        /* The caller area must precede the callee's spill_base even when the
         * first value in that area will only be produced by this return. */
        if (function->aggregate_size && !xr_rbc_aggregate_reserve(stack, frame, function)) return XR_RBC_OUT_OF_MEMORY;
        if (access.operation == XR_RBC_FUNCTION_CALL) {
            uint64_t target = stack->slots[frame->base + access.source].bits;
            XR_DCHECK(target && target <= module->function_count, "admitted function values name real code entries");
            access.callee = (uint32_t)(target - 1);
        }
        const XrRbcFunction *callee = &module->functions[access.callee];
        size_t base = frame->base + access.window + 1;
        if (!xr_rbc_callable_arguments_valid(module, callee, stack->slots + base)) return XR_RBC_INVALID;
        frame->pc = pc;
        if (!xr_rbc_call_reserve(stack, base, callee)) return XR_RBC_OUT_OF_MEMORY;
        frame = &stack->frames[stack->depth++];
        *frame = (XrRbcFrame){access.callee, 0, base,
            callee->aggregate_result_type ? access.destination : access.window,
            stack->cell_count, 0, stack->spill_count, 0};
        xr_rbc_entry_clear(stack->slots + base, &stack->entries[access.callee]);
        if (callee->reference_spill_count)
            memset(stack->spills + stack->spill_count, 0, callee->reference_spill_count * sizeof(*stack->spills));
        stack->cell_count += callee->cell_count;
        stack->spill_count += callee->spill_count;
        if (callee->aggregate_size && !xr_rbc_aggregate_reserve(stack, frame, callee)) return XR_RBC_OUT_OF_MEMORY;
        for (uint32_t n = 0; n < access.count; ++n) {
            XrRbcAggregateValue actual = function->aggregate_values[function->aggregate_arguments[access.first + n]];
            XrRbcAggregateValue formal = callee->aggregate_values[callee->aggregate_parameters[n]];
            XrRbcAggregateLayout layout;
            xr_rbc_aggregate_layout(module->aggregate_types[actual.type], &layout);
            memcpy(xr_rbc_aggregate_value(stack, frame, formal),
                xr_rbc_aggregate_value(stack, &stack->frames[parent], actual), layout.size);
        }
        return XR_RBC_OK;
    }
    XrRbcAggregateValue value = function->aggregate_values[access.source];
    XrPodType type = module->aggregate_types[value.type];
    XrRbcAggregateLayout layout;
    xr_rbc_aggregate_layout(type, &layout);
    const uint8_t *bytes = xr_rbc_aggregate_value(stack, frame, value);
    if (parent) {
        const XrRbcFrame *caller = &stack->frames[parent - 1];
        const XrRbcFunction *receiver = &module->functions[caller->function];
        XR_DCHECK(caller->aggregate_base, "call reserved its receiving aggregate area before pushing the callee");
        memcpy(xr_rbc_aggregate_value(stack, caller, receiver->aggregate_values[frame->destination]), bytes, layout.size);
    } else {
        XR_DCHECK(stack->aggregate_result, "the pending outcome owns its fixed return storage");
        xr_rbc_aggregate_pack(type, bytes, stack->aggregate_result);
        stack->slots[0] = (XrRbcRegister){0};
    }
    stack->cell_count = frame->cell_base;
    stack->spill_count = frame->spill_base;
    --stack->depth;
    return XR_RBC_OK;
}
