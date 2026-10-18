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
static XrXirValueStatus cell_scope_contains(const XirCell *scope, const XirCell *target,
    XrXirValueAdmission *admission, bool *contains) {
    *contains = false;
    if (!scope || !target || cell_root_object(scope) != cell_root_object(target)) return XR_XIR_VALUE_OK;
    if (!scope->projection_count) { *contains = true; return XR_XIR_VALUE_OK; }
    if (scope->projection_count > target->projection_count) return XR_XIR_VALUE_OK;
    const XrXirValuePathStep *a = (const XrXirValuePathStep *)(scope + 1);
    const XrXirValuePathStep *b = (const XrXirValuePathStep *)(target + 1);
    for (uint32_t i = 0; i < scope->projection_count; ++i) {
        if (a[i].kind != b[i].kind || a[i].container != b[i].container) return XR_XIR_VALUE_OK;
        if (a[i].kind == XR_XIR_PATH_KEY) {
            if (!admission) return XR_XIR_VALUE_BAD_ARGUMENT;
            XrXirValue left = path_key_value(&a[i], scope->object.arena);
            XrXirValue right = path_key_value(&b[i], target->object.arena);
            bool equal = false;
            XrXirValueStatus status = xr_xir_key_equal(&left, &right, admission, &equal);
            if (status != XR_XIR_VALUE_OK) return status;
            if (!equal) return XR_XIR_VALUE_OK;
        } else if (a[i].selector != b[i].selector) return XR_XIR_VALUE_OK;
    }
    *contains = true; return XR_XIR_VALUE_OK;
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
static bool cell_write_access(const XirCell *cell,const XrXirCellAuthority *authority) {
    if (!cell_access(cell,authority)) return false;
    const XirObject *owner=cell_root_object(cell);
    if (owner->kind!=XIR_OBJECT_CELL) return true;
    const XirCell *root=(const XirCell *)owner;
    return !root->read_loans && !root->prepared_read_loan;
}
XR_FUNC XrXirValueStatus xr_xir_cell_publication_prepare(const XrXirValue *value,
    const XrXirCellPublication *publication) {
    XirCell *cell = cell_owner(value);
    return cell && publication && publication->owner && publication->domain &&
        publication->slot != UINT32_MAX && cell->object.domain == publication->domain &&
        !cell->projection_count && !cell->module_storage && !cell->borrow_top &&
        !cell->prepared_loan && !cell->read_loans && !cell->prepared_read_loan ?
        XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
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
    return cell && !cell->projection_count && !cell->borrow_top && !cell->prepared_loan &&
        !cell->read_loans && !cell->prepared_read_loan;
}
XR_FUNC bool xr_xir_cell_is_projection(const XrXirValue *value) {
    XirCell *cell = cell_owner(value);
    return cell && cell->projection_count;
}
XR_FUNC XrXirValueStatus xr_xir_cell_projection_steps(const XrXirValue *value,
    XrXirValueAdmission *admission,uint32_t *output) {
    if (!output || !admission || !admission->arena || !admission->domain || !admission_domain_current(admission))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (admission->work<64 || !xr_xir_domain_work(admission->domain,64)) return XR_XIR_VALUE_LIMIT;
    admission->work-=64;
    const XirCell *cell=cell_owner(value);
    if (!cell || cell->object.arena!=admission->arena || cell->object.domain!=admission->domain)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    *output=cell->projection_count;return XR_XIR_VALUE_OK;
}
XR_FUNC const XrXirValue *xr_xir_cell_canonical_root(const XrXirValue *value) {
    XirCell *cell = cell_owner(value);
    return cell ? (cell->projection_count ? &cell->value : value) : NULL;
}
XR_FUNC XrXirValueStatus xir_cell_path_view(const XrXirValue *value,XrXirValuePath *output) {
    XirCell *cell=cell_owner(value);
    if (!cell || !output) return XR_XIR_VALUE_BAD_ARGUMENT;
    *output=(XrXirValuePath){cell->projection_count ? (const XrXirValuePathStep *)(cell+1) : NULL,cell->projection_count};
    return XR_XIR_VALUE_OK;
}
static bool cell_loan_empty(const XrXirCellLoan *loan) {
    return loan && !loan->cell && !loan->scope && !loan->previous && !loan->child &&
        !loan->authority.activation && !loan->authority.frame && !loan->authority.epoch;
}
static XrXirValueStatus cell_loan_prepare_operation(const XrXirValue *value,
    const XrXirCellAuthority *parent, XrXirCellLoan *output, XrXirValueAdmission *admission) {
    if (!cell_loan_empty(output)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirCell *cell = cell_owner(value);
    XirObject *owner = cell_root_object(cell);
    XrXirCellLoan **top = cell_root_loans(owner);
    if (!top) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirCell *storage = owner->kind == XIR_OBJECT_CELL ? (XirCell *)owner : NULL;
    if (storage && (storage->prepared_loan || storage->read_loans || storage->prepared_read_loan))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XirClassObject *identity = owner->kind == XIR_OBJECT_CLASS ? (XirClassObject *)owner : NULL;
    if (identity && !class_origin_reserve(identity, true)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    bool allowed = !*top ? !cell->projection_count ||
        cell_authority_equal(&cell->projection_authority, parent) :
        cell_authority_equal(&(*top)->authority, parent);
    if (allowed && *top && (*top)->scope)
        status = cell_scope_contains((*top)->scope, cell, admission, &allowed);
    if (identity && ((class_origin_state(identity) & XIR_CLASS_ORIGIN_BORROWED) != 0) != (*top != NULL))
        allowed = false;
    if (!allowed || status != XR_XIR_VALUE_OK) {
        if (identity) class_origin_cancel(identity);
        return status != XR_XIR_VALUE_OK ? status : XR_XIR_VALUE_BAD_ARGUMENT;
    }
    *output = (XrXirCellLoan){.cell=owner,.scope=cell,.previous=*top};
    /* Preparation excludes all aliases until this exact record commits or aborts. */
    if (storage) storage->prepared_loan = output;
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_cell_loan_prepare(const XrXirValue *value,
    const XrXirCellAuthority *parent, XrXirCellLoan *output) {
    return cell_loan_prepare_operation(value, parent, output, NULL);
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
    loan->authority = *authority;
    if (loan->previous) {
        XR_CHECK(!loan->previous->child,"the published parent has no younger exclusive reservation");
        loan->previous->child=loan;
    }
    *top = loan;
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
    XR_CHECK(identity || !((const XirCell *)owner)->prepared_read_loan,
        "live Cell loan release cannot race a prepared read reservation");
    XrXirCellLoan **top = cell_root_loans(owner);
    XR_CHECK(top && *top == loan && !loan->child, "cell loans restore their exact immediate parent");
    if (!identity) {
        XirCell *cell=(XirCell *)owner;
        /* Read guards exclude new exclusive loans, so every published guard
         * descends from this ending scope. Delegation may change its authority;
         * revoking every link visits each prepaid record at most once. */
        XrXirCellReadLoan *read=cell->read_loans;
        while (read) {
            XrXirCellReadLoan *next=read->next;
            XR_CHECK(read->previous && *read->previous==read,"scope revocation owns the exact read link");
            *read->previous=next;
            if (next) next->previous=read->previous;
            read->previous=NULL;read->next=NULL;read->authority=(XrXirCellAuthority){0};
            read=next;
        }
    }
    *top = loan->previous;
    if (loan->previous) {
        XR_CHECK(loan->previous->child==loan,"exclusive parent preserves its exact youngest child");
        loan->previous->child=NULL;
    }
    if (identity) {
        uint32_t state = class_origin_state(identity) & ~(uint32_t)XIR_CLASS_ORIGIN_PREPARED;
        if (!*top) state &= ~(uint32_t)XIR_CLASS_ORIGIN_BORROWED;
        atomic_store_explicit(&identity->publication_state, state, memory_order_release);
    }
    *loan = (XrXirCellLoan){0};
}
/* Whole-Cell view reservations may retire before their physical child pins.
 * Both stack directions make terminal unlink independent of stack depth. */
static void cell_loan_detach(XrXirCellLoan *loan) {
    XR_CHECK(loan && loan->cell && cell_authority_valid(&loan->authority),"detachment owns a live exclusive reservation");
    XirCell *cell=loan->cell;
    XR_CHECK(cell->object.kind==XIR_OBJECT_CELL && loan->scope==cell && !cell->projection_count &&
        !cell->prepared_loan && !cell->prepared_read_loan,"view detachment preserves its whole stable root");
    if (loan->child) {
        XR_CHECK(loan->child->previous==loan,"detachment preserves the exact younger reservation");
        loan->child->previous=loan->previous;
    } else {
        XR_CHECK(cell->borrow_top==loan,"the youngest detached reservation is the root head");
        cell->borrow_top=loan->previous;
    }
    if (loan->previous) {
        XR_CHECK(loan->previous->child==loan,"detachment preserves the exact older reservation");
        loan->previous->child=loan->child;
    }
    *loan=(XrXirCellLoan){0};
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
    if (!cell_write_access(cell_owner(cell), authority)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = xr_xir_cell_write_graph_operation(cell, value, admission, authority);
    xr_xir_value_graph_end();
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_cell_authorized_place(const XrXirValue *cell,
    const XrXirCellAuthority *authority, XrXirValueAdmission *admission, XrXirValuePlace *output) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!cell_write_access(cell_owner(cell), authority)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = xr_xir_cell_value_place_graph_operation(cell, admission, output, authority);
    xr_xir_value_graph_end();
    return status;
}
