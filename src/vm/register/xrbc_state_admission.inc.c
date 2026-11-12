/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_state_admission.inc.c - Complete module capability operand admission
 *
 * KEY CONCEPT:
 *   One retained runtime layout supplies payload kinds and mutability. Raw
 *   scalar bits never establish a state or reference capability.
 */
static bool xr_rbc_state_payload_kind(uint32_t kind) {
    return kind == XRT_MODULE_UNIT || kind == XRT_MODULE_BOOL || kind == XRT_MODULE_I64 ||
        kind == XRT_MODULE_F64 || kind == XRT_MODULE_STRING ||
        xr_rbc_state_array(kind);
}

static XrRbcRegisterKind rbc_state_register_kind(uint32_t kind) {
    return kind == XRT_MODULE_STRING ? XR_RBC_STRING : kind == XRT_MODULE_ARRAY_I64 ? XR_RBC_ARRAY_I64 :
        kind == XRT_MODULE_ARRAY_STRING ? XR_RBC_ARRAY_STRING : kind == XRT_MODULE_ARRAY_U8 ? XR_RBC_ARRAY_U8 :
        kind == XRT_MODULE_ARRAY_U64 ? XR_RBC_ARRAY_U64 : XR_RBC_BITS;
}

static bool xr_rbc_state_layout_valid(const XrRbcModule *module) {
    XrtModuleLayoutSpec layout = {0};
    if (module->state_layout) {
        if (xrt_module_layout_view(module->state_layout, &layout) != XR_XIR_VALUE_OK ||
            module->state_entry >= module->function_count) return false;
        const XrRbcFunction *entry = &module->functions[module->state_entry];
        if (!entry->parameter_count || !entry->register_kinds ||
            entry->register_kind_count != entry->register_count || entry->parameter_count > entry->register_count ||
            rbc_register_kind(entry, entry->parameter_count - 1) != XR_RBC_STATE) return false;
        for (uint32_t i = 0; i < layout.slot_count; ++i)
            if (layout.slots[i].type >= layout.type_count ||
                (!xr_rbc_state_payload_kind(layout.types[layout.slots[i].type].kind) &&
                 layout.types[layout.slots[i].type].kind != XRT_MODULE_FIXED_U8)) return false;
    } else if (module->state_entry) return false;
    for (size_t f = 0; f < module->function_count; ++f) {
        const XrRbcFunction *function = &module->functions[f];
        if (function->register_kinds && function->register_kind_count == function->register_count)
            for (uint32_t r = 0; r < function->register_count; ++r)
                if (!module->state_layout &&
                    (rbc_register_kind(function, r) == XR_RBC_STATE ||
                     rbc_register_kind(function, r) == XR_RBC_STATE_REF)) return false;
        if (function->state_access_count && (!module->state_layout || !function->state_accesses)) return false;
        for (size_t i = 0; i < function->state_access_count; ++i) {
            XrRbcStateAccess access = function->state_accesses[i];
            if (access.slot >= layout.slot_count || access.state >= function->register_count ||
                access.value >= function->register_count) return false;
        }
    }
    return true;
}

static bool xr_rbc_state_instruction_valid(const XrRbcModule *module,
    const XrRbcFunction *function, uint32_t word) {
    uint32_t op = word & 255, a = (word >> 8) & 255, b = (word >> 16) & 255, c = word >> 24;
    uint32_t index = word >> 16;
    XrtModuleLayoutSpec layout = {0};
    if (!module->state_layout || !function->register_kinds || a >= function->register_count ||
        xrt_module_layout_view(module->state_layout, &layout) != XR_XIR_VALUE_OK) return false;
    if (op == XR_RBC_MODULE_BEGIN || op == XR_RBC_MODULE_READY)
        return rbc_register_kind(function, a) == XR_RBC_STATE && index < layout.module_count;
    if (op == XR_RBC_STATE_PUBLISH)
        return !b && !c && rbc_register_kind(function, a) == XR_RBC_STATE_REF;
    if (op == XR_RBC_STATE_READ || op == XR_RBC_STATE_WRITE) {
        if (b >= function->register_count || !xr_rbc_state_payload_kind(c)) return false;
        XrRbcRegisterKind payload = rbc_state_register_kind(c);
        if (op == XR_RBC_STATE_READ)
            return rbc_register_kind(function, b) == XR_RBC_STATE_REF &&
                (c == XRT_MODULE_UNIT ? !a : rbc_register_kind(function, a) == payload);
        return rbc_register_kind(function, a) == XR_RBC_STATE_REF &&
            (c == XRT_MODULE_UNIT ? !b : rbc_register_kind(function, b) == payload);
    }
    if (index >= function->state_access_count) return false;
    XrRbcStateAccess access = function->state_accesses[index];
    XrtModuleTypeKind kind = layout.types[layout.slots[access.slot].type].kind;
    if (kind == XRT_MODULE_FIXED_U8 && op != XR_RBC_STATE_REFERENCE) return false;
    XrRbcRegisterKind payload = rbc_state_register_kind(kind);
    if (op == XR_RBC_STATE_INITIALIZE)
        return !(layout.slots[access.slot].flags & XRT_MODULE_SLOT_MUTABLE) &&
            !access.state && rbc_register_kind(function, a) == XR_RBC_STATE &&
            (kind == XRT_MODULE_UNIT ? !access.value : rbc_register_kind(function, access.value) == payload);
    if (rbc_register_kind(function, a) != XR_RBC_STATE_REF ||
        rbc_register_kind(function, access.state) != XR_RBC_STATE) return false;
    if (op == XR_RBC_STATE_REFERENCE) return !access.value;
    return op == XR_RBC_STATE_CONSTRUCT &&
        (layout.slots[access.slot].flags & XRT_MODULE_SLOT_MUTABLE) &&
        (kind == XRT_MODULE_UNIT ? !access.value : rbc_register_kind(function, access.value) == payload);
}

static uint32_t xr_rbc_state_shape(const XrRbcModule *module, uint32_t slot) {
    XrtModuleLayoutSpec layout = {0};
    if (xrt_module_layout_view(module->state_layout, &layout) != XR_XIR_VALUE_OK) return 0;
    XrtModuleSlotSpec row = layout.slots[slot];
    /* Preserve the exact byte count through MOV, joins and typed spills. The
     * low byte and mutable bit retain their existing scalar capability roles. */
    return ((uint32_t)layout.types[row.type].kind + 1u) |
        (layout.types[row.type].fixed_count << 16) |
        ((row.flags & XRT_MODULE_SLOT_MUTABLE) ? 256u : 0u);
}
