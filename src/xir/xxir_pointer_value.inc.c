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
#include "../shared/xr_sync_core.h"
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
XR_FUNC XrXirValueStatus xir_pointer_origin_create(XrXirDomain *domain,XirPointerOrigin **output) {
    if(!domain||!output||*output||!xr_xir_domain_current(domain)||xr_xir_domain_is_compile(domain))return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();XrXirValueStatus status=pointer_origin_new(domain,output);
    xr_xir_value_graph_end();return status;
}
XR_FUNC void xir_pointer_origin_invalidate(XirPointerOrigin **owner) {
    if(!owner)return;
    xr_xir_value_graph_begin();pointer_origin_close(owner);xr_xir_value_graph_end();
}
XR_FUNC XrXirValueStatus xir_pointer_origin_readonly(XirPointerOrigin *origin,
    const XrXirTypeArena *arena,XrXirType type,uintptr_t address,XrXirValue *output) {
    xr_xir_value_graph_begin();XrXirValueStatus status=XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypes *types=xr_xir_compile_type_arena_types(arena);
    if(origin&&origin->active&&!origin->page&&!origin->exclusive&&pointer_type(arena,type)&&
        !xr_xir_pointer_writable(types,type)) {
        XirPointer source={.origin=origin,.generation=origin->generation,.address=address};
        status=pointer_new(origin->domain,arena,type,&source,output);
    }
    xr_xir_value_graph_end();return status;
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
        pointer->origin->domain==pointer->object.domain && !pointer->origin->exclusive);
}
static bool pointer_write_live(const XirPointer *pointer) {
    return pointer_live(pointer) && (!pointer->origin || !pointer->origin->shared_views);
}
XR_FUNC bool xr_xir_pointer_buffer_range(const XrXirValue *source,
    const XrXirValue *owner,int64_t bytes,uint64_t *offset) {
    const XirPointer *pointer=NULL;
    if (!offset || bytes<0 || !pointer_argument(source,true,&pointer) || !pointer_live(pointer) ||
        !xr_xir_value_argument(owner,NULL,XR_XIR_BUFFER_STORAGE)) return false;
    const XirBufferStorage *storage=(const XirBufferStorage *)object_pointer(owner);
    const XirPointerOrigin *origin=storage->origin;
    if (!origin || pointer->origin!=origin || !origin->active || origin->page ||
        origin->domain!=storage->object.domain || pointer->object.domain!=storage->object.domain ||
        pointer->generation!=origin->generation) return false;
    uintptr_t base=(uintptr_t)storage->data;
    if (pointer->address<base) return false;
    uint64_t displacement=(uint64_t)(pointer->address-base);
    if (displacement>storage->length || (uint64_t)bytes>storage->length-displacement) return false;
    *offset=displacement;return true;
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
    if (!pointer_argument(target,true,&pointer) || !pointer_write_live(pointer)) return XR_XIR_VALUE_BAD_ARGUMENT;
    *(uint8_t *)(pointer->address+(uintptr_t)offset)=value;return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_pointer_copy(const XrXirValue *target,const XrXirValue *source,int64_t bytes,bool overlap) {
    const XirPointer *to=NULL,*from=NULL;
    if (bytes<0 || (uint64_t)bytes>SIZE_MAX || !pointer_argument(target,true,&to) ||
        !pointer_argument(source,false,&from) || !pointer_write_live(to) || !pointer_live(from) ||
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
XR_FUNC XrXirValueStatus xr_xir_pointer_prefetch(const XrXirValue *source,int64_t write) {
    const XirPointer *pointer=NULL;
    if (!pointer_argument(source,false,&pointer) || !pointer_live(pointer) ||
        (write && !pointer_write_live(pointer))) return XR_XIR_VALUE_BAD_ARGUMENT;
#if defined(__GNUC__) || defined(__clang__)
    if (write) __builtin_prefetch((const void *)pointer->address,1,3);
    else __builtin_prefetch((const void *)pointer->address,0,3);
#else
    (void)write;
#endif
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_pointer_cache_flush(const XrXirValue *source,int64_t bytes) {
    const XirPointer *pointer=NULL;
    if (bytes<0 || (uint64_t)bytes>SIZE_MAX || !pointer_argument(source,false,&pointer) || !pointer_live(pointer))
        return XR_XIR_VALUE_BAD_ARGUMENT;
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    uintptr_t start=pointer->address&~(uintptr_t)63u,end=pointer->address+(uintptr_t)bytes;
    if (end<pointer->address) end=UINTPTR_MAX;
    for (uintptr_t at=start;bytes && at<end;at+=64u) {
        __asm__ __volatile__("clflush (%0)" : : "r"((const void *)at) : "memory");
        if (UINTPTR_MAX-at<64u) break;
    }
#endif
    if (bytes) xr_sync_core_fence(4);
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_pointer_volatile_load(const XrXirValue *source,int64_t size,int64_t *output) {
    const XirPointer *pointer=NULL;uint64_t value=0;
    if (!output || !pointer_argument(source,false,&pointer) || !pointer_live(pointer)) return XR_XIR_VALUE_BAD_ARGUMENT;
    switch(size) {
    case 1:value=*(const volatile uint8_t *)pointer->address;break;
    case 2:value=*(const volatile uint16_t *)pointer->address;break;
    case 4:value=*(const volatile uint32_t *)pointer->address;break;
    case 8:value=*(const volatile uint64_t *)pointer->address;break;
    default:return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    memcpy(output,&value,sizeof(value));return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_pointer_volatile_store(const XrXirValue *target,int64_t value,int64_t size,bool streaming) {
    const XirPointer *pointer=NULL;
    if (!pointer_argument(target,true,&pointer) || !pointer_write_live(pointer)) return XR_XIR_VALUE_BAD_ARGUMENT;
    bool streamed=false;
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    if (streaming && size==4 && !(pointer->address&3u)) {
        __asm__ __volatile__("movnti %1, %0" : "=m"(*(uint32_t *)pointer->address) : "r"((uint32_t)value) : "memory");
        streamed=true;
    }
#if defined(__x86_64__)
    else if (streaming && size==8 && !(pointer->address&7u)) {
        __asm__ __volatile__("movnti %1, %0" : "=m"(*(uint64_t *)pointer->address) : "r"((uint64_t)value) : "memory");
        streamed=true;
    }
#endif
#endif
    if (!streamed) switch(size) {
    case 1:*(volatile uint8_t *)pointer->address=(uint8_t)value;break;
    case 2:*(volatile uint16_t *)pointer->address=(uint16_t)value;break;
    case 4:*(volatile uint32_t *)pointer->address=(uint32_t)value;break;
    case 8:*(volatile uint64_t *)pointer->address=(uint64_t)value;break;
    default:return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    if (streaming) xr_sync_core_fence(4);
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_buffer_storage_pointer(const XrXirValue *value,
    const XrXirTypeArena *arena,XrXirType type,XrXirValue *output) {
    xr_xir_value_graph_begin();XrXirValueStatus status=XR_XIR_VALUE_BAD_ARGUMENT;
    if (xr_xir_value_argument(value,NULL,XR_XIR_BUFFER_STORAGE) &&
        xr_xir_pointer_writable(xr_xir_compile_type_arena_types(arena),type) && unit_value(output)) {
        XirBufferStorage *storage=(XirBufferStorage *)object_pointer(value);
        if (storage->origin && storage->origin->exclusive) {
            xr_xir_value_graph_end();return XR_XIR_VALUE_BAD_ARGUMENT;
        }
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
