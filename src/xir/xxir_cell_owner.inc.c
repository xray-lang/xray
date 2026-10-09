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
    return object->kind == XR_XIR_TYPE_CELL ? (XirCell *)object : NULL;
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
    if (cell->borrow_top) return cell_authority_equal(&cell->borrow_top->authority, authority);
    return !cell->module_storage || cell_authority_valid(authority);
}
XR_FUNC XrXirValueStatus xr_xir_cell_publication_prepare(const XrXirValue *value,
    const XrXirCellPublication *publication) {
    XirCell *cell = cell_owner(value);
    return cell && publication && publication->owner && publication->domain &&
        publication->slot != UINT32_MAX && cell->object.domain == publication->domain &&
        !cell->module_storage && !cell->borrow_top ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
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
    XirCell *cell = cell_owner(value);
    return cell && publication && cell->module_storage && cell->module_owner == publication->owner &&
        cell->module_slot == publication->slot && cell->object.domain == publication->domain;
}
XR_FUNC bool xr_xir_cell_module_storage(const XrXirValue *value) {
    XirCell *cell = cell_owner(value);
    return cell && cell->module_storage;
}
XR_FUNC bool xr_xir_cell_same_owner(const XrXirValue *left, const XrXirValue *right) {
    XirCell *cell = cell_owner(left);
    return cell && cell == cell_owner(right);
}
XR_FUNC bool xr_xir_cell_unborrowed(const XrXirValue *value) {
    XirCell *cell = cell_owner(value);
    return cell && !cell->borrow_top;
}
XR_FUNC XrXirValueStatus xr_xir_cell_loan_prepare(const XrXirValue *value,
    const XrXirCellAuthority *parent) {
    XirCell *cell = cell_owner(value);
    return cell && (!cell->borrow_top || cell_authority_equal(&cell->borrow_top->authority, parent)) ?
        XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
}
XR_FUNC void xr_xir_cell_loan_commit(const XrXirValue *value, XrXirCellLoan *loan,
    const XrXirCellAuthority *authority) {
    XirCell *cell = cell_owner(value);
    XR_CHECK(cell && loan && !loan->cell && cell_authority_valid(authority),
        "cell loan commits into a fresh live frame record");
    loan->cell = cell;
    loan->previous = cell->borrow_top;
    loan->authority = *authority;
    cell->borrow_top = loan;
}
XR_FUNC void xr_xir_cell_loan_release(XrXirCellLoan *loan) {
    XR_CHECK(loan && loan->cell, "only a committed cell loan may be released");
    XirCell *cell = (XirCell *)loan->cell;
    XR_CHECK(cell->borrow_top == loan, "cell loans restore their exact immediate parent");
    cell->borrow_top = loan->previous;
    *loan = (XrXirCellLoan){0};
}
XR_FUNC XrXirValueStatus xr_xir_cell_authorized_read(const XrXirValue *value,
    const XrXirCellAuthority *authority, XrXirValue *output) {
    if (!cell_access(cell_owner(value), authority)) return XR_XIR_VALUE_BAD_ARGUMENT;
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
