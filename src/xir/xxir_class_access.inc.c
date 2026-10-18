/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class_access.inc.c - Short whole-object loans for immediate access
 */
static XrXirValueStatus class_access_owner(const XrXirValue *value,XrXirValueAdmission *admission,
    XirClassObject **output) {
    if (!output || *output || !xr_xir_value_valid(value) || !owned_carrier_type((XrXirType)value->type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission local={0};
    XrXirValueStatus status=storage_admission_begin(value,admission,&local);
    if (status!=XR_XIR_VALUE_OK) return status;
    XirObject *object=object_pointer(value);
    if (object->kind!=XIR_OBJECT_CLASS || !admission_owner(&local,object->arena,object->domain))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    *output=(XirClassObject *)object;return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_class_loan_prepare(const XrXirValue *value,
    const XrXirCellAuthority *parent,XrXirValueAdmission *admission,XrXirCellLoan *output) {
    if (!cell_loan_empty(output)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirClassObject *identity=NULL;
    XrXirValueStatus status=class_access_owner(value,admission,&identity);
    if (status!=XR_XIR_VALUE_OK) return status;
    if (!cell_authority_valid(parent) || !class_origin_reserve(identity,true))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirCellLoan *top=identity->borrow_top;
    uint32_t state=class_origin_state(identity);
    if (((state&XIR_CLASS_ORIGIN_BORROWED)!=0)!=(top!=NULL) ||
        (top && (top->scope || !cell_authority_equal(&top->authority,parent)))) {
        class_origin_cancel(identity);return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    /* NULL scope is a whole Class identity, never a leaf or interior address. */
    *output=(XrXirCellLoan){.cell=identity,.previous=top};return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_class_access_loan_prepare(const XrXirValue *value,
    const XrXirCellAuthority *parent,XrXirValueAdmission *admission,XrXirCellLoan *output) {
    if (!cell_loan_empty(output) || !admission_domain_current(admission))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XirCell *cell=cell_owner(value);
    if (!cell) return xr_xir_class_loan_prepare(value,parent,admission,output);
    XirObject *owner=cell_root_object(cell);XrXirValueAdmission local={0};
    XrXirValueStatus status=storage_admission_begin(value,admission,&local);
    if (status!=XR_XIR_VALUE_OK) return status;
    if (!owner || owner->kind!=XIR_OBJECT_CLASS ||
        !admission_owner(&local,cell->object.arena,cell->object.domain))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    /* A projected child can compare its complete selector prefix with its
     * existing parent. This real bounded walk is paid before the CAS. */
    if (!cell_projection_work(admission,cell->projection_count)) return XR_XIR_VALUE_LIMIT;
    return xr_xir_cell_loan_prepare(value,parent,output);
}
XR_FUNC void xr_xir_class_loan_commit(const XrXirValue *value,XrXirCellLoan *loan,
    const XrXirCellAuthority *authority) {
    XR_CHECK(xr_xir_value_valid(value) && loan && !loan->scope &&
        loan->cell==object_pointer(value) && cell_authority_valid(authority) &&
        !cell_authority_valid(&loan->authority),"whole Class access commits its prepared identity");
    XirClassObject *identity=loan->cell;
    uint32_t state=class_origin_state(identity);
    XR_CHECK(identity->object.kind==XIR_OBJECT_CLASS && identity->borrow_top==loan->previous &&
        (state&XIR_CLASS_ORIGIN_PREPARED),"whole Class access preserves its reserved parent");
    loan->authority=*authority;
    if (loan->previous) {
        XR_CHECK(!loan->previous->child,"a Class parent publishes only its exact youngest reservation");
        loan->previous->child=loan;
    }
    identity->borrow_top=loan;
    atomic_store_explicit(&identity->publication_state,
        (state|XIR_CLASS_ORIGIN_BORROWED)&~(uint32_t)XIR_CLASS_ORIGIN_PREPARED,memory_order_release);
}
static XrXirValueStatus class_access_live(const XrXirValue *value,
    const XrXirCellAuthority *authority,XrXirValueAdmission *admission,XirClassObject **output) {
    XirClassObject *identity=NULL;
    XrXirValueStatus status=class_access_owner(value,admission,&identity);
    if (status!=XR_XIR_VALUE_OK) return status;
    if (!(class_origin_state(identity)&XIR_CLASS_ORIGIN_BORROWED) ||
        !identity->borrow_top || identity->borrow_top->scope ||
        !cell_authority_equal(&identity->borrow_top->authority,authority) ||
        (class_origin_state(identity)&XIR_CLASS_ORIGIN_PREPARED)) return XR_XIR_VALUE_BAD_ARGUMENT;
    *output=identity;return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_class_authorized_path_read(const XrXirValue *value,
    const XrXirCellAuthority *authority,const XrXirValuePath *path,
    XrXirValueAdmission *admission,XrXirValue *output,XrXirFaultDetail *fault) {
    XirClassObject *identity=NULL;
    XrXirValueStatus admitted=class_access_live(value,authority,admission,&identity);
    if (admitted!=XR_XIR_VALUE_OK) return admitted;
    if (!identity || !path || !fault || !unit_value(output)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValuePlace place={(XrXirType)value->type,(void *)&value->payload};
    ValuePathRoute route={.suffix=*path,.borrow_owner=&identity->object,
        .module_publication=xr_xir_class_module_storage(value),.root_arena=identity->object.arena};
    xr_xir_value_graph_begin();
    XrXirValueStatus status=value_path_read_route(&place,&route,admission,output,fault);
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_class_authorized_path_write(const XrXirValue *value,
    const XrXirCellAuthority *authority,const XrXirValuePath *path,
    const XrXirValue *replacement,XrXirValueAdmission *admission,XrXirFaultDetail *fault) {
    XirClassObject *identity=NULL;
    XrXirValueStatus admitted=class_access_live(value,authority,admission,&identity);
    if (admitted!=XR_XIR_VALUE_OK) return admitted;
    if (!identity || !path || !replacement || !fault) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValuePlace place={(XrXirType)value->type,(void *)&value->payload};
    ValuePathRoute route={.suffix=*path,.borrow_owner=&identity->object,
        .module_publication=xr_xir_class_module_storage(value),.root_arena=identity->object.arena};
    xr_xir_value_graph_begin();
    XrXirValueStatus status=path_mutate_route(&place,&route,replacement,admission,fault,false);
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_class_authorized_path_push(const XrXirValue *value,
    const XrXirCellAuthority *authority,const XrXirValuePath *path,
    const XrXirValue *replacement,XrXirValueAdmission *admission,XrXirFaultDetail *fault) {
    XirClassObject *identity=NULL;
    XrXirValueStatus admitted=class_access_live(value,authority,admission,&identity);
    if (admitted!=XR_XIR_VALUE_OK) return admitted;
    if (!identity || !path || !replacement || !fault) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValuePlace place={(XrXirType)value->type,(void *)&value->payload};
    ValuePathRoute route={.suffix=*path,.borrow_owner=&identity->object,
        .module_publication=xr_xir_class_module_storage(value),.root_arena=identity->object.arena};
    xr_xir_value_graph_begin();
    XrXirValueStatus status=path_mutate_route(&place,&route,replacement,admission,fault,true);
    xr_xir_value_graph_end();return status;
}
