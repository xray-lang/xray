/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_nullable_execute.inc.c - Inline typed values in indexed aggregate homes
 */
static void xr_rbc_aggregate_pack(XrPodType type, const uint8_t *home, uint8_t *bytes) {
    uint32_t width = xr_rbc_pod_scalar_size(type.element);
    if (type.kind != XR_POD_FIXED_INT || type.count > XR_RBC_AGGREGATE_INLINE_SCALARS || !width) {
        memcpy(bytes, home, type.physical.size);
        return;
    }
    for (uint32_t n = 0; n < type.count; ++n) {
        uint64_t bits;
        memcpy(&bits, home + (size_t)n * sizeof(bits), sizeof(bits));
        xr_rbc_fixed_store(bytes + (size_t)n * width, width, bits);
    }
}

static void xr_rbc_aggregate_unpack(XrPodType type, const uint8_t *bytes, uint8_t *home) {
    uint32_t width = xr_rbc_pod_scalar_size(type.element);
    if (type.kind != XR_POD_FIXED_INT || type.count > XR_RBC_AGGREGATE_INLINE_SCALARS || !width) {
        memcpy(home, bytes, type.physical.size);
        return;
    }
    for (uint32_t n = 0; n < type.count; ++n) {
        uint64_t bits = xr_rbc_fixed_load(bytes + (size_t)n * width, width);
        memcpy(home + (size_t)n * sizeof(bits), &bits, sizeof(bits));
    }
}

static XrRbcStatus xr_rbc_nullable_step(const XrRbcModule *module, XrRbcStack *stack,
    XrRbcFrame *frame, const XrRbcFunction *f, XrRbcAggregateAccess access, uint32_t source) {
    bool create = access.operation == XR_RBC_NULLABLE_NONE || access.operation == XR_RBC_NULLABLE_SOME;
    XrRbcAggregateValue value = f->aggregate_values[create ? access.destination : access.source];
    XrPodType type = module->aggregate_types[value.type];
    uint8_t *bytes = xr_rbc_aggregate_value(stack, frame, value);
    XrPodLayout payload;
    bool aggregate = type.element >= XR_POD_SCALAR_COUNT;
    XrPodType child = {0};
    if (aggregate) { child = module->aggregate_types[type.element - XR_POD_SCALAR_COUNT]; payload = child.physical; }
    else if (!xr_pod_scalar_layout((XrPodScalar)type.element, &payload)) return XR_RBC_INVALID;
    if (create) {
        memset(bytes, 0, type.physical.size);
        if (access.operation == XR_RBC_NULLABLE_NONE) return XR_RBC_OK;
        if (aggregate) xr_rbc_aggregate_pack(child,
            xr_rbc_aggregate_value(stack, frame, f->aggregate_values[access.source]), bytes + payload.alignment);
        else if (payload.size) {
            uint64_t bits = xr_rbc_aggregate_scalar(stack, frame, access.result)->bits;
            XrtValueKind kind;
            if (!xr_pod_value_kind((XrPodScalar)type.element, &kind) ||
                !rbc_state_scalar_store(kind, bits, bytes + payload.alignment)) return XR_RBC_INVALID;
        }
        bytes[0] = 1;
        return XR_RBC_OK;
    }
    if (bytes[0] > 1) return XR_RBC_INVALID;
    if (access.operation == XR_RBC_NULLABLE_IS_SOME) {
        xr_rbc_aggregate_scalar(stack, frame, access.result)->bits = bytes[0];
        return XR_RBC_OK;
    }
    if (!bytes[0]) xrt_null_unwrap(source);
    if (aggregate) xr_rbc_aggregate_unpack(child, bytes + payload.alignment,
        xr_rbc_aggregate_value(stack, frame, f->aggregate_values[access.destination]));
    else if (payload.size) {
        XrtValueKind kind;
        if (!xr_pod_value_kind((XrPodScalar)type.element, &kind) ||
            !rbc_state_scalar_load(kind, bytes + payload.alignment,
                &xr_rbc_aggregate_scalar(stack, frame, access.result)->bits)) return XR_RBC_INVALID;
    }
    return XR_RBC_OK;
}
