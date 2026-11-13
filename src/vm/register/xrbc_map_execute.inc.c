/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_map_execute.inc.c - Canonical Map operations outside scalar dispatch
 *
 * KEY CONCEPT:
 *   Each operation resolves the current activation or module binding after
 *   bank growth and delegates ownership changes to the canonical runtime.
 */
static bool xr_rbc_map_reference(XrRbcStack *stack, const XrRbcModule *module,
    uint64_t bits, uint32_t source, XrtMapRef *out) {
    if (!bits) return false;
    if (bits & 1u) {
        XrRbcRegister *cell = xr_rbc_cell(stack, module, bits >> 1, XR_RBC_MAP_STRING_I64);
        if (!cell) return false;
        *out = xrt_map_ref_local(&cell->map, source);
    } else {
        XrtModuleAccess *cache = (XrtModuleAccess *)(uintptr_t)bits;
        if (cache->kind != XRT_MODULE_MAP_STRING_I64 || !cache->is_mutable) return false;
        *out = (XrtMapRef){.state=*cache};
    }
    return true;
}

static XR_NOINLINE XrRbcStatus xr_rbc_map_step(const XrRbcModule *module, XrRbcStack *stack,
    const XrRbcFrame *frame, const XrRbcFunction *f, XrRbcArrayAccess a, uint32_t source) {
    if (a.operation == XR_RBC_ARRAY_GET &&
        !xr_rbc_aggregate_reserve(stack, &stack->frames[stack->depth - 1], f)) return XR_RBC_OUT_OF_MEMORY;
    const XrRbcArrayLocation *args = a.count ? f->array_operands + a.first : NULL;
    XrRbcRegister *result = a.operation == XR_RBC_ARRAY_GET ? NULL : xr_rbc_array_location(stack, frame, a.result);
    if (a.operation == XR_RBC_ARRAY_NEW) {
        if (a.count > stack->array_argument_capacity) {
            if (sizeof(XrRbcRegister) > SIZE_MAX / a.count) return XR_RBC_OUT_OF_MEMORY;
            void *storage = xr_malloc((size_t)a.count * sizeof(XrRbcRegister));
            if (!storage) return XR_RBC_OUT_OF_MEMORY;
            xr_free(stack->array_arguments);
            stack->array_arguments = storage;
            stack->array_argument_capacity = a.count;
        }
        size_t count = a.count / 2;
        XrtString **keys = count ? stack->array_arguments : NULL;
        int64_t *values = count ? (int64_t *)(keys + count) : NULL;
        for (size_t p = 0; p < count; ++p) {
            keys[p] = xr_rbc_array_location(stack, frame, args[p * 2])->string;
            values[p] = xr_rbc_signed(xr_rbc_array_location(stack, frame, args[p * 2 + 1])->bits);
        }
        result->map = xrt_map_string_i64_new(keys, values, count, source);
        return XR_RBC_OK;
    }
    XrRbcRegister *first = xr_rbc_array_location(stack, frame, args[0]);
    XrRbcRegister *second = a.count > 1 ? xr_rbc_array_location(stack, frame, args[1]) : NULL;
    if (a.operation == XR_RBC_MAP_STATE_READ || a.operation == XR_RBC_MAP_STATE_WRITE) {
        XrtModuleAccess *reference = first->state_ref;
        xr_rbc_state_ready(reference, source);
        if (a.operation == XR_RBC_MAP_STATE_READ) {
            XrtMap *map;
            memcpy(&map, reference->payload, sizeof(map));
            result->map = xrt_map_retain(map, source);
        } else {
            XrXirValueStatus status = xrt_module_replace(reference->reference, &second->map);
            if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
        }
        return XR_RBC_OK;
    }
    if (a.operation == XR_RBC_ARRAY_BORROW) {
        bool valid = rbc_array_location_kind(f, args[0]) == XR_RBC_STATE_REF ?
            xr_rbc_array_ref_encode_state(first->state_ref, &result->bits) :
            xr_rbc_array_ref_encode_cell(first->bits, &result->bits);
        return valid ? XR_RBC_OK : XR_RBC_INVALID;
    }
    bool reference = a.operation == XR_RBC_ARRAY_REF_READ || a.operation == XR_RBC_ARRAY_REF_WRITE ||
        a.operation == XR_RBC_ARRAY_REF_SET || a.operation == XR_RBC_MAP_REF_REMOVE || a.operation == XR_RBC_MAP_REF_CLEAR;
    if (reference) {
        XrtMapRef ref;
        if (!xr_rbc_map_reference(stack, module, first->bits, source, &ref)) return XR_RBC_INVALID;
        if (a.operation == XR_RBC_ARRAY_REF_READ) result->map = xrt_map_ref_read(ref, source);
        else if (a.operation == XR_RBC_ARRAY_REF_WRITE) xrt_map_ref_write(ref, second->map, source);
        else if (a.operation == XR_RBC_ARRAY_REF_SET) xrt_map_ref_string_i64_set(ref, second->string,
            xr_rbc_signed(xr_rbc_array_location(stack, frame, args[2])->bits), source);
        else if (a.operation == XR_RBC_MAP_REF_REMOVE) result->bits = xrt_map_ref_string_i64_remove(ref, second->string, source);
        else xrt_map_ref_clear(ref, source);
        return XR_RBC_OK;
    }
    if (a.operation == XR_RBC_ARRAY_CELL_NEW) {
        XrRbcFrame *active = &stack->frames[stack->depth - 1];
        size_t cell = active->cell_base + a.cell;
        stack->cells[cell].map = xrt_map_retain(first->map, source);
        if (active->initialized_cells <= a.cell) active->initialized_cells = a.cell + 1;
        result->bits = (uint64_t)cell + 1;
        return XR_RBC_OK;
    }
    bool cell = a.operation == XR_RBC_ARRAY_CELL_READ || a.operation == XR_RBC_ARRAY_CELL_WRITE ||
        a.operation == XR_RBC_ARRAY_CELL_DROP || a.operation == XR_RBC_ARRAY_SET ||
        a.operation == XR_RBC_MAP_REMOVE || a.operation == XR_RBC_MAP_CLEAR;
    XrRbcRegister *binding = cell ? xr_rbc_cell(stack, module, first->bits, XR_RBC_MAP_STRING_I64) : first;
    if (!binding) return XR_RBC_INVALID;
    switch (a.operation) {
    case XR_RBC_ARRAY_LEN: result->bits = (uint64_t)xrt_map_length(first->map, source); break;
    case XR_RBC_MAP_CONTAINS: result->bits = xrt_map_string_i64_contains(first->map, second->string, source); break;
    case XR_RBC_ARRAY_GET: {
        XrRbcAggregateValue home = f->aggregate_values[a.result.index];
        uint8_t *bytes = xr_rbc_aggregate_value(stack, frame, home);
        int64_t number = 0;
        bool found = xrt_map_string_i64_get(first->map, second->string, &number, source);
        memset(bytes, 0, module->aggregate_types[home.type].physical.size);
        bytes[0] = found;
        memcpy(bytes + 8, &number, sizeof(number));
        break;
    }
    case XR_RBC_ARRAY_RETAIN: case XR_RBC_ARRAY_CELL_READ: result->map = xrt_map_retain(binding->map, source); break;
    case XR_RBC_ARRAY_TAKE: result->map = first->map; first->map = NULL; break;
    case XR_RBC_ARRAY_DROP: case XR_RBC_ARRAY_CELL_DROP:
        xrt_map_drop(&binding->map);
        if (cell) first->bits = 0;
        break;
    case XR_RBC_ARRAY_CELL_WRITE: xrt_map_assign(&binding->map, second->map, source); break;
    case XR_RBC_ARRAY_SET: xrt_map_string_i64_set(&binding->map, second->string,
        xr_rbc_signed(xr_rbc_array_location(stack, frame, args[2])->bits), source); break;
    case XR_RBC_MAP_REMOVE: result->bits = xrt_map_string_i64_remove(&binding->map, second->string, source); break;
    case XR_RBC_MAP_CLEAR: xrt_map_clear(&binding->map, source); break;
    default: return XR_RBC_INVALID;
    }
    return XR_RBC_OK;
}
