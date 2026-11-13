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
    for (uint32_t p = 0; p < callee->capture_count; ++p)
        if (callee->captures[p].kind != XR_RBC_CALLABLE_BINDING &&
            !xr_rbc_callable_scalar_valid(callee->captures[p], arguments[p].bits)) return false;
    XrRbcCallableType type = module->callable_types[callee->callable - 1];
    for (uint32_t p = 0; p < type.parameter_count; ++p) {
        XrRbcCallableValue parameter = type.parameters[p].value;
        if (!xr_rbc_callable_scalar_valid(parameter, arguments[callee->capture_count + p].bits)) return false;
    }
    return true;
}

static bool xr_rbc_closure_slot(const XrRbcStack *stack, uint64_t token, size_t *output) {
    uint32_t ordinal = (uint32_t)(token >> 1);
    uint32_t generation = (uint32_t)((token >> 33) & UINT32_C(0x3fffffff));
    if (!(token & (UINT64_C(1) << 63)) || (token & 1u) || !ordinal || !generation ||
        (size_t)ordinal > stack->closure_count ||
        stack->closure_owners[ordinal - 1].generation != generation || !stack->closures[ordinal - 1].type)
        return false;
    *output = (size_t)ordinal - 1;
    return true;
}

static bool xr_rbc_callable_target(const XrRbcModule *module, const XrRbcStack *stack,
    uint64_t token, uint32_t *callee, size_t *environment, const XrXirValue **owned) {
    *owned = NULL;
    if (token & (UINT64_C(1) << 63)) {
        size_t index;
        if (!xr_rbc_closure_slot(stack, token, &index)) return false;
        const XrXirValue *value = &stack->closures[index];
        const XrXirFunctionBinding *binding = xr_xir_function_binding(value);
        uint32_t count;
        if (!binding || binding->entry >= module->closure_entry_count ||
            xrt_callable_borrow(stack->closure_code, value, binding->entry, owned, &count) != XR_XIR_VALUE_OK)
            return false;
        *callee = module->closure_targets[binding->entry];
        *environment = SIZE_MAX;
        return count == module->functions[*callee].capture_count;
    }
    if (!(token & 1u)) {
        uint64_t code = token >> 1;
        if (!code || code > module->function_count) return false;
        *callee = (uint32_t)(code - 1);
        *environment = SIZE_MAX;
        return !module->functions[*callee].capture_count && !module->functions[*callee].state_capture;
    }
    uint64_t slot = token >> 1;
    if (!slot || (uint64_t)(size_t)(slot - 1) != slot - 1) return false;
    --slot;
    for (size_t n = stack->depth; n-- > 0;) {
        const XrRbcFrame *frame = &stack->frames[n];
        const XrRbcFunction *function = &module->functions[frame->function];
        if (!frame->aggregate_base || !function->environment_count) continue;
        size_t offset, total;
        if (!xr_rbc_activation_size(function, &offset, &total)) return false;
        size_t first = frame->aggregate_base - 1 + offset / sizeof(*stack->spills);
        for (uint32_t e = 0; e < function->environment_count; ++e) {
            XrRbcEnvironment home = function->environments[e];
            size_t index = first + home.offset / sizeof(*stack->spills);
            if (slot == index) {
                *callee = home.callee;
                *environment = index;
                return true;
            }
        }
    }
    return false;
}

#include "xrbc_callable_scope.inc.c"
#include "xrbc_captured_cell.inc.c"
#include "xrbc_callable_state.inc.c"

static XR_NOINLINE XrRbcStatus xr_rbc_aggregate_control(const XrRbcModule *module, XrRbcStack *stack,
    const XrRbcFunction *function, uint32_t index, uint32_t source, size_t pc) {
    (void)source;
    XrRbcAggregateAccess access = function->aggregate_accesses[index];
    size_t parent = stack->depth - 1;
    XrRbcFrame *frame = &stack->frames[parent];
    if (access.operation == XR_RBC_STATE_GROUP_INITIALIZE) {
        XrRbcStatus status = xr_rbc_aggregate_step(module, stack, frame, function, index, source);
        if (status == XR_RBC_OK) stack->frames[parent].pc = pc;
        return status;
    }
    if (access.operation >= XR_RBC_CAPTURED_CELL_NEW && access.operation <= XR_RBC_CAPTURED_CELL_BIND) {
        XrRbcStatus status = xr_rbc_captured_cell_step(module, stack, function, access, source);
        if (status == XR_RBC_OK) stack->frames[parent].pc = pc;
        return status;
    }
    if (access.operation >= XR_RBC_FUNCTION_STATE_READ && access.operation <= XR_RBC_FUNCTION_STATE_INITIALIZE) {
        XrRbcStatus status = xr_rbc_callable_state_step(module, stack, function, access, source);
        if (status == XR_RBC_OK) stack->frames[parent].pc = pc;
        return status;
    }
    if (access.operation >= XR_RBC_FUNCTION_SCOPE_ENTER && access.operation <= XR_RBC_FUNCTION_BIND) {
        uint64_t token = access.count ? stack->slots[frame->base + access.source].bits : 0;
        XrRbcStatus status = xr_rbc_closure_scope_step(module, stack, function, access,
            access.count ? &token : NULL, source);
        if (status == XR_RBC_OK) {
            if (access.count) stack->slots[frame->base + access.result.index].bits = token;
            frame->pc = pc;
        }
        return status;
    }
    if (access.operation == XR_RBC_FUNCTION_OWNED_REFERENCE) {
        if (!stack->closure_code) return XR_RBC_INVALID;
        if (!xr_rbc_closure_scope_enter(stack) || !xr_rbc_closure_owner_reserve(stack))
            return XR_RBC_OUT_OF_MEMORY;
        void *storage = stack->closure_arguments;
        if (!xr_rbc_grow(&storage, &stack->closure_argument_capacity,
                access.count, sizeof(*stack->closure_arguments))) return XR_RBC_OUT_OF_MEMORY;
        stack->closure_arguments = storage;
        XrtCallableEntry entry = module->closure_entries[access.destination - 1];
        for (uint32_t p = 0; p < access.count; ++p) {
            XrRbcArrayLocation location = function->aggregate_operands[access.first + p];
            uint64_t bits = xr_rbc_aggregate_scalar(stack, frame, location)->bits;
            if (module->functions[access.callee].captures[p].kind == XR_RBC_CALLABLE_CELL) {
                if (!xr_rbc_captured_cell_value(module, stack, rbc_array_location_kind(function, location), bits,
                        entry.captures[p], &stack->closure_arguments[p])) return XR_RBC_INVALID;
                continue;
            }
            if (!xr_rbc_callable_scalar_valid(module->functions[access.callee].captures[p], bits))
                return XR_RBC_INVALID;
            XrXirValue value = {.type=(uint32_t)entry.captures[p]};
            memcpy(&value.payload, &bits, sizeof(bits));
            stack->closure_arguments[p] = value;
        }
        if (!stack->closure_domain) {
            XrXirValueStatus status = xr_xir_domain_new(UINT64_MAX, &stack->closure_domain);
            if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
        }
        XR_DCHECK(!stack->closure_pending.type, "one pending constructor is independently rooted");
        XrXirValueStatus status = xrt_callable_new(stack->closure_code, stack->closure_domain,
            access.destination - 1, access.count ? stack->closure_arguments : NULL, access.count, &stack->closure_pending);
        if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
        stack->slots[frame->base + access.result.index].bits =
            xr_rbc_closure_owner_commit(stack, stack->closure_scope_count - 1, UINT32_MAX);
        frame->pc = pc;
        return XR_RBC_OK;
    }
    if (access.operation == XR_RBC_FUNCTION_RETURN || access.operation == XR_RBC_FUNCTION_SCOPE_RETURN) {
        XrRbcRegister result = access.count ? stack->slots[frame->base + access.source] : (XrRbcRegister){0};
        if (function->callable && !xr_rbc_callable_scalar_valid(module->callable_types[function->callable - 1].result, result.bits))
            return XR_RBC_INVALID;
        XrRbcStatus status = xr_rbc_closure_scope_return(module, stack,
            function->result_kind == XR_RBC_FUNCTION ? &result.bits : NULL,
            function->result_kind == XR_RBC_FUNCTION ? access.slot : 0, source);
        if (status != XR_RBC_OK) return status;
        if (parent) stack->slots[stack->frames[parent - 1].base + frame->destination] = result;
        else stack->slots[0] = result;
        stack->cell_count = frame->cell_base;
        stack->spill_count = frame->spill_base;
        --stack->depth;
        return XR_RBC_OK;
    }
    if (access.operation == XR_RBC_FUNCTION_REFERENCE) {
        uint64_t token = ((uint64_t)access.callee + 1) << 1;
        if (access.destination) {
            if (!xr_rbc_aggregate_reserve(stack, frame, function)) return XR_RBC_OUT_OF_MEMORY;
            size_t offset, total;
            if (!xr_rbc_activation_size(function, &offset, &total)) return XR_RBC_INVALID;
            size_t first = frame->aggregate_base - 1 + offset / sizeof(*stack->spills) +
                function->environments[access.destination - 1].offset / sizeof(*stack->spills);
            const XrRbcFunction *callee = &module->functions[access.callee];
            for (uint32_t p = 0; p < callee->capture_count; ++p) {
                uint64_t bits = xr_rbc_aggregate_scalar(stack, frame, function->aggregate_operands[access.first + p])->bits;
                if (callee->captures[p].kind == XR_RBC_CALLABLE_BINDING ?
                    !xr_rbc_cell(stack, module, bits, XR_RBC_BITS) :
                    !xr_rbc_callable_scalar_valid(callee->captures[p], bits)) return XR_RBC_INVALID;
            }
            for (uint32_t p = 0; p < access.count; ++p)
                stack->spills[first + p] = *xr_rbc_aggregate_scalar(stack, frame,
                    function->aggregate_operands[access.first + p]);
            for (uint32_t p = 0; p < callee->capture_count; ++p) {
                if (callee->captures[p].kind != XR_RBC_CALLABLE_CELL) continue;
                XrRbcArrayLocation location = function->aggregate_operands[access.first + p];
                XrXirValue value;
                if (!xr_rbc_captured_cell_value(module, stack, rbc_array_location_kind(function, location),
                        stack->spills[first + p].bits, (XrXirType)callee->captures[p].type, &value)) return XR_RBC_INVALID;
                memcpy(&stack->spills[first + p].bits, &value.payload, sizeof(value.payload));
            }
            token = ((uint64_t)(first + 1) << 1) | 1u;
        }
        stack->slots[frame->base + access.result.index].bits = token;
        frame->pc = pc;
        return XR_RBC_OK;
    }
    if (access.operation == XR_RBC_AGGREGATE_CALL || access.operation == XR_RBC_FUNCTION_CALL) {
        /* The caller area must precede the callee's spill_base even when the
         * first value in that area will only be produced by this return. */
        if ((function->aggregate_size || function->environment_size) &&
            !xr_rbc_aggregate_reserve(stack, frame, function)) return XR_RBC_OUT_OF_MEMORY;
        size_t environment = SIZE_MAX;
        const XrXirValue *owned = NULL;
        if (access.operation == XR_RBC_FUNCTION_CALL) {
            uint64_t target = stack->slots[frame->base + access.source].bits;
            if (!xr_rbc_callable_target(module, stack, target, &access.callee, &environment, &owned)) return XR_RBC_INVALID;
        }
        const XrRbcFunction *callee = &module->functions[access.callee];
        bool indirect = access.operation == XR_RBC_FUNCTION_CALL;
        if (indirect && frame->base > SIZE_MAX - function->register_count) return XR_RBC_OUT_OF_MEMORY;
        size_t base = frame->base + (indirect ? function->register_count : access.window + 1);
        frame->pc = pc;
        if (!xr_rbc_call_reserve(stack, base, callee)) return XR_RBC_OUT_OF_MEMORY;
        frame = &stack->frames[parent];
        if (indirect) {
            XrRbcCallableType type = module->callable_types[access.slot - 1];
            for (uint32_t p = 0; p < type.parameter_count; ++p)
                stack->slots[base + callee->capture_count + p] =
                    stack->slots[frame->base + access.window + 1 + p];
            for (uint32_t p = 0; p < callee->capture_count; ++p)
                if (owned) memcpy(&stack->slots[base + p].bits, &owned[p].payload, sizeof(uint64_t));
                else stack->slots[base + p] = stack->spills[environment + p];
            if (callee->state_capture)
                stack->slots[base + callee->parameter_count - 1] = stack->spills[environment + callee->capture_count];
        }
        if (!xr_rbc_callable_arguments_valid(module, callee, stack->slots + base)) return XR_RBC_INVALID;
        frame = &stack->frames[stack->depth++];
        *frame = (XrRbcFrame){access.callee, 0, base,
            callee->aggregate_result_type ? access.destination : access.window,
            stack->cell_count, 0, stack->spill_count, 0};
        xr_rbc_entry_clear(stack->slots + base, &stack->entries[access.callee]);
        if (callee->reference_spill_count)
            memset(stack->spills + stack->spill_count, 0, callee->reference_spill_count * sizeof(*stack->spills));
        stack->cell_count += callee->cell_count;
        stack->spill_count += callee->spill_count;
        if ((callee->aggregate_size || callee->environment_size) &&
            !xr_rbc_aggregate_reserve(stack, frame, callee)) return XR_RBC_OUT_OF_MEMORY;
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
    XrRbcStatus status = xr_rbc_closure_scope_return(module, stack, NULL, 0, source);
    if (status != XR_RBC_OK) return status;
    stack->cell_count = frame->cell_base;
    stack->spill_count = frame->spill_base;
    --stack->depth;
    return XR_RBC_OK;
}
