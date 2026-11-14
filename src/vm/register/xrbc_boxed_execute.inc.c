/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_boxed_execute.inc.c - Shared canonical operations outside scalar dispatch
 */
static XR_NOINLINE XrRbcStatus xr_rbc_boxed_step(const XrRbcModule *module, XrRbcStack *stack,
    XrRbcFrame *frame, const XrRbcFunction *function, uint32_t index, uint32_t source) {
    XrRbcAggregateAccess access = function->aggregate_accesses[index];
    const XrRbcArrayLocation *args = access.count ? function->aggregate_operands + access.first : NULL;
    XrRbcRegister *result = xr_rbc_aggregate_scalar(stack, frame, access.result);
    if (access.operation == XR_RBC_BOXED_TAKE) {
        XrRbcRegister *input = &stack->slots[frame->base + access.source];
        *result = *input;
        input->bits = 0;
        return XR_RBC_OK;
    }
    if (access.operation == XR_RBC_BOXED_NEW) {
        if (access.count > stack->array_argument_capacity) {
            void *storage = xr_malloc((size_t)access.count * sizeof(uint64_t));
            if (!storage) return XR_RBC_OUT_OF_MEMORY;
            xr_free(stack->array_arguments);
            stack->array_arguments = storage;
            stack->array_argument_capacity = access.count;
        }
        uint64_t *fields = stack->array_arguments;
        for (uint32_t p = 0; p < access.count; ++p) fields[p] = xr_rbc_aggregate_scalar(stack, frame, args[p])->bits;
        result->bits = (uint64_t)(uintptr_t)xrt_boxed_create(module->closure_types, (XrXirType)access.slot, fields, access.count, source);
        return XR_RBC_OK;
    }
    XrRbcRegister *input = xr_rbc_aggregate_scalar(stack, frame, args[0]);
    if (access.operation >= XR_RBC_BOXED_STATE_READ) {
        bool construct = access.operation == XR_RBC_BOXED_STATE_CONSTRUCT;
        bool initialize = access.operation == XR_RBC_BOXED_STATE_INITIALIZE;
        XrtModuleStatus status;
        if (construct || initialize) {
            XrRbcRegister *value = xr_rbc_aggregate_scalar(stack, frame, args[1]);
            if (initialize) status = xrt_module_initialize(input->state, access.destination, value);
            else {
                XrtModuleRef *created = NULL;
                status = xrt_module_construct(input->state, access.destination, value, &created);
                if (!status) {
                    XrtModuleAccess *reference = &stack->module_accesses[(size_t)access.destination * 2];
                    status = xrt_module_reference_cache(created, reference);
                    if (!status) result->state_ref = reference;
                }
            }
        } else {
            XrtModuleAccess *reference = input->state_ref;
            xr_rbc_state_ready(reference, source);
            if (access.operation == XR_RBC_BOXED_STATE_READ) {
                XrtBoxed *value = NULL;
                memcpy(&value, reference->payload, sizeof(value));
                result->bits = (uint64_t)(uintptr_t)xrt_boxed_retain(value, source);
                return XR_RBC_OK;
            }
            status = xrt_module_replace(reference->reference, xr_rbc_aggregate_scalar(stack, frame, args[1]));
        }
        if (status) xrt_value_failure((XrXirValueStatus)status, source);
        return XR_RBC_OK;
    }
    XrtBoxed *value = (XrtBoxed *)(uintptr_t)input->bits;
    if (access.operation == XR_RBC_BOXED_RETAIN) result->bits = (uint64_t)(uintptr_t)xrt_boxed_retain(value, source);
    else if (access.operation == XR_RBC_BOXED_DROP) { xrt_boxed_drop(value, source); input->bits = 0; }
    else if (access.operation == XR_RBC_BOXED_PRESENT) result->bits = xrt_boxed_present(value, source);
    else result->bits = xrt_boxed_field(value, access.destination, source);
    return XR_RBC_OK;
}
