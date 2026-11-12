/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_aggregate_call.inc.c - By-value transfers between indexed activations
 */
static XR_NOINLINE XrRbcStatus xr_rbc_aggregate_control(const XrRbcModule *module, XrRbcStack *stack,
    const XrRbcFunction *function, uint32_t index, uint32_t source, size_t pc) {
    (void)source;
    XrRbcAggregateAccess access = function->aggregate_accesses[index];
    size_t parent = stack->depth - 1;
    XrRbcFrame *frame = &stack->frames[parent];
    if (access.operation == XR_RBC_AGGREGATE_CALL) {
        /* The caller area must precede the callee's spill_base even when the
         * first value in that area will only be produced by this return. */
        if (!xr_rbc_aggregate_reserve(stack, frame, function)) return XR_RBC_OUT_OF_MEMORY;
        const XrRbcFunction *callee = &module->functions[access.callee];
        size_t base = frame->base + access.window + 1;
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
    XrRbcAggregateType type = module->aggregate_types[value.type];
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
        if (layout.scalar_slots)
            for (uint32_t n = 0; n < type.count; ++n) {
                uint64_t bits = 0;
                memcpy(&bits, bytes + (size_t)n * sizeof(bits), sizeof(bits));
                stack->aggregate_result[n] = (uint8_t)bits;
            }
        else memcpy(stack->aggregate_result, bytes, type.count);
    }
    stack->cell_count = frame->cell_base;
    stack->spill_count = frame->spill_base;
    --stack->depth;
    return XR_RBC_OK;
}
