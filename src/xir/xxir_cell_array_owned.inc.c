/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_cell_array_owned.inc.c - Canonical loan checks on prepaid integer Cells
 *
 * KEY CONCEPT:
 *   The typed activation producer authenticates immutable shallow shape and
 *   physical lifetime. Every access still checks the current canonical loan.
 */
static XirCell *cell_array_i64_owned(const XrXirValue *value) {
    if (!value || !value->payload) return NULL;
    XirCell *cell = (XirCell *)object_pointer(value);
#if XR_XIR_PARANOID
    if (!xr_xir_value_valid(value) || cell->object.kind != XIR_OBJECT_CELL ||
        cell->projection_count || !cell->initialized) return NULL;
    const XrXirTypes *types = xr_xir_compile_type_arena_types(cell->object.arena);
    XrXirType element = xr_xir_cell_element(types, cell->object.type);
    const XrXirTypeNode *array = xr_xir_type_node(types, element);
    if (!array || array->kind != XR_XIR_TYPE_ARRAY || array->element != XR_XIR_I64 ||
        !xr_xir_value_argument(&cell->value, cell->object.arena, element)) return NULL;
#endif
    XR_DCHECK(cell->object.kind == XIR_OBJECT_CELL && !cell->projection_count,
        "the typed producer keeps one admitted whole Cell identity alive");
    return cell;
}
XR_FUNC XrXirValueStatus xir_cell_array_i64_read_owned(const XrXirValue *value,
    int64_t index, const XrXirCellAuthority *authority, int64_t *output) {
    XirCell *cell = cell_array_i64_owned(value);
    if (!output || !cell_access(cell, authority)) return XR_XIR_VALUE_BAD_ARGUMENT;
    return xr_xir_array_i64_read_owned(&cell->value, index, output);
}
XR_FUNC XrXirValueStatus xir_cell_array_i64_write_owned(const XrXirValue *value,
    int64_t index, int64_t number, const XrXirCellAuthority *authority,
    XrXirValueAdmission *admission, XrXirFaultDetail *fault) {
    XirCell *cell = cell_array_i64_owned(value);
    if (!admission || !admission->arena || !admission->domain ||
        !admission_domain_current(admission) || !cell_write_access(cell, authority) ||
        cell->object.domain != admission->domain) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirType element = xr_xir_cell_element(xr_xir_compile_type_arena_types(cell->object.arena),
        cell->object.type);
    XrXirValuePlace place = {element, &cell->value.payload};
    XrXirValue replacement = {XR_XIR_I64, 0, number};
    return xr_xir_array_set(&place, index, &replacement, admission, fault);
}
