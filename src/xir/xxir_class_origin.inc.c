/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class_origin.inc.c - Failure-atomic reachable Class publication
 *
 * KEY CONCEPT:
 *   A temporary owned worklist reuses the actual graph walker, including cycles.
 */
typedef struct ClassPublicationScan {
    XrXirClassPublication *publication;
    XrXirValueStatus status;
} ClassPublicationScan;
static bool class_publication_empty(const XrXirClassPublication *publication) {
    return publication && !publication->admission && !publication->values &&
        !publication->count && !publication->capacity && !publication->bytes &&
        !publication->operation && !publication->committed &&
        !publication->borrow_owner && !publication->borrow_loan;
}
static bool class_publication_work(XrXirValueAdmission *admission, uint64_t work) {
    if (work > admission->work || !xr_xir_domain_work(admission->domain, work)) return false;
    admission->work -= work;
    return true;
}
/* The only exception is the same already-MODULE object held by the immediate
 * path route. Every reachable child is still walked and separately reserved. */
static bool class_publication_borrowed(const XrXirClassPublication *publication,
    const XirClassObject *identity) {
    if (!publication->borrow_owner || publication->borrow_owner != identity) return false;
    const XrXirCellLoan *loan = publication->borrow_loan;
    uint32_t state = class_origin_state(identity);
    return loan && identity->borrow_top == loan && loan->cell == identity &&
        loan->authority.activation && loan->authority.frame && loan->authority.epoch &&
        state == (XIR_CLASS_ORIGIN_MODULE | XIR_CLASS_ORIGIN_BORROWED);
}
static XrXirValueStatus class_publication_grow(XrXirClassPublication *publication) {
    XrXirValueAdmission *admission = publication->admission;
    if (publication->capacity > SIZE_MAX / 2 / sizeof(*publication->values)) return XR_XIR_VALUE_LIMIT;
    size_t capacity = publication->capacity ? publication->capacity * 2 : 4;
    size_t bytes = capacity * sizeof(*publication->values);
    if (bytes > admission->scratch_bytes ||
        !class_publication_work(admission, publication->count * sizeof(*publication->values) + 2))
        return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XrXirValue *values = xr_xir_domain_allocate(admission->domain, bytes, &status);
    if (!values) return status;
    admission->scratch_bytes -= bytes;
    if (publication->values) {
        memcpy(values, publication->values, publication->count * sizeof(*values));
        xr_xir_domain_deallocate(admission->domain, publication->values, publication->bytes);
        admission->scratch_bytes += publication->bytes;
    }
    publication->values = values; publication->capacity = capacity; publication->bytes = bytes;
    return XR_XIR_VALUE_OK;
}
static bool class_publication_edge(const XrXirValue *value, void *payload, void *context) {
    (void)payload;
    ClassPublicationScan *scan = context;
    XrXirClassPublication *publication = scan->publication;
    XrXirValueAdmission *admission = publication->admission;
    if (!class_publication_work(admission, 1)) { scan->status = XR_XIR_VALUE_LIMIT; return false; }
    if (!xr_xir_value_valid(value)) { scan->status = XR_XIR_VALUE_BAD_ARGUMENT; return false; }
    if (!owned_carrier_type((XrXirType)value->type)) return true;
    XirObject *object = object_pointer(value);
    if (empty_enum_object(object) || object->type == XR_XIR_STRING || object->kind == XR_XIR_TYPE_ATOMIC ||
        object->kind == XR_XIR_TYPE_CHANNEL)
        return true;
    for (size_t i = 0; i < publication->count; ++i) {
        if (!class_publication_work(admission, 1)) { scan->status = XR_XIR_VALUE_LIMIT; return false; }
        if (object_pointer(&publication->values[i]) == object) return true;
    }
    bool identity = object->kind == XIR_OBJECT_CLASS;
    if (identity) {
        XrXirValueAdmission local={0};
        scan->status=storage_admission_begin(value,admission,&local);
        if (scan->status!=XR_XIR_VALUE_OK) return false;
        if (!class_allocation_valid(object) || !admission_owner(&local,object->arena,object->domain)) {
            scan->status=XR_XIR_VALUE_BAD_ARGUMENT;return false;
        }
    }
    /* Own-slot write, drop, commit classification and atomic write are prepaid;
     * Class additionally pays reservation CAS and possible rollback. The unused
     * rollback ticket stays charged. Retaining never grants a loan. */
    if (!class_publication_work(admission, identity ? (object == publication->borrow_owner ? 16 : 6) : 3)) {
        scan->status = XR_XIR_VALUE_LIMIT; return false;
    }
    if (publication->count == publication->capacity) {
        scan->status = class_publication_grow(publication);
        if (scan->status != XR_XIR_VALUE_OK) return false;
    }
    XrXirValue owned = {0};
    scan->status = xr_xir_value_copy(value, &owned);
    if (scan->status != XR_XIR_VALUE_OK) return false;
    if (identity) {
        XirClassObject *instance = (XirClassObject *)object;
        if (object == publication->borrow_owner) {
            if (!class_publication_borrowed(publication, instance)) {
                xr_xir_value_drop(&owned); scan->status = XR_XIR_VALUE_BAD_ARGUMENT; return false;
            }
            publication->values[publication->count++] = owned;
            return true;
        }
        if (!class_origin_reserve(instance, false)) {
            xr_xir_value_drop(&owned); scan->status = XR_XIR_VALUE_BAD_ARGUMENT; return false;
        }
        if (instance->borrow_top) {
            class_origin_cancel(instance); xr_xir_value_drop(&owned);
            scan->status = XR_XIR_VALUE_BAD_ARGUMENT; return false;
        }
    }
    publication->values[publication->count++] = owned;
    return true;
}
XR_FUNC void xr_xir_class_publication_drop(XrXirClassPublication *publication) {
    if (!publication || class_publication_empty(publication)) return;
    XrXirClassPublication owned = *publication;
    *publication = (XrXirClassPublication){0};
    XR_CHECK(owned.operation && admission_domain_current(owned.admission),
        "prepared Class publication retains its original admission owner");
    XR_CHECK(!owned.borrow_owner || class_publication_borrowed(&owned, owned.borrow_owner),
        "MODULE route drop retains its exact live loan even for a scalar replacement");
    for (size_t i = 0; i < owned.count; ++i) {
        XirObject *object = object_pointer(&owned.values[i]);
        if (object == owned.borrow_owner) {
            XR_CHECK(class_publication_borrowed(&owned, (XirClassObject *)object),
                "MODULE route drops while its exact active loan remains top");
        } else if (!owned.committed && object->kind == XIR_OBJECT_CLASS) {
            class_origin_cancel((XirClassObject *)object);
        }
        xr_xir_value_drop(&owned.values[i]);
    }
    if (owned.values) {
        xr_xir_domain_deallocate(owned.admission->domain, owned.values, owned.bytes);
        owned.admission->scratch_bytes += owned.bytes;
    }
    xr_xir_value_graph_end();
}
static XrXirValueStatus class_publication_prepare_route(const XrXirValue *values,
    uint32_t count, XrXirValueAdmission *admission, const XirObject *borrow_owner,
    XrXirClassPublication *output) {
    if (!class_publication_empty(output) || !admission_domain_current(admission) ||
        !admission || !admission->domain || (count && !values))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    /* Builtin-only programs have no type arena. Constructed roots still
     * require their actual arena before any work or publication is charged. */
    for (uint32_t i = 0; i < count; ++i)
        if ((!admission->arena && values[i].type >= XR_XIR_CONSTRUCTED_TYPE_BASE) ||
            !xr_xir_value_valid(&values[i])) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirClassPublication publication = {.admission = admission, .operation = true};
    if (borrow_owner) {
        XrXirValue root={(uint32_t)borrow_owner->type,0,0};
        memcpy(&root.payload,&borrow_owner,sizeof(borrow_owner));
        XrXirValueAdmission local={0};
        XrXirValueStatus status=storage_admission_begin(&root,admission,&local);
        if (status!=XR_XIR_VALUE_OK) return status;
        if (borrow_owner->kind != XIR_OBJECT_CLASS || !class_allocation_valid(borrow_owner) ||
            !admission_owner(&local,borrow_owner->arena,borrow_owner->domain))
            return XR_XIR_VALUE_BAD_ARGUMENT;
        publication.borrow_owner = borrow_owner;
        publication.borrow_loan = ((const XirClassObject *)borrow_owner)->borrow_top;
        if (!class_publication_borrowed(&publication, (const XirClassObject *)borrow_owner))
            return XR_XIR_VALUE_BAD_ARGUMENT;
        /* Prepay preparation, commit and drop checks even when the replacement
         * contains no owning object and the identity never enters the worklist. */
        if (!class_publication_work(admission, 24)) return XR_XIR_VALUE_LIMIT;
    }
    /* A legitimate empty preparation also pays its begin/publish/commit/drop
     * control work. Finite-prefix roots and occupied outputs are rejected first. */
    if (!class_publication_work(admission, (uint64_t)count + 4)) return XR_XIR_VALUE_LIMIT;
    ClassPublicationScan scan = {&publication, XR_XIR_VALUE_OK};
    xr_xir_value_graph_begin();
    for (uint32_t i = 0; scan.status == XR_XIR_VALUE_OK && i < count; ++i)
        (void)class_publication_edge(&values[i], NULL, &scan);
    for (size_t i = 0; scan.status == XR_XIR_VALUE_OK && i < publication.count; ++i) {
        GraphEdgeBudget budget = {admission, XR_XIR_VALUE_OK};
        if (!graph_edges_bounded(object_pointer(&publication.values[i]), class_publication_edge, &scan, &budget) &&
            scan.status == XR_XIR_VALUE_OK)
            scan.status = budget.status == XR_XIR_VALUE_OK ? XR_XIR_VALUE_BAD_ARGUMENT : budget.status;
    }
    if (scan.status != XR_XIR_VALUE_OK) { xr_xir_class_publication_drop(&publication); return scan.status; }
    *output = publication;
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_class_publication_prepare(const XrXirValue *values,
    uint32_t count, XrXirValueAdmission *admission, XrXirClassPublication *output) {
    return class_publication_prepare_route(values, count, admission, NULL, output);
}
XR_FUNC void xr_xir_class_publication_commit(XrXirClassPublication *publication) {
    XR_CHECK(publication && publication->operation && !publication->committed &&
        admission_domain_current(publication->admission), "Class publication commits one prepared owner");
    XR_CHECK(!publication->borrow_owner || class_publication_borrowed(publication, publication->borrow_owner),
        "MODULE route commit retains its exact live loan even for a scalar replacement");
    for (size_t i = 0; i < publication->count; ++i) {
        XirObject *object = object_pointer(&publication->values[i]);
        if (object->kind == XIR_OBJECT_CLASS) {
            XirClassObject *instance = (XirClassObject *)object;
            if (object == publication->borrow_owner) {
                XR_CHECK(class_publication_borrowed(publication, instance),
                    "MODULE route commits while its exact active loan remains top");
                continue;
            }
            uint32_t state = class_origin_state(instance);
            XR_CHECK((state & XIR_CLASS_ORIGIN_PREPARED) &&
                !(state & XIR_CLASS_ORIGIN_BORROWED) && !instance->borrow_top,
                "Class publication commits its exclusive unchanged reservation");
            atomic_store_explicit(&instance->publication_state,
                XIR_CLASS_ORIGIN_MODULE, memory_order_release);
        }
    }
    publication->committed = true;
}
XR_FUNC bool xr_xir_class_module_storage(const XrXirValue *value) {
    if (!xr_xir_value_valid(value) || !owned_carrier_type((XrXirType)value->type)) return false;
    const XirObject *object = object_pointer(value);
    return object->kind == XIR_OBJECT_CLASS &&
        (atomic_load_explicit(&((const XirClassObject *)object)->publication_state, memory_order_acquire) &
            XIR_CLASS_ORIGIN_MODULE) != 0;
}
