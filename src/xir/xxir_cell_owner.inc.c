/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_cell_owner.inc.c - Cell publication and exact parent loan restoration
 *
 * KEY CONCEPT:
 *   Publication fixes storage identity; every loan is owned by a live frame.
 */
static XirCell *cell_owner(const XrXirValue *value) {
    if (!xr_xir_value_valid(value) || !owned_carrier_type((XrXirType)value->type)) return NULL;
    XirObject *object = object_pointer(value);
    return object->kind == XIR_OBJECT_CELL ? (XirCell *)object : NULL;
}
static XirObject *cell_root_object(const XirCell *cell) {
    return cell ? (cell->projection_count ? object_pointer(&cell->value) :
        (XirObject *)&cell->object) : NULL;
}
static XrXirCellLoan **cell_root_loans(XirObject *object) {
    if (!object) return NULL;
    if (object->kind == XIR_OBJECT_CELL) return &((XirCell *)object)->borrow_top;
    return object->kind == XIR_OBJECT_CLASS ? &((XirClassObject *)object)->borrow_top : NULL;
}
static XirCell *cell_root(const XirCell *cell) {
    XirObject *object = cell_root_object(cell);
    return object && object->kind == XIR_OBJECT_CELL ? (XirCell *)object : NULL;
}
static bool cell_scope_contains(const XirCell *scope, const XirCell *target) {
    if (!scope || !target || cell_root_object(scope) != cell_root_object(target)) return false;
    if (!scope->projection_count) return true;
    if (scope->projection_count > target->projection_count) return false;
    const XrXirValuePathStep *a = (const XrXirValuePathStep *)(scope + 1);
    const XrXirValuePathStep *b = (const XrXirValuePathStep *)(target + 1);
    for (uint32_t i = 0; i < scope->projection_count; ++i)
        if (a[i].kind != b[i].kind || a[i].container != b[i].container ||
            a[i].selector != b[i].selector) return false;
    return true;
}
static bool cell_authority_valid(const XrXirCellAuthority *authority) {
    return authority && authority->activation && authority->frame && authority->epoch;
}
static bool cell_authority_equal(const XrXirCellAuthority *left, const XrXirCellAuthority *right) {
    return cell_authority_valid(left) && cell_authority_valid(right) &&
        left->activation == right->activation && left->frame == right->frame && left->epoch == right->epoch;
}
static bool cell_access(const XirCell *cell, const XrXirCellAuthority *authority) {
    if (!cell || !cell->initialized) return false;
    XirObject *owner = cell_root_object(cell);
    XrXirCellLoan **top = cell_root_loans(owner);
    if (!top) return false;
    if (*top) {
        const XirCell *scope = (*top)->scope;
        return cell_authority_equal(&(*top)->authority, authority) &&
            (!scope->projection_count || scope == cell);
    }
    if (cell->projection_count) return cell_authority_equal(&cell->projection_authority, authority);
    return !((const XirCell *)owner)->module_storage || cell_authority_valid(authority);
}
XR_FUNC XrXirValueStatus xr_xir_cell_publication_prepare(const XrXirValue *value,
    const XrXirCellPublication *publication) {
    XirCell *cell = cell_owner(value);
    return cell && publication && publication->owner && publication->domain &&
        publication->slot != UINT32_MAX && cell->object.domain == publication->domain &&
        !cell->projection_count && !cell->module_storage && !cell->borrow_top ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
}
XR_FUNC void xr_xir_cell_publication_commit(const XrXirValue *value,
    const XrXirCellPublication *publication) {
    XR_CHECK(xr_xir_cell_publication_prepare(value, publication) == XR_XIR_VALUE_OK,
        "cell publication must commit an unchanged prepared owner");
    XirCell *cell = cell_owner(value);
    cell->module_owner = publication->owner;
    cell->module_slot = publication->slot;
    cell->module_storage = true;
}
XR_FUNC bool xr_xir_cell_publication_matches(const XrXirValue *value,
    const XrXirCellPublication *publication) {
    XirCell *cell = cell_root(cell_owner(value));
    return cell && publication && cell->module_storage && cell->module_owner == publication->owner &&
        cell->module_slot == publication->slot && cell->object.domain == publication->domain;
}
XR_FUNC bool xr_xir_cell_module_storage(const XrXirValue *value) {
    XirCell *cell = cell_root(cell_owner(value));
    return cell && cell->module_storage;
}
XR_FUNC bool xr_xir_cell_same_owner(const XrXirValue *left, const XrXirValue *right) {
    XirObject *owner = cell_root_object(cell_owner(left));
    return owner && owner == cell_root_object(cell_owner(right));
}
XR_FUNC bool xr_xir_cell_unborrowed(const XrXirValue *value) {
    XirCell *cell = cell_owner(value);
    return cell && !cell->projection_count && !cell->borrow_top;
}
XR_FUNC bool xr_xir_cell_is_projection(const XrXirValue *value) {
    XirCell *cell = cell_owner(value);
    return cell && cell->projection_count;
}
XR_FUNC const XrXirValue *xr_xir_cell_canonical_root(const XrXirValue *value) {
    XirCell *cell = cell_owner(value);
    return cell ? (cell->projection_count ? &cell->value : value) : NULL;
}
XR_FUNC XrXirValueStatus xr_xir_cell_loan_prepare(const XrXirValue *value,
    const XrXirCellAuthority *parent) {
    XirCell *cell = cell_owner(value);
    XrXirCellLoan **top = cell_root_loans(cell_root_object(cell));
    if (!top) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!*top) return !cell->projection_count ||
        cell_authority_equal(&cell->projection_authority, parent) ?
            XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
    return cell_authority_equal(&(*top)->authority, parent) &&
        cell_scope_contains((*top)->scope, cell) ?
            XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
}
XR_FUNC void xr_xir_cell_loan_commit(const XrXirValue *value, XrXirCellLoan *loan,
    const XrXirCellAuthority *authority) {
    XirCell *cell = cell_owner(value);
    XR_CHECK(cell && loan && !loan->cell && cell_authority_valid(authority),
        "cell loan commits into a fresh live frame record");
    XirObject *owner = cell_root_object(cell);
    XrXirCellLoan **top = cell_root_loans(owner);
    XR_CHECK(top, "cell loans require one stable storage owner");
    loan->cell = owner;
    loan->scope = cell;
    loan->previous = *top;
    loan->authority = *authority;
    *top = loan;
}
XR_FUNC void xr_xir_cell_loan_release(XrXirCellLoan *loan) {
    XR_CHECK(loan && loan->cell, "only a committed cell loan may be released");
    XrXirCellLoan **top = cell_root_loans((XirObject *)loan->cell);
    XR_CHECK(top && *top == loan, "cell loans restore their exact immediate parent");
    *top = loan->previous;
    *loan = (XrXirCellLoan){0};
}
XR_FUNC XrXirValueStatus xr_xir_cell_authorized_read(const XrXirValue *value,
    const XrXirCellAuthority *authority, XrXirValue *output) {
    if (!cell_access(cell_owner(value), authority) || xr_xir_cell_is_projection(value))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = xr_xir_value_copy(&cell_owner(value)->value, output);
    xr_xir_value_graph_end();
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_cell_authorized_write(const XrXirValue *cell,
    const XrXirCellAuthority *authority, const XrXirValue *value, XrXirValueAdmission *admission) {
    if (!cell_access(cell_owner(cell), authority)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = xr_xir_cell_write_graph_operation(cell, value, admission, authority);
    xr_xir_value_graph_end();
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_cell_authorized_place(const XrXirValue *cell,
    const XrXirCellAuthority *authority, XrXirValueAdmission *admission, XrXirValuePlace *output) {
    if (!cell_access(cell_owner(cell), authority)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = xr_xir_cell_value_place_graph_operation(cell, admission, output, authority);
    xr_xir_value_graph_end();
    return status;
}
