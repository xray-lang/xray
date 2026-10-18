/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_value.inc.c - Stable physical view objects with separate scope checks
 *
 * KEY CONCEPT:
 *   A child physically pins its parent's loan record without copying permission.
 */
static bool view_value_kind(uint32_t kind) {
    return kind==XR_XIR_TYPE_SLICE || kind==XR_XIR_TYPE_MUT_SLICE;
}
static bool view_value_valid(const XirView *view) {
    const XrXirViewStorage *storage=&view->storage;
    const XrXirTypeNode *node=xr_xir_type_node(xr_xir_compile_type_arena_types(view->object.arena),view->object.type);
    XirCell *cell=cell_read_loan_root(&storage->root);XrXirType element;int64_t length;
    if (!node || !cell || storage->kind!=view->object.kind || node->element!=storage->element ||
        cell->object.arena!=view->object.arena || cell->object.domain!=view->object.domain ||
        storage->offset<0 || storage->length<0 || storage->length>INT64_MAX-storage->offset) return false;
    bool revoked=storage->kind==XR_XIR_TYPE_SLICE && !storage->loan.shared.previous;
    if (view->state==XIR_VIEW_ACTIVE && !revoked && (!view_storage_source(cell,&element,&length) ||
        element!=storage->element || storage->offset>length || storage->length>length-storage->offset)) return false;
    if (view->state==XIR_VIEW_RETIRED) {
        if (!cell_loan_empty(&storage->loan.exclusive)) return false;
    } else if (view->state!=XIR_VIEW_ACTIVE) return false;
    else if (storage->kind==XR_XIR_TYPE_SLICE) {
        const XrXirCellReadLoan *loan=&storage->loan.shared;
        if (loan->cell!=cell) return false;
        if (revoked) {
            if (loan->next || loan->authority.activation || loan->authority.frame || loan->authority.epoch) return false;
        } else if (*loan->previous!=loan || !cell_authority_valid(&loan->authority)) return false;
    } else {
        const XrXirCellLoan *loan=&storage->loan.exclusive;
        if (cell->value.type==XR_XIR_STRING || loan->cell!=cell || loan->scope!=cell ||
            !cell_authority_valid(&loan->authority) ||
            (loan->child ? loan->child->previous!=loan : cell->borrow_top!=loan) ||
            (loan->previous && loan->previous->child!=loan)) return false;
    }
    if (!unit_value(&view->parent)) {
        if (!value_header_valid(&view->parent)) return false;
        const XirObject *parent=object_pointer(&view->parent);
        if (!view_value_kind(parent->kind) || parent->arena!=view->object.arena ||
            parent->domain!=view->object.domain || !((const XirView *)parent)->children) return false;
    }
    return true;
}
static void view_value_detach(XirView *view) {
    XR_CHECK(view->state==XIR_VIEW_ACTIVE,"a logical view reservation retires exactly once");
    if (view->storage.kind==XR_XIR_TYPE_MUT_SLICE) cell_loan_detach(&view->storage.loan.exclusive);
    else {
        XR_CHECK(view->storage.kind==XR_XIR_TYPE_SLICE,"retirement retains a known view kind");
        xr_xir_cell_read_loan_release(&view->storage.loan.shared);
    }
    view->state=XIR_VIEW_RETIRED;
}
static XrXirValue view_value_handle(const XirView *view) {
    XrXirValue result={(uint32_t)view->object.type,0,0};
    memcpy(&result.payload,&view,sizeof(view));return result;
}
static XrXirValueStatus view_value_permission(const XrXirValue *value,const XrXirViewAccess *access,
    bool writable,XrXirCellAuthority *authority) {
    if (!access || !access->authorize || !admission_domain_current(access->values) ||
        !xr_xir_value_valid(value) || !owned_carrier_type((XrXirType)value->type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirObject *object=object_pointer(value);
    if (!view_value_kind(object->kind) || ((const XirView *)object)->state!=XIR_VIEW_ACTIVE ||
        !admission_owner(access->values,object->arena,object->domain) ||
        (writable && object->kind!=XR_XIR_TYPE_MUT_SLICE)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!view_storage_work(access->values,1)) return XR_XIR_VALUE_LIMIT;
    *authority=(XrXirCellAuthority){0};
    XrXirValueStatus status=access->authorize(access->context,value,writable,access->values,authority);
    if (status!=XR_XIR_VALUE_OK) return status;
    return view_storage_access(&((const XirView *)object)->storage,authority,access->values) ?
        XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
}
static bool view_value_type(XrXirType type,const XrXirViewStorageSpec *spec,
    XrXirValueAdmission *admission,XrXirType *element) {
    if (!spec || !admission_domain_current(admission)) return false;
    const XrXirTypeNode *node=xr_xir_type_node(xr_xir_compile_type_arena_types(admission->arena),type);
    if (!node || !view_value_kind(node->kind) || node->kind!=spec->kind) return false;
    *element=node->element;return true;
}
static XrXirValueStatus view_origin_pin_hold(XirView *view,const XrXirCellAuthority *authority,
    XrXirValueAdmission *admission,XrXirViewOriginPin *output) {
    XR_CHECK(output && !output->body && cell_authority_valid(authority),"origin publication follows exact leaf authentication");
    if (view->children==UINT32_MAX) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    if (!view_storage_work(admission,sizeof(*output)+2*sizeof(void *))) return XR_XIR_VALUE_LIMIT;
    if (!graph_object_retain(&view->object)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    ++view->children;*output=(XrXirViewOriginPin){view,*authority};return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_view_origin_pin(const XrXirValue *value,
    const XrXirViewAccess *access,XrXirViewOriginPin *output) {
    if (!output || output->body || output->authority.activation || output->authority.frame ||
        output->authority.epoch) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();XrXirCellAuthority authority={0};
    XrXirValueStatus status=view_value_permission(value,access,false,&authority);
    if (status==XR_XIR_VALUE_OK)
        status=view_origin_pin_hold((XirView *)object_pointer(value),&authority,access->values,output);
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_view_origin_info(const XrXirViewOriginPin *pin,
    XrXirValueAdmission *admission,XrXirViewOriginInfo *output) {
    if (!pin || !pin->body || !cell_authority_valid(&pin->authority) || !output ||
        !admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirView *view=pin->body;XrXirValue value=view_value_handle(view);
    if (!xr_xir_value_valid(&value) || !view_value_kind(view->object.kind) || !view->children ||
        !admission_owner(admission,view->object.arena,view->object.domain)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!view_storage_work(admission,sizeof(*output))) return XR_XIR_VALUE_LIMIT;
    bool logical=view->state==XIR_VIEW_ACTIVE &&
        (view->storage.kind==XR_XIR_TYPE_MUT_SLICE || view->storage.loan.shared.previous);
    *output=(XrXirViewOriginInfo){&view->storage.root,pin->authority,view->storage.kind,
        view->storage.element,view->storage.offset,view->storage.length,logical};
    return XR_XIR_VALUE_OK;
}
XR_FUNC void xr_xir_view_origin_unpin(XrXirViewOriginPin *pin) {
    if (!pin) return;
    if (!pin->body) {
        XR_CHECK(!pin->authority.activation && !pin->authority.frame && !pin->authority.epoch,
            "an empty physical origin pin has no authority snapshot");
        return;
    }
    xr_xir_value_graph_begin();XirView *view=pin->body;XrXirValue value=view_value_handle(view);
    XR_CHECK(cell_authority_valid(&pin->authority) && xr_xir_value_valid(&value) &&
        view_value_kind(view->object.kind) && view->children,"physical origin release owns one exact retained identity");
    --view->children;*pin=(XrXirViewOriginPin){0};
    xr_xir_value_drop(&value);xr_xir_value_graph_end();
}
XR_FUNC void xr_xir_view_scope_pin_created(const XrXirValue *value,
    const XrXirCellAuthority *authority,XrXirViewOriginPin *output) {
    xr_xir_value_graph_begin();
    XR_CHECK(xr_xir_value_valid(value) && output && !output->body &&
        !output->authority.activation && !output->authority.frame && !output->authority.epoch &&
        cell_authority_valid(authority),"a prepaid scope pin follows successful view construction");
    XirObject *object=object_pointer(value);
    XR_CHECK(view_value_kind(object->kind),"a scope pin only retains a created view body");
    XirView *view=(XirView *)object;
    const XrXirCellAuthority *actual=view->storage.kind==XR_XIR_TYPE_SLICE ?
        &view->storage.loan.shared.authority : &view->storage.loan.exclusive.authority;
    XR_CHECK(view_value_kind(view->object.kind) && view->state==XIR_VIEW_ACTIVE &&
        !view->children && atomic_load_explicit(&view->object.references,memory_order_acquire)==1 &&
        cell_authority_equal(actual,authority),"only the exact newly created view enters its scope registry");
    XR_CHECK(graph_object_retain(&view->object),"a unique created view has room for one physical scope pin");
    ++view->children;*output=(XrXirViewOriginPin){view,*authority};xr_xir_value_graph_end();
}
XR_FUNC void xr_xir_view_scope_close(XrXirViewOriginPin *pin) {
    if (!pin || !pin->body) {xr_xir_view_origin_unpin(pin);return;}
    xr_xir_value_graph_begin();XirView *view=pin->body;XrXirValue value=view_value_handle(view);
    XR_CHECK(xr_xir_value_valid(&value) && view_value_kind(view->object.kind) && view->children,
        "scope retirement owns a physical pin until permission is revoked");
    if (view->state==XIR_VIEW_ACTIVE) {
        const XrXirCellAuthority *actual=view->storage.kind==XR_XIR_TYPE_SLICE ?
            &view->storage.loan.shared.authority : &view->storage.loan.exclusive.authority;
        bool revoked=view->storage.kind==XR_XIR_TYPE_SLICE && !view->storage.loan.shared.previous;
        XR_CHECK(revoked || cell_authority_equal(actual,&pin->authority),"scope closing never revokes another authority");
        view_value_detach(view);
    }
    xr_xir_view_origin_unpin(pin);xr_xir_value_graph_end();
}
XR_FUNC bool xr_xir_view_scope_active(const XrXirViewOriginPin *pin) {
    if (!pin || !pin->body) return false;
    xr_xir_value_graph_begin();const XirView *view=pin->body;XrXirValue value=view_value_handle(view);
    XR_CHECK(xr_xir_value_valid(&value) && view_value_kind(view->object.kind) && view->children,
        "scope metadata owns an exact physical view pin");
    bool active=view->state==XIR_VIEW_ACTIVE &&
        (view->storage.kind==XR_XIR_TYPE_MUT_SLICE || view->storage.loan.shared.previous);
    xr_xir_value_graph_end();return active;
}
static XrXirValueStatus view_value_new_operation(XrXirType type,const XrXirValue *root,
    const XrXirCellAuthority *parent,const XrXirViewStorageSpec *spec,
    XrXirValueAdmission *admission,XrXirValue *output) {
    XrXirType element,actual;int64_t length;
    if (!unit_value(output) || !view_value_type(type,spec,admission,&element)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirCell *cell=cell_read_loan_root(root);
    if (!cell || !admission_owner(admission,cell->object.arena,cell->object.domain) ||
        !cell_authority_valid(&spec->authority) || !view_storage_source(cell,&actual,&length) || actual!=element ||
        spec->offset<0 || spec->length<0 || spec->offset>length || spec->length>length-spec->offset ||
        (spec->kind==XR_XIR_TYPE_MUT_SLICE && cell->value.type==XR_XIR_STRING)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!view_storage_work(admission,sizeof(XirView))) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    XirView *view=(XirView *)constructed_allocate(admission->domain,(XrXirTypeArena *)admission->arena,type,sizeof(*view),&status);
    if (!view) return status;
    view->storage=(XrXirViewStorage){0};view->parent=(XrXirValue){0};view->children=0;view->state=XIR_VIEW_ACTIVE;
    status=xr_xir_view_storage_begin(root,parent,spec,admission,&view->storage);
    if (status!=XR_XIR_VALUE_OK) {constructed_discard(&view->object,sizeof(*view));return status;}
    xr_xir_value_object_publish(&view->object);*output=view_value_handle(view);return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_view_new(XrXirType type,const XrXirValue *root,
    const XrXirCellAuthority *parent,const XrXirViewStorageSpec *spec,
    XrXirValueAdmission *admission,XrXirValue *output) {
    xr_xir_value_graph_begin();XrXirValueStatus status=view_value_new_operation(type,root,parent,spec,admission,output);
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_view_reborrow(XrXirType type,const XrXirValue *parent,
    const XrXirViewStorageSpec *spec,const XrXirViewAccess *access,XrXirValue *output) {
    if (!unit_value(output) || !spec) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();XrXirCellAuthority authority={0};XrXirType element;
    XrXirValueStatus status=view_value_permission(parent,access,spec->kind==XR_XIR_TYPE_MUT_SLICE,&authority);
    XirView *source=status==XR_XIR_VALUE_OK ? (XirView *)object_pointer(parent) : NULL;
    if (status==XR_XIR_VALUE_OK && (!view_value_type(type,spec,access->values,&element) ||
        source->storage.element!=element)) status=XR_XIR_VALUE_BAD_ARGUMENT;
    if (status==XR_XIR_VALUE_OK && (source->children==UINT32_MAX ||
        !view_storage_work(access->values,sizeof(XirView)+1u))) status=XR_XIR_VALUE_LIMIT;
    XirView *view=NULL;
    if (status==XR_XIR_VALUE_OK) view=(XirView *)constructed_allocate(access->values->domain,
        (XrXirTypeArena *)access->values->arena,type,sizeof(*view),&status);
    if (view) {
        view->storage=(XrXirViewStorage){0};view->parent=(XrXirValue){0};view->children=0;view->state=XIR_VIEW_ACTIVE;
        if (!graph_object_retain(&source->object)) status=XR_XIR_VALUE_REFCOUNT_LIMIT;
        else {
            view->parent=*parent;
            status=xr_xir_view_storage_reborrow(&source->storage,&authority,spec,access->values,&view->storage);
        }
        if (status==XR_XIR_VALUE_OK) {
            ++source->children;xr_xir_value_object_publish(&view->object);*output=view_value_handle(view);
        } else {
            xr_xir_value_drop(&view->parent);constructed_discard(&view->object,sizeof(*view));
        }
    }
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_view_len(const XrXirValue *value,const XrXirViewAccess *access,int64_t *output) {
    if (!output) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirCellAuthority authority={0};XrXirValueStatus status=view_value_permission(value,access,false,&authority);
    return status==XR_XIR_VALUE_OK ? xr_xir_view_storage_len(&((const XirView *)object_pointer(value))->storage,
        &authority,access->values,output) : status;
}
XR_FUNC XrXirValueStatus xr_xir_view_get(const XrXirValue *value,int64_t index,
    const XrXirViewAccess *access,XrXirValue *output,XrXirFaultDetail *fault) {
    if (fault) *fault=(XrXirFaultDetail){0};
    if (!fault || !unit_value(output)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirCellAuthority authority={0};XrXirValueStatus status=view_value_permission(value,access,false,&authority);
    return status==XR_XIR_VALUE_OK ? xr_xir_view_storage_get(&((const XirView *)object_pointer(value))->storage,
        &authority,index,access->values,output,fault) : status;
}
XR_FUNC XrXirValueStatus xr_xir_view_set(const XrXirValue *value,int64_t index,const XrXirValue *element,
    const XrXirViewAccess *access,XrXirFaultDetail *fault) {
    if (fault) *fault=(XrXirFaultDetail){0};
    if (!fault) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirCellAuthority authority={0};XrXirValueStatus status=view_value_permission(value,access,true,&authority);
    return status==XR_XIR_VALUE_OK ? xr_xir_view_storage_set(&((const XirView *)object_pointer(value))->storage,
        &authority,index,element,access->values,fault) : status;
}
XR_FUNC XrXirValueStatus xr_xir_view_copy(XrXirType array,const XrXirValue *value,
    const XrXirViewAccess *access,XrXirValue *output) {
    if (!unit_value(output)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirCellAuthority authority={0};XrXirValueStatus status=view_value_permission(value,access,false,&authority);
    return status==XR_XIR_VALUE_OK ? xr_xir_view_storage_copy(array,&((const XirView *)object_pointer(value))->storage,
        &authority,access->values,output) : status;
}
