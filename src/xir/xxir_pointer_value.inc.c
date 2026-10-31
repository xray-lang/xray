/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_pointer_value.inc.c - Raw byte-pointer origin and readonly reception
 *
 * KEY CONCEPT:
 *   Origin records outlive their payload without retaining it. A stale pointer
 *   remains destructible, but cannot be used after the actual owner closes.
 */
static void pointer_origin_drop(XirPointerOrigin *origin) {
    if (!origin || !xr_xir_reference_release(&origin->references)) return;
    XrXirDomain *domain=origin->domain;
    xr_xir_domain_deallocate(domain,origin,sizeof(*origin));xr_xir_domain_drop(domain);
}
static void pointer_origin_close(XirPointerOrigin **slot) {
    XirPointerOrigin *origin=*slot;
    if (!origin) return;
    origin->active=false;*slot=NULL;pointer_origin_drop(origin);
}
static XrXirValueStatus pointer_origin_new(XrXirDomain *domain,XirPointerOrigin **output) {
    if (!xr_xir_domain_retain(domain)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    XirPointerOrigin *origin=xr_xir_domain_allocate(domain,sizeof(*origin),&status);
    if (!origin) {xr_xir_domain_drop(domain);return status;}
    *origin=(XirPointerOrigin){.domain=domain,.generation=1,.active=true};
    atomic_init(&origin->references,1);*output=origin;return XR_XIR_VALUE_OK;
}
static bool pointer_type(const XrXirTypeArena *arena,XrXirType type) {
    return xr_xir_type_is_pointer(xr_xir_compile_type_arena_types(arena),type);
}
static XrXirValueStatus pointer_new(XrXirDomain *domain,const XrXirTypeArena *arena,
    XrXirType type,const XirPointer *source,XrXirValue *output) {
    if (!xr_xir_domain_current(domain) || xr_xir_domain_is_compile(domain) ||
        !unit_value(output) || !pointer_type(arena,type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(domain,1)) return XR_XIR_VALUE_LIMIT;
    if (!xr_xir_domain_retain(domain)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    if (!xr_xir_compile_type_arena_retain((XrXirTypeArena *)arena)) {xr_xir_domain_drop(domain);return XR_XIR_VALUE_REFCOUNT_LIMIT;}
    if (source && source->origin && !xr_xir_reference_retain(&source->origin->references)) {
        xr_xir_compile_type_arena_drop((XrXirTypeArena *)arena);xr_xir_domain_drop(domain);return XR_XIR_VALUE_REFCOUNT_LIMIT;
    }
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    XirPointer *pointer=xr_xir_domain_allocate(domain,sizeof(*pointer),&status);
    if (!pointer) {
        if (source) pointer_origin_drop(source->origin);
        xr_xir_compile_type_arena_drop((XrXirTypeArena *)arena);xr_xir_domain_drop(domain);return status;
    }
    memset(pointer,0,sizeof(*pointer));atomic_init(&pointer->object.references,1);
    pointer->object.domain=domain;pointer->object.arena=(XrXirTypeArena *)arena;pointer->object.type=type;
    pointer->object.kind=xr_xir_type_node(xr_xir_compile_type_arena_types(arena),type)->kind;
    if (source) {pointer->origin=source->origin;pointer->generation=source->generation;pointer->address=source->address;}
    graph_object_initialize(&pointer->object);xr_xir_value_object_publish(&pointer->object);
    XrXirValue value={(uint32_t)type,0,0};memcpy(&value.payload,&pointer,sizeof(pointer));
    *output=value;return XR_XIR_VALUE_OK;
}
static bool pointer_argument(const XrXirValue *value,bool writable,const XirPointer **output) {
    if (!xr_xir_value_valid(value) || !pointer_type(xr_xir_value_arena(value),(XrXirType)value->type)) return false;
    const XirObject *object=object_pointer(value);
    if (!object || (object->kind!=XR_XIR_TYPE_PTR && object->kind!=XR_XIR_TYPE_MUT_PTR) ||
        (writable && object->kind!=XR_XIR_TYPE_MUT_PTR)) return false;
    *output=(const XirPointer *)object;return true;
}
static bool pointer_live(const XirPointer *pointer) {
    return !pointer->origin || (pointer->origin->active && pointer->generation==pointer->origin->generation &&
        pointer->origin->domain==pointer->object.domain);
}
XR_FUNC XrXirValueStatus xr_xir_pointer_null(XrXirDomain *domain,const XrXirTypeArena *arena,
    XrXirType type,XrXirValue *output) {
    xr_xir_value_graph_begin();XrXirValueStatus status=pointer_new(domain,arena,type,NULL,output);
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_pointer_readonly(const XrXirValue *source,
    const XrXirTypeArena *arena,XrXirType type,XrXirValue *output) {
    xr_xir_value_graph_begin();const XirPointer *pointer=NULL;
    const XrXirTypeNode *node=xr_xir_type_node(xr_xir_compile_type_arena_types(arena),type);
    XrXirValueStatus status=!pointer_argument(source,false,&pointer) || !node || node->kind!=XR_XIR_TYPE_PTR ?
        XR_XIR_VALUE_BAD_ARGUMENT:pointer_new(pointer->object.domain,arena,type,pointer,output);
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_pointer_offset(const XrXirValue *source,int64_t offset,XrXirValue *output) {
    xr_xir_value_graph_begin();const XirPointer *pointer=NULL;XrXirValueStatus status=XR_XIR_VALUE_BAD_ARGUMENT;
    if (pointer_argument(source,false,&pointer) && pointer_live(pointer)) {
        XirPointer derived={.origin=pointer->origin,.generation=pointer->generation,
            .address=pointer->address+(uintptr_t)offset};
        status=pointer_new(pointer->object.domain,pointer->object.arena,pointer->object.type,&derived,output);
    }
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_pointer_load(const XrXirValue *source,int64_t offset,uint8_t *output) {
    const XirPointer *pointer=NULL;
    if (!output || !pointer_argument(source,false,&pointer) || !pointer_live(pointer)) return XR_XIR_VALUE_BAD_ARGUMENT;
    *output=*(const uint8_t *)(pointer->address+(uintptr_t)offset);return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_pointer_store(const XrXirValue *target,int64_t offset,uint8_t value) {
    const XirPointer *pointer=NULL;
    if (!pointer_argument(target,true,&pointer) || !pointer_live(pointer)) return XR_XIR_VALUE_BAD_ARGUMENT;
    *(uint8_t *)(pointer->address+(uintptr_t)offset)=value;return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_pointer_copy(const XrXirValue *target,const XrXirValue *source,int64_t bytes,bool overlap) {
    const XirPointer *to=NULL,*from=NULL;
    if (bytes<0 || (uint64_t)bytes>SIZE_MAX || !pointer_argument(target,true,&to) ||
        !pointer_argument(source,false,&from) || !pointer_live(to) || !pointer_live(from) ||
        to->object.domain!=from->object.domain) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (bytes) {
        if (overlap) memmove((void *)to->address,(const void *)from->address,(size_t)bytes);
        else memcpy((void *)to->address,(const void *)from->address,(size_t)bytes);
    }
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_pointer_equal(const XrXirValue *left,const XrXirValue *right,bool *output) {
    const XirPointer *a=NULL,*b=NULL;
    if (!output || !pointer_argument(left,false,&a) || !pointer_argument(right,false,&b) ||
        a->object.domain!=b->object.domain) return XR_XIR_VALUE_BAD_ARGUMENT;
    *output=a->address==b->address;return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_buffer_storage_pointer(const XrXirValue *value,
    const XrXirTypeArena *arena,XrXirType type,XrXirValue *output) {
    xr_xir_value_graph_begin();XrXirValueStatus status=XR_XIR_VALUE_BAD_ARGUMENT;
    if (xr_xir_value_argument(value,NULL,XR_XIR_BUFFER_STORAGE) &&
        xr_xir_pointer_writable(xr_xir_compile_type_arena_types(arena),type) && unit_value(output)) {
        XirBufferStorage *storage=(XirBufferStorage *)object_pointer(value);
        XirPointerOrigin *origin=storage->origin;
        status=origin ? XR_XIR_VALUE_OK:pointer_origin_new(storage->object.domain,&origin);
        if (status==XR_XIR_VALUE_OK) {
            XirPointer source={.origin=origin,.generation=origin->generation,.address=(uintptr_t)storage->data};
            status=pointer_new(storage->object.domain,arena,type,&source,output);
            if (status==XR_XIR_VALUE_OK) storage->origin=origin;
            else if (!storage->origin) pointer_origin_drop(origin);
        }
    }
    xr_xir_value_graph_end();return status;
}
