/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_aggregate_execute.inc.c - Indexed activation storage for physical values
 *
 * KEY CONCEPT:
 *   The existing spill allocation also holds the activation's aggregate area.
 *   A frame stores an index plus one, never an address into its movable bank.
 *   Repeated construction reuses that area and each RET retires it with spills.
 */
static bool xr_rbc_aggregate_reserve(XrRbcStack *stack, XrRbcFrame *frame, const XrRbcFunction *f) {
    if (frame->aggregate_base) return true;
    size_t slots = f->aggregate_size / sizeof(*stack->spills) +
        (f->aggregate_size % sizeof(*stack->spills) != 0);
    if (!slots || slots > SIZE_MAX - stack->spill_count || stack->spill_count == SIZE_MAX) return false;
    void *storage = stack->spills;
    if (!xr_rbc_grow(&storage, &stack->spill_capacity, stack->spill_count + slots, sizeof(*stack->spills)))
        return false;
    stack->spills = storage;
    frame->aggregate_base = stack->spill_count + 1;
    stack->spill_count += slots;
    return true;
}

static XrRbcRegister *xr_rbc_aggregate_scalar(XrRbcStack *stack, const XrRbcFrame *frame,
    XrRbcArrayLocation location) {
    return location.spill ? &stack->spills[frame->spill_base + location.index] :
        &stack->slots[frame->base + location.index];
}

static uint8_t *xr_rbc_aggregate_value(XrRbcStack *stack, const XrRbcFrame *frame,
    XrRbcAggregateValue value) {
    return (uint8_t *)(stack->spills + frame->aggregate_base - 1) + value.offset;
}

#include "xrbc_nullable_execute.inc.c"

static XrRbcStatus xr_rbc_aggregate_state_step(const XrRbcModule *module, XrRbcStack *stack,
    XrRbcFrame *frame, const XrRbcFunction *f, uint32_t index, uint32_t source) {
    XrRbcAggregateAccess access = f->aggregate_accesses[index];
    bool read = access.operation == XR_RBC_AGGREGATE_STATE_READ;
    bool construct = access.operation == XR_RBC_AGGREGATE_STATE_CONSTRUCT;
    bool initialize = access.operation == XR_RBC_AGGREGATE_STATE_INITIALIZE;
    XrRbcArrayLocation operand = f->aggregate_operands[access.first];
    XrtModuleAccess *reference = NULL;
    if (!construct && !initialize) {
        reference = xr_rbc_aggregate_scalar(stack, frame, operand)->state_ref;
        xr_rbc_state_ready(reference, source);
    }
    if (!xr_rbc_aggregate_reserve(stack, frame, f)) return XR_RBC_OUT_OF_MEMORY;
    XrRbcAggregateValue value = f->aggregate_values[read ? access.destination : access.source];
    XrRbcAggregateType type = module->aggregate_types[value.type];
    uint8_t *bytes = xr_rbc_aggregate_value(stack, frame, value);
    uint8_t lanes[XR_RBC_AGGREGATE_INLINE_SCALARS];
    bool packed = type.kind == XR_RBC_FIXED_U8 && type.count <= XR_RBC_AGGREGATE_INLINE_SCALARS;
    void *payload = packed ? lanes : bytes;
    if (read) {
        if (packed) xr_rbc_aggregate_unpack(type, reference->payload, bytes);
        else memcpy(bytes, reference->payload, type.physical.size);
        return XR_RBC_OK;
    }
    if (packed) xr_rbc_aggregate_pack(type, bytes, lanes);
    XrtModuleStatus status;
    if (construct || initialize) {
        XrtModuleState *state = xr_rbc_aggregate_scalar(stack, frame, operand)->state;
        if (initialize) status = xrt_module_initialize(state, access.slot, payload);
        else {
            XrtModuleRef *created = NULL;
            status = xrt_module_construct(state, access.slot, payload, &created);
            if (!status) {
                reference = &stack->module_accesses[(size_t)access.slot * 2];
                status = xrt_module_reference_cache(created, reference);
                if (!status) xr_rbc_aggregate_scalar(stack, frame, access.result)->state_ref = reference;
            }
        }
    } else status = xrt_module_replace(reference->reference, payload);
    if (status) xrt_value_failure((XrXirValueStatus)status, source);
    return XR_RBC_OK;
}

static XR_NOINLINE XrRbcStatus xr_rbc_aggregate_step(const XrRbcModule *module, XrRbcStack *stack,
    XrRbcFrame *frame, const XrRbcFunction *f, uint32_t index, uint32_t source) {
    if (f->aggregate_accesses[index].operation >= XR_RBC_AGGREGATE_STATE_READ)
        return xr_rbc_aggregate_state_step(module, stack, frame, f, index, source);
    if (!xr_rbc_aggregate_reserve(stack, frame, f)) return XR_RBC_OUT_OF_MEMORY;
    XrRbcAggregateAccess access = f->aggregate_accesses[index];
    if (access.operation >= XR_RBC_NULLABLE_NONE && access.operation <= XR_RBC_NULLABLE_UNWRAP)
        return xr_rbc_nullable_step(module, stack, frame, f, access, source);
    XrRbcAggregateValue value = f->aggregate_values[access.operation == XR_RBC_AGGREGATE_GET ?
        access.source : access.destination];
    XrRbcAggregateType type = module->aggregate_types[value.type];
    XrRbcAggregateLayout layout;
    XR_CHECK(xr_rbc_aggregate_layout(type, &layout), "prepared aggregate has an exact physical layout");
    uint8_t *bytes = xr_rbc_aggregate_value(stack, frame, value);
    if (access.operation == XR_RBC_AGGREGATE_NEW) {
        for (uint32_t n = 0; n < access.count; ++n) {
            XrRbcArrayLocation operand = f->aggregate_operands[access.first + n];
            if (xr_rbc_aggregate_scalar(stack, frame, operand)->bits > UINT8_MAX)
                xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
        }
        for (uint32_t n = 0; n < access.count; ++n) {
            uint64_t bits = xr_rbc_aggregate_scalar(stack, frame, f->aggregate_operands[access.first + n])->bits;
            if (layout.scalar_slots) memcpy(bytes + (size_t)n * sizeof(bits), &bits, sizeof(bits));
            else bytes[n] = (uint8_t)bits;
        }
    } else if (access.operation == XR_RBC_AGGREGATE_GET) {
        uint64_t offset = xr_rbc_aggregate_scalar(stack, frame, f->aggregate_operands[access.first])->bits;
        if (offset >= type.count) xrt_fixed_array_bounds(source);
        uint64_t bits = 0;
        if (layout.scalar_slots) memcpy(&bits, bytes + (size_t)offset * sizeof(bits), sizeof(bits));
        else bits = bytes[(size_t)offset];
        xr_rbc_aggregate_scalar(stack, frame, access.result)->bits = bits;
    } else {
        uint8_t *original = xr_rbc_aggregate_value(stack, frame, f->aggregate_values[access.source]);
        memmove(bytes, original, layout.size);
    }
    return XR_RBC_OK;
}
