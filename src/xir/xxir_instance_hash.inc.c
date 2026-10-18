/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_hash.inc.c - Private hash edits and authenticated publication
 */
#include "xxir_hash_iteration.h"
XR_FUNC XrXirCallStatus xr_xir_instance_hash_new(XrXirCallView *view, XrXirType type,
    const XrXirValue *values, uint32_t count, XrXirValue *output) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!view_instance(view)) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    if (!admission || !output || !value_unit(*output) || (count && !values)) return XR_XIR_CALL_BAD_STATE;
    const XrXirTypes *types = xr_xir_compile_type_arena_types(admission->arena);
    if (xr_xir_type_is_set(types, type))
        return value_call_status(xr_xir_set_new(type, values, count, admission, output));
    if (!xr_xir_type_is_map(types, type) || (count & 1u)) return XR_XIR_CALL_BAD_STATE;
    XrXirValue result = {0};
    XrXirValueStatus status = xr_xir_map_new(type, NULL, NULL, 0, admission, &result);
    XrXirValuePlace place = {type, &result.payload};
    for (uint32_t i = 0; status == XR_XIR_VALUE_OK && i < count; i += 2)
        status = xr_xir_map_set(&place, &values[i], &values[i + 1], admission);
    if (status == XR_XIR_VALUE_OK) { *output = result; result = (XrXirValue){0}; }
    xr_xir_value_drop(&result); return value_call_status(status);
}
static bool instance_hash_write(XrXirOp operation) {
    return operation == XR_XIR_MAP_SET || operation == XR_XIR_SET_ADD ||
        operation == XR_XIR_HASH_DELETE || operation == XR_XIR_HASH_CLEAR;
}
static bool instance_hash_shape(const XrXirHashRequest *request) {
    if (!request || !request->receiver || !request->path ||
        request->operation < XR_XIR_MAP_GET || request->operation > XR_XIR_MAP_VALUE_AT) return false;
    bool key = request->operation >= XR_XIR_SET_UNION ||
        (request->operation < XR_XIR_HASH_KEYS &&
        request->operation != XR_XIR_HASH_LEN && request->operation != XR_XIR_HASH_CLEAR);
    if ((request->key != NULL) != key || (request->value != NULL) != (request->operation == XR_XIR_MAP_SET)) return false;
    if (request->operation >= XR_XIR_SET_IS_SUBSET && request->operation <= XR_XIR_MAP_CONTAINS_VALUE)
        return request->result == XR_XIR_BOOL;
    if (request->operation == XR_XIR_MAP_GET || request->operation >= XR_XIR_MAP_INDEX) return true;
    XrXirType result = request->operation == XR_XIR_HASH_LEN ? XR_XIR_I64 :
        request->operation == XR_XIR_HASH_CONTAINS || request->operation == XR_XIR_HASH_DELETE ? XR_XIR_BOOL : XR_XIR_UNIT;
    return request->result == result;
}
static XrXirValueStatus instance_set_read(const XrXirHashRequest *request,
    const XrXirValue *container, XrXirValueAdmission *admission, XrXirValue *output) {
    bool relation = false; XrXirValueStatus status;
    if (request->operation >= XR_XIR_SET_IS_SUBSET) {
        status = request->operation == XR_XIR_SET_IS_SUBSET ?
            xr_xir_set_is_subset(container, request->key, admission, &relation) :
            xr_xir_set_is_superset(container, request->key, admission, &relation);
        if (status == XR_XIR_VALUE_OK) *output = (XrXirValue){XR_XIR_BOOL, 0, relation};
        return status;
    }
    if (request->result != (XrXirType)container->type) return XR_XIR_VALUE_BAD_ARGUMENT;
    switch (request->operation) {
    case XR_XIR_SET_UNION: return xr_xir_set_union(container, request->key, admission, output);
    case XR_XIR_SET_INTERSECTION: return xr_xir_set_intersection(container, request->key, admission, output);
    case XR_XIR_SET_DIFFERENCE: return xr_xir_set_difference(container, request->key, admission, output);
    case XR_XIR_SET_SYMMETRIC_DIFFERENCE: return xr_xir_set_symmetric_difference(container, request->key, admission, output);
    default: return XR_XIR_VALUE_BAD_ARGUMENT;
    }
}
static XrXirValueStatus instance_hash_position(const XrXirHashRequest *request,
    const XrXirValue *container, XrXirValueAdmission *admission, XrXirValue *output, XrXirFaultDetail *fault) {
    if (!request->key || request->key->type != XR_XIR_I64 || request->key->reserved) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypes *types = xr_xir_compile_type_arena_types(admission->arena);
    bool values = request->operation == XR_XIR_MAP_VALUE_AT;
    if (request->result != (values ? xr_xir_map_element(types, (XrXirType)container->type) :
        xr_xir_type_is_map(types, (XrXirType)container->type) ? xr_xir_map_key(types, (XrXirType)container->type) :
        xr_xir_set_element(types, (XrXirType)container->type))) return XR_XIR_VALUE_BAD_ARGUMENT;
    return values ? xr_xir_map_value_at(container, request->key->payload, admission, output, fault) :
        xr_xir_hash_key_at(container, request->key->payload, admission, output, fault);
}
static XrXirValueStatus instance_hash_read_value(const XrXirHashRequest *request,
    const XrXirValue *container, XrXirValueAdmission *admission, XrXirValue *output, bool *missing) {
    XrXirValueStatus status; bool found = false;
    if (request->operation == XR_XIR_MAP_CONTAINS_VALUE) {
        status = xr_xir_map_contains_value(container, request->key, admission, &found);
        if (status == XR_XIR_VALUE_OK) *output = (XrXirValue){XR_XIR_BOOL, 0, found};
    } else if (request->operation >= XR_XIR_SET_UNION) {
        status = instance_set_read(request, container, admission, output);
    } else if (request->operation == XR_XIR_HASH_KEYS) {
        status = xr_xir_map_keys(container, request->result, admission, output);
    } else if (request->operation == XR_XIR_HASH_VALUES) {
        status = xr_xir_hash_values(container, request->result, admission, output);
    } else if (request->operation == XR_XIR_MAP_ENTRIES) {
        status = xr_xir_map_entries(container, request->result, admission, output);
    } else if (request->operation == XR_XIR_HASH_LEN) {
        int64_t length = 0; status = xr_xir_hash_len(container, admission, &length);
        if (status == XR_XIR_VALUE_OK) *output = (XrXirValue){XR_XIR_I64, 0, length};
    } else if (request->operation == XR_XIR_HASH_CONTAINS) {
        const XrXirTypes *types = xr_xir_compile_type_arena_types(admission->arena);
        status = xr_xir_type_is_map(types, (XrXirType)container->type) ?
            xr_xir_map_contains_key(container, request->key, admission, &found) :
            xr_xir_set_contains(container, request->key, admission, &found);
        if (status == XR_XIR_VALUE_OK) *output = (XrXirValue){XR_XIR_BOOL, 0, found};
    } else {
        const XrXirTypes *types = xr_xir_compile_type_arena_types(admission->arena);
        bool required = request->operation == XR_XIR_MAP_INDEX;
        XrXirType element_type = xr_xir_map_element(types, (XrXirType)container->type);
        if (!xr_xir_type_is_map(types, (XrXirType)container->type) || (required ? request->result != element_type :
            !xr_xir_type_is_nullable(types, request->result) || xr_xir_nullable_element(types, request->result) != element_type))
            return XR_XIR_VALUE_BAD_ARGUMENT;
        XrXirValue element = {0};
        status = xr_xir_map_get(container, request->key, admission, &element, &found);
        if (status == XR_XIR_VALUE_OK && required) {
            if (found) { *output = element; element = (XrXirValue){0}; }
            else *missing = true;
        } else if (status == XR_XIR_VALUE_OK)
            status = xr_xir_nullable_new(request->result, found ? &element : NULL, admission, output);
        xr_xir_value_drop(&element);
    }
    return status;
}
static XrXirValueStatus instance_hash_edit(const XrXirHashRequest *request,
    XrXirValue *candidate, XrXirValueAdmission *admission, bool *removed) {
    XrXirValuePlace place = {(XrXirType)candidate->type, &candidate->payload};
    switch (request->operation) {
    case XR_XIR_MAP_SET: return xr_xir_map_set(&place, request->key, request->value, admission);
    case XR_XIR_SET_ADD: return xr_xir_set_add(&place, request->key, admission);
    case XR_XIR_HASH_DELETE: return xr_xir_hash_delete(&place, request->key, admission, removed);
    case XR_XIR_HASH_CLEAR: return xr_xir_hash_clear(&place, admission);
    default: return XR_XIR_VALUE_BAD_ARGUMENT;
    }
}
XR_FUNC XrXirCallStatus xr_xir_instance_hash_apply(XrXirCallView *view,
    const XrXirHashRequest *request, XrXirValue *output, XrXirFaultDetail *fault) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!view_instance(view)) return XR_XIR_CALL_BAD_STATE;
    if (!fault || !output || !value_unit(*output) || !instance_hash_shape(request)) return XR_XIR_CALL_BAD_STATE;
    *fault = (XrXirFaultDetail){0};
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    bool write = instance_hash_write(request->operation);
    const XrXirValueReceiver *receiver = request->receiver;
    XrXirValue container = {0}, borrowed = {0};
    const XrXirValue *current = &container;
    XrXirCallStatus status;
    if (receiver->kind == XR_XIR_ROOT_VALUE) {
        if (write || request->path->count || receiver->local_payload || receiver->slot ||
            receiver->value.type != (uint32_t)receiver->type) return XR_XIR_CALL_BAD_STATE;
        borrowed = receiver->value; current = &borrowed; status = XR_XIR_CALL_READY;
    } else status = xr_xir_instance_path_read_at(view, request->instruction, receiver,
        request->path, &container, fault);
    if (status == XR_XIR_CALL_READY && !write && request->operation >= XR_XIR_HASH_KEY_AT)
        status = value_call_status(instance_hash_position(request, current, admission, output, fault));
    else if (status == XR_XIR_CALL_READY && !write) {
        bool missing = false;
        status = value_call_status(instance_hash_read_value(request, current, admission, output, &missing));
        if (status == XR_XIR_CALL_READY && missing) {
            *fault = (XrXirFaultDetail){XR_XIR_PANIC_KEY_NOT_FOUND, 0, 0, 0};
            status = XR_XIR_CALL_RUNTIME_PANIC;
        }
    }
    if (status == XR_XIR_CALL_READY && write) {
        int64_t before = container.payload; bool removed = false;
        status = value_call_status(instance_hash_edit(request, &container, admission, &removed));
        /* The read owns a logical copy, so edits cannot change the source root.
         * Final path publication rechecks the real Cell/Class authority. */
        if (status == XR_XIR_CALL_READY && container.payload != before)
            status = xr_xir_instance_path_write_at(view, request->instruction, receiver,
                request->path, &container, fault);
        if (status == XR_XIR_CALL_READY && request->operation == XR_XIR_HASH_DELETE)
            *output = (XrXirValue){XR_XIR_BOOL, 0, removed};
    }
    xr_xir_value_drop(&container); return status;
}
