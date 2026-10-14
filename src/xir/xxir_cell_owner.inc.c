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
    if (!top || (owner->kind == XIR_OBJECT_CELL && ((const XirCell *)owner)->prepared_loan) ||
        (owner->kind == XIR_OBJECT_CLASS &&
        (class_origin_state((const XirClassObject *)owner) & XIR_CLASS_ORIGIN_PREPARED))) return false;
    if (*top) {
        const XirCell *scope = (*top)->scope;
        return cell_authority_equal(&(*top)->authority, authority) &&
            (!scope || !scope->projection_count || scope == cell);
    }
    if (cell->projection_count) return cell_authority_equal(&cell->projection_authority, authority);
    return !((const XirCell *)owner)->module_storage || cell_authority_valid(authority);
}
XR_FUNC XrXirValueStatus xr_xir_cell_publication_prepare(const XrXirValue *value,
    const XrXirCellPublication *publication) {
    XirCell *cell = cell_owner(value);
    return cell && publication && publication->owner && publication->domain &&
        publication->slot != UINT32_MAX && cell->object.domain == publication->domain &&
        !cell->projection_count && !cell->module_storage && !cell->borrow_top &&
        !cell->prepared_loan ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
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
    const XirObject *owner = cell_root_object(cell_owner(value));
    if (!owner) return false;
    if (owner->kind == XIR_OBJECT_CLASS)
        return (class_origin_state((const XirClassObject *)owner) & XIR_CLASS_ORIGIN_MODULE) != 0;
    return owner->kind == XIR_OBJECT_CELL && ((const XirCell *)owner)->module_storage;
}
XR_FUNC bool xr_xir_cell_same_owner(const XrXirValue *left, const XrXirValue *right) {
    XirObject *owner = cell_root_object(cell_owner(left));
    return owner && owner == cell_root_object(cell_owner(right));
}
XR_FUNC bool xr_xir_cell_unborrowed(const XrXirValue *value) {
    XirCell *cell = cell_owner(value);
    return cell && !cell->projection_count && !cell->borrow_top && !cell->prepared_loan;
}
XR_FUNC bool xr_xir_cell_is_projection(const XrXirValue *value) {
    XirCell *cell = cell_owner(value);
    return cell && cell->projection_count;
}
XR_FUNC const XrXirValue *xr_xir_cell_canonical_root(const XrXirValue *value) {
    XirCell *cell = cell_owner(value);
    return cell ? (cell->projection_count ? &cell->value : value) : NULL;
}
static bool cell_loan_empty(const XrXirCellLoan *loan) {
    return loan && !loan->cell && !loan->scope && !loan->previous &&
        !loan->authority.activation && !loan->authority.frame && !loan->authority.epoch;
}
XR_FUNC XrXirValueStatus xr_xir_cell_loan_prepare(const XrXirValue *value,
    const XrXirCellAuthority *parent, XrXirCellLoan *output) {
    if (!cell_loan_empty(output)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirCell *cell = cell_owner(value);
    XirObject *owner = cell_root_object(cell);
    XrXirCellLoan **top = cell_root_loans(owner);
    if (!top) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirCell *storage = owner->kind == XIR_OBJECT_CELL ? (XirCell *)owner : NULL;
    if (storage && storage->prepared_loan) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirClassObject *identity = owner->kind == XIR_OBJECT_CLASS ? (XirClassObject *)owner : NULL;
    if (identity && !class_origin_reserve(identity, true)) return XR_XIR_VALUE_BAD_ARGUMENT;
    bool allowed = !*top ? !cell->projection_count ||
        cell_authority_equal(&cell->projection_authority, parent) :
        cell_authority_equal(&(*top)->authority, parent) && (!(*top)->scope || cell_scope_contains((*top)->scope, cell));
    if (identity && ((class_origin_state(identity) & XIR_CLASS_ORIGIN_BORROWED) != 0) != (*top != NULL))
        allowed = false;
    if (!allowed) {
        if (identity) class_origin_cancel(identity);
        return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    *output = (XrXirCellLoan){owner, cell, *top, {0}};
    /* Preparation excludes all aliases until this exact record commits or aborts. */
    if (storage) storage->prepared_loan = output;
    return XR_XIR_VALUE_OK;
}
XR_FUNC void xr_xir_cell_loan_abort(XrXirCellLoan *loan) {
    XR_CHECK(loan && loan->cell && !cell_authority_valid(&loan->authority),
        "only a prepared unpublished cell loan may be aborted");
    XirObject *owner = loan->cell;
    XrXirCellLoan **top = cell_root_loans(owner);
    XR_CHECK(top && *top == loan->previous, "prepared loan preserves its exact original parent");
    if (owner->kind == XIR_OBJECT_CLASS) class_origin_cancel((XirClassObject *)owner);
    else {
        XirCell *storage = (XirCell *)owner;
        XR_CHECK(storage->prepared_loan == loan, "Cell abort owns its exact exclusive reservation");
        storage->prepared_loan = NULL;
    }
    *loan = (XrXirCellLoan){0};
}
XR_FUNC void xr_xir_cell_loan_commit(const XrXirValue *value, XrXirCellLoan *loan,
    const XrXirCellAuthority *authority) {
    XirCell *cell = cell_owner(value);
    XirObject *owner = cell_root_object(cell);
    XR_CHECK(cell && loan && loan->cell == owner && loan->scope == cell &&
        !cell_authority_valid(&loan->authority) && cell_authority_valid(authority),
        "cell loan commits exactly its prepared scope into a live frame");
    XrXirCellLoan **top = cell_root_loans(owner);
    XR_CHECK(top && *top == loan->previous, "cell loan commit preserves its prepared parent");
    uint32_t state = owner->kind == XIR_OBJECT_CLASS ? class_origin_state((XirClassObject *)owner) : 0;
    XR_CHECK(owner->kind != XIR_OBJECT_CLASS || (state & XIR_CLASS_ORIGIN_PREPARED),
        "Class loan commit owns the actual exclusive reservation");
    XR_CHECK(owner->kind != XIR_OBJECT_CELL || ((XirCell *)owner)->prepared_loan == loan,
        "Cell loan commit owns its exact exclusive reservation");
    loan->authority = *authority; *top = loan;
    if (owner->kind == XIR_OBJECT_CLASS)
        atomic_store_explicit(&((XirClassObject *)owner)->publication_state,
            (state | XIR_CLASS_ORIGIN_BORROWED) & ~(uint32_t)XIR_CLASS_ORIGIN_PREPARED, memory_order_release);
    else ((XirCell *)owner)->prepared_loan = NULL;
}
XR_FUNC void xr_xir_cell_loan_release(XrXirCellLoan *loan) {
    XR_CHECK(loan && loan->cell && cell_authority_valid(&loan->authority),
        "only a committed cell loan may be released");
    XirObject *owner = loan->cell;
    XirClassObject *identity = owner->kind == XIR_OBJECT_CLASS ? (XirClassObject *)owner : NULL;
    XR_CHECK(!identity || class_origin_reserve(identity, true),
        "live Class loan release cannot race a prepared child or publication");
    XR_CHECK(identity || !((const XirCell *)owner)->prepared_loan,
        "live Cell loan release cannot race a prepared child");
    XrXirCellLoan **top = cell_root_loans(owner);
    XR_CHECK(top && *top == loan, "cell loans restore their exact immediate parent");
    *top = loan->previous;
    if (identity) {
        uint32_t state = class_origin_state(identity) & ~(uint32_t)XIR_CLASS_ORIGIN_PREPARED;
        if (!*top) state &= ~(uint32_t)XIR_CLASS_ORIGIN_BORROWED;
        atomic_store_explicit(&identity->publication_state, state, memory_order_release);
    }
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
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!cell_access(cell_owner(cell), authority)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = xr_xir_cell_write_graph_operation(cell, value, admission, authority);
    xr_xir_value_graph_end();
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_cell_authorized_place(const XrXirValue *cell,
    const XrXirCellAuthority *authority, XrXirValueAdmission *admission, XrXirValuePlace *output) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!cell_access(cell_owner(cell), authority)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = xr_xir_cell_value_place_graph_operation(cell, admission, output, authority);
    xr_xir_value_graph_end();
    return status;
}
