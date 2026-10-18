/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_transfer.inc.c - Failure-atomic rebuilding of borrowed wrapper DAGs
 *
 * KEY CONCEPT:
 *   Immutable descriptor order places every wrapper after its child types.
 */
typedef struct BorrowedClone {
    BorrowedVisit visit;
    XrXirValue *values;
    uint32_t *indices;
    uint64_t bytes;
    const XrXirViewTransfer *transfer;
} BorrowedClone;
static XrXirValueStatus borrowed_clone_leaf(const XrXirValue *value,
    const XrXirViewTransfer *transfer,XrXirValue *output) {
    XrXirViewOriginPin origin={0};XrXirValue result={0};
    XrXirValueStatus status=xr_xir_view_origin_pin(value,transfer->source,&origin);
    if (status==XR_XIR_VALUE_OK) status=transfer->borrow(transfer->context,value,&result);
    if (status==XR_XIR_VALUE_OK && (!xr_xir_value_argument(&result,transfer->source->values->arena,(XrXirType)value->type) ||
        object_pointer(&result)==object_pointer(value))) status=XR_XIR_VALUE_BAD_ARGUMENT;
    if (status==XR_XIR_VALUE_OK)
        status=xr_xir_borrowed_value_origin_match(&result,(XrXirType)value->type,&origin,transfer->destination);
    if (status==XR_XIR_VALUE_OK) {
        XrXirViewOriginInfo info={0};status=xr_xir_view_origin_info(&origin,transfer->source->values,&info);
        if (status==XR_XIR_VALUE_OK && !info.logical_active) status=XR_XIR_VALUE_BAD_ARGUMENT;
    }
    if (status==XR_XIR_VALUE_OK) {*output=result;result=(XrXirValue){0};}
    xr_xir_value_drop(&result);xr_xir_view_origin_unpin(&origin);return status;
}
static XrXirValueStatus borrowed_clone_inventory(BorrowedClone *clone,const XrXirValue *value,XrXirType type) {
    const XrXirViewAccess *access=clone->transfer->source;
    XrXirValueStatus status=borrowed_admit_operation(value,type,access);
    if (status==XR_XIR_VALUE_OK) status=borrowed_visit_push(&clone->visit,value,type,access);
    for (uint32_t i=0;i<clone->visit.count && status==XR_XIR_VALUE_OK;++i) {
        XirObject *object=clone->visit.objects[i];const XrXirValue *fields=NULL;uint32_t count=0;
        if (view_value_kind(object->kind)) continue;
        if (object->kind==XR_XIR_TYPE_TUPLE) {fields=((XirTuple *)object)->fields;count=((XirTuple *)object)->count;}
        else if (object->kind==XR_XIR_TYPE_NULLABLE) {fields=((XirNominalValue *)object)->fields;count=((XirNominalValue *)object)->count;}
        else return XR_XIR_VALUE_BAD_ARGUMENT;
        for (uint32_t f=0;f<count && status==XR_XIR_VALUE_OK;++f)
            status=borrowed_visit_push(&clone->visit,&fields[f],(XrXirType)fields[f].type,access);
    }
    return status;
}
static XrXirValueStatus borrowed_clone_sift(BorrowedClone *clone,uint32_t start,uint32_t count) {
    XrXirValueAdmission *admission=clone->transfer->source->values;XirObject **objects=clone->visit.objects;
    while (start<count/2u) {
        if (!view_storage_work(admission,3)) return XR_XIR_VALUE_LIMIT;
        uint32_t child=start*2u+1u;
        if (child+1u<count && objects[child]->type<objects[child+1u]->type) ++child;
        if (objects[start]->type>=objects[child]->type) break;
        XirObject *temporary=objects[start];objects[start]=objects[child];objects[child]=temporary;start=child;
    }
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus borrowed_clone_prepare(BorrowedClone *clone) {
    XrXirValueAdmission *admission=clone->transfer->source->values;uint32_t count=clone->visit.count;
    for (uint32_t i=count/2u;i;--i) {
        XrXirValueStatus status=borrowed_clone_sift(clone,i-1u,count);if (status!=XR_XIR_VALUE_OK) return status;
    }
    for (uint32_t i=count;i>1u;--i) {
        if (!view_storage_work(admission,1)) return XR_XIR_VALUE_LIMIT;
        XirObject *temporary=clone->visit.objects[0];clone->visit.objects[0]=clone->visit.objects[i-1u];clone->visit.objects[i-1u]=temporary;
        XrXirValueStatus status=borrowed_clone_sift(clone,0,i-1u);if (status!=XR_XIR_VALUE_OK) return status;
    }
    uint64_t bytes=(uint64_t)count*sizeof(XrXirValue)+(uint64_t)clone->visit.capacity*2u*sizeof(uint32_t);
    if (bytes>SIZE_MAX || bytes>admission->scratch_bytes || !view_storage_work(admission,bytes)) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    clone->values=xr_xir_domain_allocate(admission->domain,(size_t)bytes,&status);
    if (!clone->values) return status;
    clone->bytes=bytes;admission->scratch_bytes-=bytes;memset(clone->values,0,(size_t)bytes);
    clone->indices=(uint32_t *)(clone->values+count);
    for (uint32_t i=0;i<count;++i) {
        if (!view_storage_work(admission,1)) return XR_XIR_VALUE_LIMIT;
        uint32_t at=borrowed_hash(clone->visit.objects[i])&(clone->visit.capacity*2u-1u);
        while (clone->indices[at]) {
            if (!view_storage_work(admission,1)) return XR_XIR_VALUE_LIMIT;
            at=(at+1u)&(clone->visit.capacity*2u-1u);
        }
        clone->indices[at]=i+1u;
    }
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus borrowed_clone_index(BorrowedClone *clone,const XirObject *object,uint32_t *output) {
    XrXirValueAdmission *admission=clone->transfer->source->values;
    uint32_t at=borrowed_hash(object)&(clone->visit.capacity*2u-1u);
    for (;;) {
        if (!view_storage_work(admission,1)) return XR_XIR_VALUE_LIMIT;
        uint32_t index=clone->indices[at];if (!index) return XR_XIR_VALUE_BAD_ARGUMENT;
        if (clone->visit.objects[index-1u]==object) {*output=index-1u;return XR_XIR_VALUE_OK;}
        at=(at+1u)&(clone->visit.capacity*2u-1u);
    }
}
static XrXirValueStatus borrowed_clone_field(BorrowedClone *clone,const XrXirValue *source,
    XrXirType parent,XrXirValue *output) {
    XrXirValueAdmission *admission=clone->transfer->source->values;
    uint32_t flags=xr_xir_compile_type_arena_borrowed(admission->arena,(XrXirType)source->type);
    if (!flags) return xr_xir_value_copy(source,output);
    if (source->type>=(uint32_t)parent) return XR_XIR_VALUE_BAD_ARGUMENT;
    uint32_t index=0;XrXirValueStatus status=borrowed_clone_index(clone,object_pointer(source),&index);
    if (status!=XR_XIR_VALUE_OK) return status;
    if (!clone->values[index].type) return XR_XIR_VALUE_BAD_ARGUMENT;
    return flags&XR_XIR_VIEW_MUTABLE ? xr_xir_value_move(&clone->values[index],output) :
        xr_xir_value_copy(&clone->values[index],output);
}
static XrXirValueStatus borrowed_clone_wrapper(BorrowedClone *clone,uint32_t index) {
    XirObject *source=clone->visit.objects[index];XrXirValueAdmission *admission=clone->transfer->source->values;
    bool tuple=source->kind==XR_XIR_TYPE_TUPLE;
    const XrXirValue *fields=tuple ? ((XirTuple *)source)->fields : ((XirNominalValue *)source)->fields;
    uint32_t count=tuple ? ((XirTuple *)source)->count : ((XirNominalValue *)source)->count;
    size_t header=tuple ? sizeof(XirTuple) : sizeof(XirNominalValue);
    uint64_t bytes=header+(uint64_t)count*sizeof(XrXirValue);
    if (bytes>SIZE_MAX || !view_storage_work(admission,bytes+(uint64_t)count*sizeof(XrXirValue))) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    XirObject *created=constructed_allocate(admission->domain,(XrXirTypeArena *)admission->arena,source->type,(size_t)bytes,&status);
    if (!created) return status;
    XrXirValue *owned=count ? (XrXirValue *)((unsigned char *)created+header) : NULL;
    if (count) memset(owned,0,(size_t)count*sizeof(*owned));
    uint32_t copied=0;
    while (copied<count && status==XR_XIR_VALUE_OK) {
        status=borrowed_clone_field(clone,fields+copied,source->type,owned+copied);
        if (status==XR_XIR_VALUE_OK) ++copied;
    }
    if (status!=XR_XIR_VALUE_OK) {
        while (copied) xr_xir_value_drop(owned+--copied);
        constructed_discard(created,(size_t)bytes);return status;
    }
    if (tuple) {((XirTuple *)created)->count=count;((XirTuple *)created)->fields=owned;}
    else {((XirNominalValue *)created)->count=count;((XirNominalValue *)created)->fields=owned;
        ((XirNominalValue *)created)->variant=((XirNominalValue *)source)->variant;}
    XrXirValue result={(uint32_t)source->type,0,0};memcpy(&result.payload,&created,sizeof(created));
    xr_xir_value_object_publish(created);clone->values[index]=result;return XR_XIR_VALUE_OK;
}
static XrXirValueStatus borrowed_clone_operation(const XrXirValue *value,XrXirType type,
    const XrXirViewTransfer *transfer,XrXirValue *output) {
    if (!unit_value(output) || !transfer || !transfer->source || !transfer->destination || !transfer->borrow ||
        transfer->source->values!=transfer->destination->values || !transfer->destination->authorize ||
        !admission_domain_current(transfer->source->values) ||
        !xr_xir_compile_type_arena_borrowed(transfer->source->values->arena,type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_value_argument(value,transfer->source->values->arena,type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (view_value_kind(object_pointer(value)->kind)) return borrowed_clone_leaf(value,transfer,output);
    XirObject *first[192];BorrowedClone clone={.visit={.first=first},.transfer=transfer};
    XrXirValueAdmission *admission=transfer->source->values;
    XrXirValueStatus status=borrowed_clone_inventory(&clone,value,type);
    if (status==XR_XIR_VALUE_OK) status=borrowed_clone_prepare(&clone);
    for (uint32_t i=0;i<clone.visit.count && status==XR_XIR_VALUE_OK;++i) {
        XirObject *object=clone.visit.objects[i];
        if (!view_storage_work(admission,1)) {status=XR_XIR_VALUE_LIMIT;break;}
        if (view_value_kind(object->kind)) {
            XrXirValue leaf=view_value_handle((XirView *)object);
            status=borrowed_clone_leaf(&leaf,transfer,clone.values+i);
        } else status=borrowed_clone_wrapper(&clone,i);
    }
    if (status==XR_XIR_VALUE_OK) {
        uint32_t root=0;status=borrowed_clone_index(&clone,object_pointer(value),&root);
        if (status==XR_XIR_VALUE_OK) {*output=clone.values[root];clone.values[root]=(XrXirValue){0};}
    }
    if (clone.values) {
        for (uint32_t i=clone.visit.count;i;--i) xr_xir_value_drop(clone.values+i-1u);
        xr_xir_domain_deallocate(admission->domain,clone.values,(size_t)clone.bytes);admission->scratch_bytes+=clone.bytes;
    }
    if (clone.visit.objects) {
        if (clone.visit.objects!=clone.visit.first)
            xr_xir_domain_deallocate(admission->domain,clone.visit.objects,(size_t)clone.visit.bytes);
        admission->scratch_bytes+=clone.visit.bytes;
    }
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_borrowed_value_reborrow(const XrXirValue *value,
    XrXirType type,const XrXirViewTransfer *transfer,XrXirValue *output) {
    xr_xir_value_graph_begin();XrXirValueStatus status=borrowed_clone_operation(value,type,transfer,output);
    xr_xir_value_graph_end();return status;
}
