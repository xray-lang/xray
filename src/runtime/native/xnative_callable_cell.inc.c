/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static XrXirValue callable_cell_value(XrtCallableGroup *group, const XrtCapturedCell *cell, uint32_t source) {
    if (!cell) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    const XirObject *object = (const XirObject *)cell;
    XrXirValue value = {.type=(uint32_t)object->type, .payload=(int64_t)(intptr_t)cell};
    XrXirType element = xr_xir_cell_element(xr_xir_compile_type_arena_types(group->types), object->type);
    const XrXirTypeNode *node = xr_xir_type_node(xr_xir_compile_type_arena_types(group->types), element);
    bool map = node && node->kind == XR_XIR_TYPE_MAP &&
        node->element == XR_XIR_STRING && node->result == XR_XIR_I64;
    if (!xr_xir_value_argument(&value, group->types, object->type) || (element != XR_XIR_I64 && !map))
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    return value;
}
static XrtCapturedCell *callable_cell_scope_new(XrtCallableScope *scope, uint32_t type,
    const XrXirValue *initial, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    if (!scope || scope != root->callable_scope || scope->root != root || type >= scope->image->type_count)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrtCallableScopeBody *body = callable_scope_body(scope, source);
    XrXirType physical = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + XRT_CALLABLE_PREFIX_COUNT + type);
    if (body->count == UINT32_MAX) xrt_value_failure(XR_XIR_VALUE_LIMIT, source);
    callable_scope_grow(body, false, body->count + 1, source);
    XrtCallableScopeValue *received = &body->values[body->count];
    *received = (XrtCallableScopeValue){.binding=UINT32_MAX};
    callable_scope_require(xrt_callable_cell_new(body->group->code, body->group->domain, physical,
        initial, &received->value), source);
    ++body->count;
    return (XrtCapturedCell *)(intptr_t)received->value.payload;
}
XRAY_API XrtCapturedCell *xrt_callable_cell_scope_new(XrtCallableScope *scope, uint32_t type, int64_t initial, uint32_t source) {
    XrXirValue value = {XR_XIR_I64, 0, initial};
    return callable_cell_scope_new(scope, type, &value, source);
}
XRAY_API XrtCapturedCell *xrt_callable_cell_scope_map_new(XrtCallableScope *scope, uint32_t type,
    const struct XrtMap *initial, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    if (!scope || scope != root->callable_scope || scope->root != root || type >= scope->image->type_count)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrtCallableScopeBody *body = callable_scope_body(scope, source);
    XrXirType expected;
    XrXirValue value = {0};
    if (!xrt_callable_type_id((XrtCallableValueSpec){XRT_CALLABLE_MAP_STRING_I64, 0}, 0, &expected) ||
        !xr_xir_value_from_payload(body->group->types, expected, (int64_t)(intptr_t)initial, &value))
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    return callable_cell_scope_new(scope, type, &value, source);
}
XRAY_API XrtCapturedCell *xrt_callable_cell_scope_bind(XrtCallableScope *scope, uint32_t binding,
    XrtCapturedCell *cell, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    XrtCallableScope *current = root->callable_scope;
    while (current && current != scope) current = current->parent;
    if (!current || current->root != root || binding == UINT32_MAX || !cell)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrtCallableScopeBody *body = callable_scope_body(scope, source);
    XrXirValue value = callable_cell_value(body->group, cell, source);
    uint32_t slot = 0;
    while (slot < body->count && body->values[slot].binding != binding) ++slot;
    bool existing = slot < body->count;
    if (!existing) {
        if (body->count == UINT32_MAX) xrt_value_failure(XR_XIR_VALUE_LIMIT, source);
        callable_scope_grow(body, false, body->count + 1, source);
    }
    XrXirValue received = {0};
    callable_scope_require(xr_xir_value_copy(&value, &received), source);
    XrXirValue previous = existing ? body->values[slot].value : (XrXirValue){0};
    if (!existing) {
        slot = 0;
        while (slot < body->count && body->values[slot].binding < binding) ++slot;
        memmove(body->values + slot + 1, body->values + slot, (size_t)(body->count - slot) * sizeof(*body->values));
        ++body->count;
    }
    body->values[slot] = (XrtCallableScopeValue){received, binding};
    xr_xir_value_drop(&previous);
    return cell;
}
XRAY_API int64_t xrt_callable_cell_i64_read(const XrtCallableImage *image, const XrtCapturedCell *cell, uint32_t source) {
    XrtCallableGroup *group = callable_group(callable_registry(source), image, source);
    XrXirValue value = callable_cell_value(group, cell, source), result = {0};
    if (xr_xir_cell_element(xr_xir_compile_type_arena_types(group->types), (XrXirType)value.type) != XR_XIR_I64)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    callable_scope_require(xr_xir_cell_read(&value, &result), source);
    return result.payload;
}
XRAY_API void xrt_callable_cell_i64_write(const XrtCallableImage *image, const XrtCapturedCell *cell,
    int64_t bits, uint32_t source) {
    XrtCallableGroup *group = callable_group(callable_registry(source), image, source);
    XrXirValue value = callable_cell_value(group, cell, source), replacement = {XR_XIR_I64, 0, bits};
    callable_scope_require(xrt_callable_cell_write(group->code, group->domain,
        (XrXirType)value.type, &value, &replacement), source);
}
