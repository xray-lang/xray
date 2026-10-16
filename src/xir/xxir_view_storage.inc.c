/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_storage.inc.c - Range access over an exact live root reservation
 *
 * KEY CONCEPT:
 *   Exclusive storage becomes unique before publication, never after a write.
 */
static bool view_storage_empty(const XrXirViewStorage *view) {
    return view && unit_value(&view->root) && !view->kind && !view->element &&
        !view->offset && !view->length && !view->parent && cell_read_loan_empty(&view->loan.shared);
}
static bool view_storage_work(XrXirValueAdmission *admission,uint64_t work) {
    if (admission->work<work) return false;
    admission->work-=work;return xr_xir_domain_work(admission->domain,work);
}
static bool view_storage_source(const XirCell *cell,XrXirType *element,int64_t *length) {
    const XrXirValue *value=&cell->value;
    if (value->type==XR_XIR_STRING) {
        const XirString *string=(const XirString *)object_pointer(value);
        if (string->length>INT64_MAX) return false;
        *element=XR_XIR_U8;*length=(int64_t)string->length;return true;
    }
    if (!xr_xir_type_is_array(xr_xir_compile_type_arena_types(cell->object.arena),(XrXirType)value->type))
        return false;
    const XirArray *array=(const XirArray *)object_pointer(value);
    if (!array_storage_valid(array) || array->length>INT64_MAX) return false;
    *element=array->element;*length=(int64_t)array->length;return true;
}
static XirCell *view_storage_access(const XrXirViewStorage *view,
    const XrXirCellAuthority *authority,XrXirValueAdmission *admission) {
    if (!view || !admission_domain_current(admission) ||
        (view->kind!=XR_XIR_TYPE_SLICE && view->kind!=XR_XIR_TYPE_MUT_SLICE)) return NULL;
    XirCell *cell=cell_read_loan_root(&view->root);
    if (!cell || !admission_owner(admission,cell->object.arena,cell->object.domain)) return NULL;
    bool shared=view->kind==XR_XIR_TYPE_SLICE;
    const XrXirCellAuthority *origin=shared ? &view->loan.shared.authority : &view->loan.exclusive.authority;
    if (!cell_authority_equal(origin,authority) || !cell_access(cell,authority)) return NULL;
    if (shared) {
        if (view->loan.shared.cell!=cell || !view->loan.shared.previous ||
            *view->loan.shared.previous!=&view->loan.shared || cell->borrow_top!=view->parent) return NULL;
    } else if (view->loan.exclusive.cell!=cell || cell->borrow_top!=&view->loan.exclusive) return NULL;
    XrXirType element;int64_t length;
    return view_storage_source(cell,&element,&length) && element==view->element &&
        view->offset>=0 && view->length>=0 && view->offset<=length && view->length<=length-view->offset ? cell : NULL;
}
static XrXirValueStatus view_storage_begin_operation(const XrXirValue *root,
    const XrXirCellAuthority *parent,const XrXirViewStorageSpec *spec,
    XrXirValueAdmission *admission,XrXirViewStorage *output) {
    if (!view_storage_empty(output) || !spec || !cell_authority_valid(&spec->authority) ||
        (spec->kind!=XR_XIR_TYPE_SLICE && spec->kind!=XR_XIR_TYPE_MUT_SLICE)) return XR_XIR_VALUE_BAD_ARGUMENT;
    bool shared=spec->kind==XR_XIR_TYPE_SLICE;
    XirCell *cell=cell_read_loan_root(root);XrXirType element;int64_t length;
    if (!cell || !admission_owner(admission,cell->object.arena,cell->object.domain) ||
        !view_storage_source(cell,&element,&length) ||
        (!shared && cell->value.type==XR_XIR_STRING) || spec->offset<0 || spec->length<0 ||
        spec->offset>length || spec->length>length-spec->offset ||
        (!!cell->borrow_top!=!!parent) || (parent && !cell_authority_equal(&cell->borrow_top->authority,parent)) ||
        (shared && parent && !cell_authority_equal(parent,&spec->authority)) ||
        (!shared && parent && cell_authority_equal(parent,&spec->authority))) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!view_storage_work(admission,16)) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status=xr_xir_value_copy_graph_operation(root,&output->root);
    if (status!=XR_XIR_VALUE_OK) return status;
    bool prepared=false;XrXirValue replacement={0};
    if (shared) status=xr_xir_cell_read_loan_prepare(root,&spec->authority,admission,&output->loan.shared);
    else status=xr_xir_cell_loan_prepare(root,parent,&output->loan.exclusive);
    if (status!=XR_XIR_VALUE_OK) goto failed;
    prepared=true;output->parent=cell->borrow_top;
    if (!shared && (atomic_load_explicit(&object_pointer(&cell->value)->references,memory_order_acquire)!=1 ||
        object_pointer(&cell->value)->domain!=admission->domain)) {
        const XirArray *source=(const XirArray *)object_pointer(&cell->value);
        uint64_t bytes=source->length*(uint64_t)(source->stride ? source->stride : 1u);
        if (!view_storage_work(admission,bytes)) {status=XR_XIR_VALUE_LIMIT;goto failed;}
        status=path_array_clone(&cell->value,admission,&replacement);
        if (status!=XR_XIR_VALUE_OK) goto failed;
    }
    /* Nothing after this point allocates, charges work or fails. */
    XrXirValue old={0};
    if (replacement.type) {old=cell->value;cell->value=replacement;replacement=(XrXirValue){0};}
    if (shared) xr_xir_cell_read_loan_commit(root,&output->loan.shared,&spec->authority);
    else xr_xir_cell_loan_commit(root,&output->loan.exclusive,&spec->authority);
    output->element=element;output->offset=spec->offset;output->length=spec->length;output->kind=spec->kind;
    xr_xir_value_drop(&old);return XR_XIR_VALUE_OK;
failed:
    if (prepared) {
        if (shared) xr_xir_cell_read_loan_abort(&output->loan.shared);
        else xr_xir_cell_loan_abort(&output->loan.exclusive);
    }
    xr_xir_value_drop(&replacement);xr_xir_value_drop(&output->root);
    *output=(XrXirViewStorage){0};return status;
}
XR_FUNC XrXirValueStatus xr_xir_view_storage_begin(const XrXirValue *root,
    const XrXirCellAuthority *parent,const XrXirViewStorageSpec *spec,
    XrXirValueAdmission *admission,XrXirViewStorage *output) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status=view_storage_begin_operation(root,parent,spec,admission,output);
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_view_storage_reborrow(const XrXirViewStorage *parent,
    const XrXirCellAuthority *authority,const XrXirViewStorageSpec *spec,
    XrXirValueAdmission *admission,XrXirViewStorage *output) {
    if (!view_storage_empty(output) || !spec || !view_storage_access(parent,authority,admission) ||
        (spec->kind==XR_XIR_TYPE_MUT_SLICE && parent->kind!=XR_XIR_TYPE_MUT_SLICE) ||
        spec->offset<0 || spec->length<0 || spec->offset>parent->length ||
        spec->length>parent->length-spec->offset) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (spec->kind==XR_XIR_TYPE_MUT_SLICE) {
        const XirObject *array=object_pointer(&cell_read_loan_root(&parent->root)->value);
        if (atomic_load_explicit(&array->references,memory_order_acquire)!=1 || array->domain!=admission->domain)
            return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    XrXirViewStorageSpec child=*spec;child.offset+=parent->offset;
    /* A shared root with no exclusive ancestor still has live read authority.
     * Only an actual exclusive ancestor supplies begin's parent reservation. */
    const XrXirCellAuthority *root_parent=parent->kind==XR_XIR_TYPE_SLICE && !parent->parent ? NULL : authority;
    xr_xir_value_graph_begin();
    XrXirValueStatus status=view_storage_begin_operation(&parent->root,root_parent,&child,admission,output);
    xr_xir_value_graph_end();return status;
}
XR_FUNC void xr_xir_view_storage_end(XrXirViewStorage *view) {
    if (!view || view_storage_empty(view)) return;
    xr_xir_value_graph_begin();
    XirCell *cell=cell_read_loan_root(&view->root);
    XR_CHECK(cell,"view retirement retains its actual root");
    if (view->kind==XR_XIR_TYPE_SLICE) xr_xir_cell_read_loan_release(&view->loan.shared);
    else {
        XR_CHECK(view->kind==XR_XIR_TYPE_MUT_SLICE && !cell->read_loans && !cell->prepared_read_loan,
            "exclusive view retirement follows all shared children");
        xr_xir_cell_loan_release(&view->loan.exclusive);
    }
    xr_xir_value_drop(&view->root);*view=(XrXirViewStorage){0};xr_xir_value_graph_end();
}
XR_FUNC XrXirValueStatus xr_xir_view_storage_len(const XrXirViewStorage *view,
    const XrXirCellAuthority *authority,XrXirValueAdmission *admission,int64_t *output) {
    if (!output || !view_storage_access(view,authority,admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!view_storage_work(admission,1)) return XR_XIR_VALUE_LIMIT;
    *output=view->length;return XR_XIR_VALUE_OK;
}
static XrXirValueStatus view_storage_index(const XrXirViewStorage *view,int64_t index,
    XrXirValueAdmission *admission,XrXirFaultDetail *fault) {
    if (!view_storage_work(admission,1)) return XR_XIR_VALUE_LIMIT;
    if (index<0 || index>=view->length) {
        *fault=(XrXirFaultDetail){XR_XIR_PANIC_BOUNDS,0,index,view->length};return XR_XIR_VALUE_BOUNDS;
    }
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus view_storage_get_operation(const XrXirViewStorage *view,
    const XrXirCellAuthority *authority,int64_t index,XrXirValueAdmission *admission,
    XrXirValue *output,XrXirFaultDetail *fault) {
    XirCell *cell=view_storage_access(view,authority,admission);
    if (!cell || !unit_value(output) || !fault) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status=view_storage_index(view,index,admission,fault);
    if (status!=XR_XIR_VALUE_OK) return status;
    size_t offset=(size_t)(view->offset+index);
    if (cell->value.type==XR_XIR_STRING) {
        const XirString *string=(const XirString *)object_pointer(&cell->value);
        *output=(XrXirValue){XR_XIR_U8,0,(unsigned char)string->bytes[offset]};return XR_XIR_VALUE_OK;
    }
    XirArray *array=(XirArray *)object_pointer(&cell->value);
    return storage_unpack((StorageSpan){array->element,array_slot(array,offset)},admission,output);
}
XR_FUNC XrXirValueStatus xr_xir_view_storage_get(const XrXirViewStorage *view,
    const XrXirCellAuthority *authority,int64_t index,XrXirValueAdmission *admission,
    XrXirValue *output,XrXirFaultDetail *fault) {
    if (fault) *fault=(XrXirFaultDetail){0};
    xr_xir_value_graph_begin();
    XrXirValueStatus status=view_storage_get_operation(view,authority,index,admission,output,fault);
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_view_storage_set(const XrXirViewStorage *view,
    const XrXirCellAuthority *authority,int64_t index,const XrXirValue *value,
    XrXirValueAdmission *admission,XrXirFaultDetail *fault) {
    if (fault) *fault=(XrXirFaultDetail){0};
    XirCell *cell=view_storage_access(view,authority,admission);
    if (!cell || !fault || view->kind!=XR_XIR_TYPE_MUT_SLICE || !cell_write_access(cell,authority))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirObject *array=object_pointer(&cell->value);
    if (atomic_load_explicit(&array->references,memory_order_acquire)!=1 || array->domain!=admission->domain)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status=view_storage_index(view,index,admission,fault);
    if (status!=XR_XIR_VALUE_OK) return status;
    XrXirValuePlace place={(XrXirType)cell->value.type,&cell->value.payload};
    return xr_xir_array_set(&place,view->offset+index,value,admission,fault);
}
static XrXirValueStatus view_storage_copy_operation(XrXirType type,
    const XrXirViewStorage *view,const XrXirCellAuthority *authority,
    XrXirValueAdmission *admission,XrXirValue *output) {
    XirCell *cell=view_storage_access(view,authority,admission);
    const XrXirTypes *types=admission ? xr_xir_compile_type_arena_types(admission->arena) : NULL;
    if (!cell || !unit_value(output) || !xr_xir_type_is_array(types,type) ||
        xr_xir_array_element(types,type)!=view->element) return XR_XIR_VALUE_BAD_ARGUMENT;
    uint64_t bytes=(uint64_t)view->length;
    if (cell->value.type!=XR_XIR_STRING) {
        const XirArray *source=(const XirArray *)object_pointer(&cell->value);
        bytes*=source->stride ? source->stride : 1u;
    }
    if (bytes==UINT64_MAX || !view_storage_work(admission,bytes+1u)) return XR_XIR_VALUE_LIMIT;
    XirArray *copy=NULL;XrXirValueStatus status=array_allocate(type,(size_t)view->length,admission,&copy);
    if (status!=XR_XIR_VALUE_OK) return status;
    XrXirValue owned=array_value(copy);
    if (cell->value.type==XR_XIR_STRING) {
        const XirString *string=(const XirString *)object_pointer(&cell->value);
        if (view->length) memcpy(copy->data,string->bytes+view->offset,(size_t)view->length);
        copy->length=(size_t)view->length;
    } else {
        XirArray *source=(XirArray *)object_pointer(&cell->value);StoragePack pack={0};
        status=storage_pack_begin(admission,source->element,&pack);
        if (status==XR_XIR_VALUE_OK) {
            for (size_t i=0;i<(size_t)view->length;++i) {
                status=storage_pack_copy(&pack,array_slot(source,(size_t)view->offset+i),array_slot(copy,i));
                if (status!=XR_XIR_VALUE_OK) break;
                ++copy->length;
            }
            storage_pack_end(&pack);
        }
    }
    if (status!=XR_XIR_VALUE_OK) {xr_xir_value_drop(&owned);return status;}
    *output=owned;return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_view_storage_copy(XrXirType type,
    const XrXirViewStorage *view,const XrXirCellAuthority *authority,
    XrXirValueAdmission *admission,XrXirValue *output) {
    xr_xir_value_graph_begin();
    XrXirValueStatus status=view_storage_copy_operation(type,view,authority,admission,output);
    xr_xir_value_graph_end();return status;
}
