/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_borrowed_value.inc.c - Scoped aggregate admission and atomic owner transfer
 *
 * KEY CONCEPT:
 *   Shared physical DAG nodes are admitted once; all failures precede consumption.
 */
typedef struct BorrowedVisit {
    XirObject **objects,**slots,**first;
    uint32_t count,capacity;
    uint64_t bytes;
} BorrowedVisit;
static uint32_t borrowed_hash(const XirObject *object) {
    /* Metering cannot depend on allocator addresses. Physical identity remains
     * the equality key, and every collision is explicitly charged. */
    uint64_t value=object->type;
    value^=value>>33;value*=UINT64_C(0xff51afd7ed558ccd);value^=value>>33;
    return (uint32_t)value;
}
static XrXirValueStatus borrowed_visit_grow(BorrowedVisit *visit,XrXirValueAdmission *admission) {
    if (visit->capacity>UINT32_MAX/4u) return XR_XIR_VALUE_LIMIT;
    uint32_t capacity=visit->capacity ? visit->capacity*2u : 64u;
    uint64_t bytes=(uint64_t)capacity*3u*sizeof(XirObject *);
    if (bytes>SIZE_MAX || bytes>admission->scratch_bytes || !view_storage_work(admission,bytes))
        return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    XirObject **objects=visit->capacity ? xr_xir_domain_allocate(admission->domain,(size_t)bytes,&status) : visit->first;
    if (!objects) return status;
    XirObject **slots=objects+capacity;
    memset(slots,0,(size_t)capacity*2u*sizeof(*slots));
    if (visit->count) memcpy(objects,visit->objects,(size_t)visit->count*sizeof(*objects));
    for (uint32_t i=0;i<visit->count;++i) {
        uint32_t at=borrowed_hash(objects[i])&(capacity*2u-1u);
        while (slots[at]) {
            if (!view_storage_work(admission,1)) {status=XR_XIR_VALUE_LIMIT;break;}
            at=(at+1u)&(capacity*2u-1u);
        }
        if (status!=XR_XIR_VALUE_OK) break;
        slots[at]=objects[i];
    }
    if (status!=XR_XIR_VALUE_OK) {
        if (objects!=visit->first) xr_xir_domain_deallocate(admission->domain,objects,(size_t)bytes);
        return status;
    }
    admission->scratch_bytes-=bytes;
    if (visit->objects) {
        if (visit->objects!=visit->first) xr_xir_domain_deallocate(admission->domain,visit->objects,(size_t)visit->bytes);
        admission->scratch_bytes+=visit->bytes;
    }
    visit->objects=objects;visit->slots=slots;visit->capacity=capacity;visit->bytes=bytes;return XR_XIR_VALUE_OK;
}
static XrXirValueStatus borrowed_visit_push(BorrowedVisit *visit,const XrXirValue *value,
    XrXirType type,const XrXirViewAccess *access) {
    XrXirValueAdmission *admission=access->values;
    if (!view_storage_work(admission,1)) return XR_XIR_VALUE_LIMIT;
    if (type==XR_XIR_UNIT) return unit_value(value) ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_compile_type_arena_borrowed(admission->arena,type)) return xr_xir_value_admit(value,type,admission);
    if (!xr_xir_value_argument(value,admission->arena,type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirObject *object=object_pointer(value);
    if (object->domain!=admission->domain) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!visit->capacity) {
        XrXirValueStatus status=borrowed_visit_grow(visit,admission);if (status!=XR_XIR_VALUE_OK) return status;
    }
    uint32_t at=borrowed_hash(object)&(visit->capacity*2u-1u);
    while (visit->slots[at]) {
        if (!view_storage_work(admission,1)) return XR_XIR_VALUE_LIMIT;
        if (visit->slots[at]==object) return XR_XIR_VALUE_OK;
        at=(at+1u)&(visit->capacity*2u-1u);
    }
    if (visit->count==visit->capacity) {
        XrXirValueStatus status=borrowed_visit_grow(visit,admission);if (status!=XR_XIR_VALUE_OK) return status;
        at=borrowed_hash(object)&(visit->capacity*2u-1u);
        while (visit->slots[at]) {
            if (!view_storage_work(admission,1)) return XR_XIR_VALUE_LIMIT;
            at=(at+1u)&(visit->capacity*2u-1u);
        }
    }
    visit->objects[visit->count++]=object;visit->slots[at]=object;return XR_XIR_VALUE_OK;
}
typedef XrXirValueStatus (*BorrowedLeafInspection)(const XirView *view,
    const XrXirCellAuthority *authority,XrXirValueAdmission *admission,void *context);
static XrXirValueStatus borrowed_leaf_inspect(const XrXirValue *value,const XrXirViewAccess *access,
    BorrowedLeafInspection inspect,void *context) {
    XrXirCellAuthority authority={0};
    XrXirValueStatus status=view_value_permission(value,access,false,&authority);
    return status==XR_XIR_VALUE_OK && inspect ?
        inspect((const XirView *)object_pointer(value),&authority,access->values,context) : status;
}
static XrXirValueStatus borrowed_inspect_operation(const XrXirValue *value,XrXirType type,
    const XrXirViewAccess *access,BorrowedLeafInspection inspect,void *context) {
    if (!access || !access->authorize || !admission_domain_current(access->values)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission *admission=access->values;
    if (owned_carrier_type(type) && xr_xir_value_argument(value,admission->arena,type) &&
        view_value_kind(object_pointer(value)->kind)) {
        return borrowed_leaf_inspect(value,access,inspect,context);
    }
    XirObject *first[192];BorrowedVisit visit={.first=first};
    XrXirValueStatus status=borrowed_visit_push(&visit,value,type,access);
    for (uint32_t at=0;at<visit.count && status==XR_XIR_VALUE_OK;++at) {
        XirObject *object=visit.objects[at];
        if (view_value_kind(object->kind)) {
            XrXirValue handle=view_value_handle((const XirView *)object);
            status=borrowed_leaf_inspect(&handle,access,inspect,context);continue;
        }
        const XrXirValue *fields=NULL;uint32_t count=0;
        if (object->kind==XR_XIR_TYPE_TUPLE) {fields=((const XirTuple *)object)->fields;count=((const XirTuple *)object)->count;}
        else if (object->kind==XR_XIR_TYPE_NULLABLE) {
            fields=((const XirNominalValue *)object)->fields;count=((const XirNominalValue *)object)->count;
        } else {status=XR_XIR_VALUE_BAD_ARGUMENT;break;}
        for (uint32_t i=0;i<count && status==XR_XIR_VALUE_OK;++i)
            status=borrowed_visit_push(&visit,&fields[i],(XrXirType)fields[i].type,access);
    }
    if (visit.objects) {
        if (visit.objects!=visit.first) xr_xir_domain_deallocate(admission->domain,visit.objects,(size_t)visit.bytes);
        admission->scratch_bytes+=visit.bytes;
    }
    return status;
}
static XrXirValueStatus borrowed_admit_operation(const XrXirValue *value,XrXirType type,
    const XrXirViewAccess *access) {
    return borrowed_inspect_operation(value,type,access,NULL,NULL);
}
XR_FUNC XrXirValueStatus xr_xir_borrowed_value_admit(const XrXirValue *value,XrXirType type,
    const XrXirViewAccess *access) {
    xr_xir_value_graph_begin();XrXirValueStatus status=borrowed_admit_operation(value,type,access);
    xr_xir_value_graph_end();return status;
}
typedef struct ViewOriginMatch {
    const XirView *origin;
    BorrowedVisit proven;
    bool memoize;
} ViewOriginMatch;
static XrXirValueStatus view_origin_proven(const BorrowedVisit *visit,const XirObject *object,
    XrXirValueAdmission *admission,bool *output) {
    *output=false;
    if (!visit->capacity) return XR_XIR_VALUE_OK;
    uint32_t at=borrowed_hash(object)&(visit->capacity*2u-1u);
    while (visit->slots[at]) {
        if (!view_storage_work(admission,1)) return XR_XIR_VALUE_LIMIT;
        if (visit->slots[at]==object) {*output=true;break;}
        at=(at+1u)&(visit->capacity*2u-1u);
    }
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus view_origin_leaf_match(const XirView *leaf,
    const XrXirCellAuthority *authority,XrXirValueAdmission *admission,void *context) {
    ViewOriginMatch *match=context;(void)authority;
    if (!match->origin) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirView *view=leaf;XrXirValueStatus status=XR_XIR_VALUE_OK;bool proven=false;
    while (view!=match->origin) {
        if (!view_storage_work(admission,1)) return XR_XIR_VALUE_LIMIT;
        status=view_origin_proven(&match->proven,&view->object,admission,&proven);
        if (status!=XR_XIR_VALUE_OK || proven) break;
        if (unit_value(&view->parent) || !xr_xir_value_valid(&view->parent)) return XR_XIR_VALUE_BAD_ARGUMENT;
        const XirObject *parent=object_pointer(&view->parent);
        if (!view_value_kind(parent->kind) || parent->arena!=admission->arena || parent->domain!=admission->domain)
            return XR_XIR_VALUE_BAD_ARGUMENT;
        view=(const XirView *)parent;
    }
    if (status!=XR_XIR_VALUE_OK) return status;
    /* A direct view has no other returned leaf that could reuse this prefix. */
    if (!match->memoize) return XR_XIR_VALUE_OK;
    /* Publish only proven prefixes. An unproved cycle cannot become a match;
     * each ancestry edge is metered even before any memo allocation succeeds. */
    view=leaf;XrXirViewAccess physical={.values=admission};
    while (view!=match->origin) {
        status=view_origin_proven(&match->proven,&view->object,admission,&proven);
        if (status!=XR_XIR_VALUE_OK || proven) break;
        XrXirValue value=view_value_handle(view);
        status=borrowed_visit_push(&match->proven,&value,(XrXirType)value.type,&physical);
        if (status!=XR_XIR_VALUE_OK) break;
        view=(const XirView *)object_pointer(&view->parent);
    }
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_borrowed_value_origin_match(const XrXirValue *value,XrXirType type,
    const XrXirViewOriginPin *candidate,const XrXirViewAccess *access) {
    if (!access || !access->authorize || !admission_domain_current(access->values) ||
        !xr_xir_compile_type_arena_borrowed(access->values->arena,type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (candidate && !candidate->body && (candidate->authority.activation || candidate->authority.frame ||
        candidate->authority.epoch)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();XrXirValueAdmission *admission=access->values;
    const XrXirTypeNode *node=xr_xir_type_node(xr_xir_compile_type_arena_types(admission->arena),type);
    XirObject *first[192];ViewOriginMatch match={.origin=candidate ? candidate->body : NULL,
        .proven={.first=first},.memoize=!view_value_kind(node->kind)};
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    if (match.origin) {
        XrXirViewOriginInfo info={0};status=xr_xir_view_origin_info(candidate,admission,&info);
    }
    if (status==XR_XIR_VALUE_OK) status=borrowed_inspect_operation(value,type,access,view_origin_leaf_match,&match);
    if (match.proven.objects) {
        if (match.proven.objects!=match.proven.first)
            xr_xir_domain_deallocate(admission->domain,match.proven.objects,(size_t)match.proven.bytes);
        admission->scratch_bytes+=match.proven.bytes;
    }
    xr_xir_value_graph_end();return status;
}
static XrXirValueStatus borrowed_origin_pin_leaf(const XirView *view,
    const XrXirCellAuthority *authority,XrXirValueAdmission *admission,void *context) {
    XrXirViewOriginPin *pin=context;
    if (pin->body) return XR_XIR_VALUE_BAD_ARGUMENT;
    return view_origin_pin_hold((XirView *)view,authority,admission,pin);
}
XR_FUNC XrXirValueStatus xr_xir_borrowed_value_origin_pin(const XrXirValue *value,XrXirType type,
    const XrXirViewAccess *access,XrXirViewOriginPin *output) {
    if (!output || output->body || output->authority.activation || output->authority.frame || output->authority.epoch ||
        !access || !access->authorize || !admission_domain_current(access->values) ||
        xr_xir_compile_type_arena_borrowed_leaves(access->values->arena,type)!=1) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();XrXirViewOriginPin prepared={0};
    XrXirValueStatus status=borrowed_inspect_operation(value,type,access,borrowed_origin_pin_leaf,&prepared);
    if (status==XR_XIR_VALUE_OK) {*output=prepared;prepared=(XrXirViewOriginPin){0};}
    xr_xir_view_origin_unpin(&prepared);xr_xir_value_graph_end();return status;
}
static XrXirValueStatus borrowed_tuple_move_operation(XrXirType type,XrXirValue *fields,
    uint32_t count,const XrXirViewAccess *access,XrXirValue *output) {
    if (!unit_value(output) || !access || !admission_domain_current(access->values) || !fields || !count)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission *admission=access->values;
    const XrXirTypeNode *node=xr_xir_tuple_signature(xr_xir_compile_type_arena_types(admission->arena),type);
    if (!node || node->parameter_count!=count || !xr_xir_compile_type_arena_borrowed(admission->arena,type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    uint64_t bytes=sizeof(XirTuple)+(uint64_t)count*sizeof(XrXirValue);
    if (bytes>SIZE_MAX || !view_storage_work(admission,bytes+(uint64_t)count*sizeof(XrXirValue)))
        return XR_XIR_VALUE_LIMIT;
    for (uint32_t i=0;i<count;++i) {
        if (output==&fields[i]) return XR_XIR_VALUE_BAD_ARGUMENT;
        XrXirValueStatus status=borrowed_admit_operation(&fields[i],node->parameters[i].type,access);
        if (status!=XR_XIR_VALUE_OK) return status;
    }
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    XirTuple *tuple=(XirTuple *)constructed_allocate(admission->domain,(XrXirTypeArena *)admission->arena,type,(size_t)bytes,&status);
    if (!tuple) return status;
    tuple->count=count;tuple->fields=(XrXirValue *)(tuple+1);
    memcpy(tuple->fields,fields,(size_t)count*sizeof(*fields));
    for (uint32_t i=0;i<count;++i) fields[i]=(XrXirValue){0};
    XrXirValue result={(uint32_t)type,0,0};memcpy(&result.payload,&tuple,sizeof(tuple));
    xr_xir_value_object_publish(&tuple->object);*output=result;return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_borrowed_tuple_new_move(XrXirType type,XrXirValue *fields,
    uint32_t count,const XrXirViewAccess *access,XrXirValue *output) {
    xr_xir_value_graph_begin();XrXirValueStatus status=borrowed_tuple_move_operation(type,fields,count,access,output);
    xr_xir_value_graph_end();return status;
}
static XrXirValueStatus borrowed_some_move_operation(XrXirType type,XrXirValue *value,
    const XrXirViewAccess *access,XrXirValue *output) {
    if (!unit_value(output) || output==value || !access || !admission_domain_current(access->values))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission *admission=access->values;
    const XrXirTypeNode *node=xr_xir_type_node(xr_xir_compile_type_arena_types(admission->arena),type);
    if (!node || node->kind!=XR_XIR_TYPE_NULLABLE || !xr_xir_compile_type_arena_borrowed(admission->arena,type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    uint64_t bytes=sizeof(XirNominalValue)+sizeof(XrXirValue);
    if (!view_storage_work(admission,bytes+sizeof(XrXirValue))) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status=borrowed_admit_operation(value,node->element,access);
    if (status!=XR_XIR_VALUE_OK) return status;
    XirNominalValue *record=(XirNominalValue *)constructed_allocate(admission->domain,
        (XrXirTypeArena *)admission->arena,type,(size_t)bytes,&status);
    if (!record) return status;
    record->count=1;record->variant=1;record->fields=(XrXirValue *)(record+1);
    record->fields[0]=*value;*value=(XrXirValue){0};
    XrXirValue result={(uint32_t)type,0,0};memcpy(&result.payload,&record,sizeof(record));
    xr_xir_value_object_publish(&record->object);*output=result;return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_borrowed_some_new_move(XrXirType type,XrXirValue *value,
    const XrXirViewAccess *access,XrXirValue *output) {
    xr_xir_value_graph_begin();XrXirValueStatus status=borrowed_some_move_operation(type,value,access,output);
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_borrowed_tuple_unpack_move(XrXirValue *value,
    XrXirValue *fields,uint32_t count,const XrXirViewAccess *access) {
    if (!value || !fields || !access || !admission_domain_current(access->values) ||
        !xr_xir_value_argument(value,access->values->arena,(XrXirType)value->type) ||
        !xr_xir_compile_type_arena_borrowed(access->values->arena,(XrXirType)value->type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypeNode *node=xr_xir_tuple_signature(xr_xir_compile_type_arena_types(access->values->arena),(XrXirType)value->type);
    if (!node || node->parameter_count!=count || !count) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirTuple *tuple=(XirTuple *)object_pointer(value);
    if (atomic_load_explicit(&tuple->object.references,memory_order_acquire)!=1) return XR_XIR_VALUE_BAD_ARGUMENT;
    for (uint32_t i=0;i<count;++i) if (&fields[i]==value || !unit_value(&fields[i])) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status=borrowed_admit_operation(value,(XrXirType)value->type,access);
    uint64_t bytes=sizeof(XirTuple)+(uint64_t)count*sizeof(XrXirValue);
    if (status==XR_XIR_VALUE_OK && !view_storage_work(access->values,bytes)) status=XR_XIR_VALUE_LIMIT;
    if (status==XR_XIR_VALUE_OK) {
        memcpy(fields,tuple->fields,(size_t)count*sizeof(*fields));*value=(XrXirValue){0};
        constructed_discard(&tuple->object,(size_t)bytes);
    }
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_borrowed_some_take(XrXirValue *value,
    const XrXirViewAccess *access,XrXirValue *output) {
    if (!value || value==output || !unit_value(output) || !access || !admission_domain_current(access->values) ||
        !xr_xir_value_argument(value,access->values->arena,(XrXirType)value->type) ||
        !xr_xir_compile_type_arena_borrowed(access->values->arena,(XrXirType)value->type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypeNode *node=xr_xir_type_node(xr_xir_compile_type_arena_types(access->values->arena),(XrXirType)value->type);
    if (!node || node->kind!=XR_XIR_TYPE_NULLABLE) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirNominalValue *record=(XirNominalValue *)object_pointer(value);
    if (record->count!=1 || atomic_load_explicit(&record->object.references,memory_order_acquire)!=1)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status=borrowed_admit_operation(value,(XrXirType)value->type,access);
    size_t bytes=sizeof(XirNominalValue)+sizeof(XrXirValue);
    if (status==XR_XIR_VALUE_OK && !view_storage_work(access->values,bytes)) status=XR_XIR_VALUE_LIMIT;
    if (status==XR_XIR_VALUE_OK) {
        XrXirValue payload=record->fields[0];*value=(XrXirValue){0};
        constructed_discard(&record->object,bytes);*output=payload;
    }
    xr_xir_value_graph_end();return status;
}
