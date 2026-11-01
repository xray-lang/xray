/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_storage_buffer.inc.c - Nonowning byte ranges with live origin checks
 *
 * KEY CONCEPT:
 *   The origin is retained independently of the weak Buffer identity. Its live
 *   generation must be checked before that identity may be dereferenced.
 */
static XirCell *view_storage_access(const XrXirViewStorage *view,
    const XrXirCellAuthority *authority,XrXirValueAdmission *admission);
static const XirBufferStorage *view_storage_buffer_live(const XrXirViewStorage *view) {
    const XirPointerOrigin *origin=view ? view->buffer_origin : NULL;
    if (!origin || !origin->active || origin->generation!=view->buffer_generation ||
        (!unit_value(&view->root) && (view->kind!=XR_XIR_TYPE_MUT_SLICE || !cell_read_loan_root(&view->root))) ||
        (view->kind!=XR_XIR_TYPE_SLICE && view->kind!=XR_XIR_TYPE_MUT_SLICE) || view->element!=XR_XIR_U8 ||
        !xr_xir_value_argument(&view->buffer_root,NULL,XR_XIR_BUFFER_STORAGE)) return NULL;
    const XirBufferStorage *storage=(const XirBufferStorage *)object_pointer(&view->buffer_root);
    if (origin->domain!=storage->object.domain || storage->origin!=origin ||
        view->offset<0 || view->length<0 || (uint64_t)view->offset>storage->length ||
        (uint64_t)view->length>storage->length-(size_t)view->offset) return NULL;
    return storage;
}
static const XirBufferStorage *view_storage_buffer_access(const XrXirViewStorage *view,
    const XrXirCellAuthority *authority,XrXirValueAdmission *admission) {
    if (!view || !admission_domain_current(admission) || admission_is_compile(admission) ||
        !cell_authority_valid(authority) ||
        !cell_authority_equal(authority,&view->loan.shared.authority)) return NULL;
    const XirBufferStorage *storage=view_storage_buffer_live(view);
    if (!storage || storage->object.domain!=admission->domain || !view->buffer_linked ||
        (view->kind==XR_XIR_TYPE_MUT_SLICE && view->buffer_children)) return NULL;
    if (!unit_value(&view->root)) {
        XirCell *cell=cell_read_loan_root(&view->root);
        if (!cell || !cell_access(cell,authority) || cell->borrow_top!=&view->loan.exclusive ||
            view->loan.exclusive.cell!=cell || view->loan.exclusive.scope!=cell) return NULL;
    }
    const XirPointerOrigin *origin=view->buffer_origin;
    if (view->kind==XR_XIR_TYPE_MUT_SLICE) return origin->exclusive==view && !origin->shared_views ? storage : NULL;
    /* Shared descendants reserve the current exclusive stack top. That top
     * cannot change until every descendant ends, even if an intermediate
     * shared handle is logically retired during a returned-child adoption. */
    return !origin->exclusive || origin->shared_views ? storage : NULL;
}
static bool view_storage_accessible(const XrXirViewStorage *view,
    const XrXirCellAuthority *authority,XrXirValueAdmission *admission) {
    if (!view) return false;
    return view->buffer_origin ? view_storage_buffer_access(view,authority,admission)!=NULL :
        view_storage_access(view,authority,admission)!=NULL;
}
static bool view_storage_buffer_reservable(const XirPointerOrigin *origin,
    const XrXirViewStorage *parent,uint32_t kind) {
    if (!origin) return !parent;
    if (origin->shared_views==UINT32_MAX || (parent && parent->buffer_children==UINT32_MAX)) return false;
    if (parent && (parent->buffer_origin!=origin || !parent->buffer_linked ||
        (parent->kind==XR_XIR_TYPE_MUT_SLICE && parent->buffer_children))) return false;
    if (kind==XR_XIR_TYPE_MUT_SLICE)
        return !origin->shared_views && origin->exclusive==parent &&
            (!parent || parent->kind==XR_XIR_TYPE_MUT_SLICE);
    return !origin->exclusive || (parent && (parent->kind==XR_XIR_TYPE_SLICE || origin->exclusive==parent));
}
static XrXirValueStatus view_storage_buffer_begin(const XrXirValue *root,
    const XrXirViewStorage *parent,const XrXirViewStorageSpec *spec,
    XrXirValueAdmission *admission,XrXirViewStorage *output) {
    if (!view_storage_empty(output) || !spec || !admission_domain_current(admission) ||
        admission_is_compile(admission) || !cell_authority_valid(&spec->authority) ||
        (spec->kind!=XR_XIR_TYPE_SLICE && spec->kind!=XR_XIR_TYPE_MUT_SLICE) ||
        !xr_xir_value_argument(root,NULL,XR_XIR_BUFFER_STORAGE))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XirBufferStorage *storage=(XirBufferStorage *)object_pointer(root);
    if (storage->object.domain!=admission->domain || spec->offset<0 || spec->length<0 ||
        (uint64_t)spec->offset>storage->length ||
        (uint64_t)spec->length>storage->length-(size_t)spec->offset) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!view_storage_buffer_reservable(storage->origin,parent,spec->kind)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!view_storage_work(admission,16)) return XR_XIR_VALUE_LIMIT;
    XirPointerOrigin *origin=storage->origin;bool created=!origin;
    XrXirValueStatus status=created ? pointer_origin_new(admission->domain,&origin) : XR_XIR_VALUE_OK;
    if (status!=XR_XIR_VALUE_OK) return status;
    /* The allocator may observe the caller. Recheck the actual root before
     * linking a new origin; no payload address crosses this callback. */
    if (!xr_xir_value_argument(root,NULL,XR_XIR_BUFFER_STORAGE) ||
        object_pointer(root)!=&storage->object || storage->object.domain!=admission->domain ||
        (created ? storage->origin!=NULL : storage->origin!=origin) || !origin->active ||
        origin->domain!=admission->domain || origin->page || !view_storage_buffer_reservable(origin,parent,spec->kind) ||
        (uint64_t)spec->offset>storage->length ||
        (uint64_t)spec->length>storage->length-(size_t)spec->offset) status=XR_XIR_VALUE_BAD_ARGUMENT;
    else if (!xr_xir_reference_retain(&origin->references)) status=XR_XIR_VALUE_REFCOUNT_LIMIT;
    if (status!=XR_XIR_VALUE_OK) {if (created) pointer_origin_drop(origin);return status;}
    XrXirViewStorage result={0};result.kind=spec->kind;result.element=XR_XIR_U8;
    result.offset=spec->offset;result.length=spec->length;result.loan.shared.authority=spec->authority;
    result.buffer_root=*root;result.buffer_origin=origin;result.buffer_generation=origin->generation;
    result.buffer_parent=(XrXirViewStorage *)parent;result.buffer_linked=true;
    storage->origin=origin;*output=result;
    if (parent) ++output->buffer_parent->buffer_children;
    if (spec->kind==XR_XIR_TYPE_MUT_SLICE) origin->exclusive=output;
    else ++origin->shared_views;
    return XR_XIR_VALUE_OK;
}
static void view_storage_buffer_detach(XrXirViewStorage *view) {
    if (!view->buffer_linked) return;
    XirPointerOrigin *origin=view->buffer_origin;
    XR_CHECK(origin,"Buffer loan retirement retains its actual origin record");
    if (view->kind==XR_XIR_TYPE_MUT_SLICE) {
        XR_CHECK(origin->exclusive==view && !origin->shared_views && !view->buffer_children,
            "an exclusive Buffer loan is the actual stack top without live children");
        origin->exclusive=view->buffer_parent;
    } else {
        XR_CHECK(origin->shared_views,"a shared Buffer loan owns one active reservation");
        --origin->shared_views;
    }
    if (view->buffer_parent) {
        XR_CHECK(view->buffer_parent->buffer_children,"a child owns one parent reservation");
        --view->buffer_parent->buffer_children;view->buffer_parent=NULL;
    }
    view->buffer_linked=false;
    if (!unit_value(&view->root)) {
        XR_CHECK(view->kind==XR_XIR_TYPE_MUT_SLICE,"only an exclusive Buffer scope retains a true Cell loan");
        xr_xir_cell_loan_release(&view->loan.exclusive);
        xr_xir_value_drop(&view->root);
    }
}
static XrXirValueStatus view_storage_buffer_copy(XrXirType type,
    const XrXirViewStorage *view,const XrXirCellAuthority *authority,
    XrXirValueAdmission *admission,XrXirValue *output) {
    const XrXirTypes *types=admission ? xr_xir_compile_type_arena_types(admission->arena) : NULL;
    if (!unit_value(output) || !view_storage_buffer_access(view,authority,admission) ||
        !xr_xir_type_is_array(types,type) || xr_xir_array_element(types,type)!=XR_XIR_U8)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!view_storage_work(admission,(uint64_t)view->length+1u)) return XR_XIR_VALUE_LIMIT;
    XirArray *copy=NULL;XrXirValueStatus status=array_allocate(type,(size_t)view->length,admission,&copy);
    if (status!=XR_XIR_VALUE_OK) return status;
    XrXirValue result=array_value(copy);
    const XirBufferStorage *storage=view_storage_buffer_access(view,authority,admission);
    if (!storage) {xr_xir_value_drop(&result);return XR_XIR_VALUE_BAD_ARGUMENT;}
    if (view->length) memcpy(copy->data,(const uint8_t *)storage->data+view->offset,(size_t)view->length);
    copy->length=(size_t)view->length;*output=result;return XR_XIR_VALUE_OK;
}
static XrXirValueStatus view_storage_buffer_utf8(const XrXirViewStorage *view,
    const XrXirCellAuthority *authority,XrXirValueAdmission *admission,XrXirValue *output) {
    if (!unit_value(output) || !view_storage_buffer_access(view,authority,admission))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!view_storage_work(admission,(uint64_t)view->length+1u)) return XR_XIR_VALUE_LIMIT;
    XirString *copy=NULL;XrXirValueStatus status=string_allocate(admission->domain,(size_t)view->length,&copy);
    if (status!=XR_XIR_VALUE_OK) return status;
    const XirBufferStorage *storage=view_storage_buffer_access(view,authority,admission);
    if (!storage) status=XR_XIR_VALUE_BAD_ARGUMENT;
    else {
        if (view->length) memcpy(copy->bytes,(const uint8_t *)storage->data+view->offset,(size_t)view->length);
        XrUtf8ScanResult scan=xr_utf8_core_scan_strict((const uint8_t *)copy->bytes,copy->length);
        if (scan.error!=XR_UTF8_OK) status=XR_XIR_VALUE_BAD_UTF8;
        else copy->runes=scan.rune_count;
    }
    XrXirValue result=string_value(copy);
    if (status!=XR_XIR_VALUE_OK) {
        memset(copy->bytes,0,copy->length);copy->runes=copy->length;xr_xir_value_drop(&result);
    } else *output=result;
    return status;
}
