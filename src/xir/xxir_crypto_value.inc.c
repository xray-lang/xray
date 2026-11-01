/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_crypto_value.inc.c - Authenticated byte observation and transactional entropy replacement
 *
 * KEY CONCEPT:
 *   Real scoped ref authority selects one transactional destination. Private
 *   COW storage and publication are ready before the nonfailing provider runs.
 */
#include "xxir_crypto_value.h"
#include "../shared/xr_byte_compare_core.h"
#include "../os/os_random.h"
static XrXirValueStatus crypto_array(const XrXirValue *value,XrXirValueAdmission *admission,
    XirArray **output) {
    XrXirValueAdmission storage={0};
    XrXirValueStatus status=storage_admission_begin(value,admission,&storage);
    if(status!=XR_XIR_VALUE_OK)return status;
    const XrXirTypes *types=xr_xir_compile_type_arena_types(storage.arena);
    if(!value || !xr_xir_type_is_array(types,(XrXirType)value->type) ||
        xr_xir_array_element(types,(XrXirType)value->type)!=XR_XIR_U8) status=XR_XIR_VALUE_BAD_ARGUMENT;
    if(status==XR_XIR_VALUE_OK) {
        XirArray *array=(XirArray *)object_pointer(value);
        if(array->element!=XR_XIR_U8 || array->stride!=1)status=XR_XIR_VALUE_BAD_ARGUMENT;
        else *output=array;
    }
    return storage_admission_end(admission,&storage,status);
}
XR_FUNC XrXirValueStatus xir_crypto_timing_equal(const XrXirValue *left,const XrXirValue *right,
    XrXirValueAdmission *admission,bool *output) {
    if(!output || !admission_domain_current(admission))return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();XirArray *a=NULL,*b=NULL;
    XrXirValueStatus status=crypto_array(left,admission,&a);
    if(status==XR_XIR_VALUE_OK)status=crypto_array(right,admission,&b);
    size_t count=status==XR_XIR_VALUE_OK?(a->length<b->length?a->length:b->length):0;
    if(status==XR_XIR_VALUE_OK && !cell_projection_work(admission,count))status=XR_XIR_VALUE_LIMIT;
    if(status==XR_XIR_VALUE_OK)
        *output=xr_crypto_core_timing_safe_equal(a->data,a->length,a->stride,b->data,b->length,b->stride);
    xr_xir_value_graph_end();return status;
}
static XrXirValueStatus crypto_fill_storage(const XrXirValue *value,const XrXirCellAuthority *authority,
    XrXirValueAdmission *admission,XrXirFaultDetail *fault,const XirCryptoCommit *commit) {
    XirCell *cell=cell_owner(value);
    if(!cell || !commit || !commit->ready || !fault || !cell_write_access(cell,authority) ||
        !admission_domain_current(admission) || cell->object.domain!=admission->domain)return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission local={0};
    XrXirValueStatus status=storage_admission_begin(value,admission,&local);
    if(status!=XR_XIR_VALUE_OK)return status;
    if(!admission_owner(&local,cell->object.arena,cell->object.domain))
        return storage_admission_end(admission,&local,XR_XIR_VALUE_BAD_ARGUMENT);
    if(cell->projection_count && !cell_projection_work(&local,cell->projection_count))
        return storage_admission_end(admission,&local,XR_XIR_VALUE_LIMIT);
    XirObject *owner=cell_root_object(cell);
    XrXirValue *root=owner->kind==XIR_OBJECT_CLASS?&cell->value:&((XirCell *)owner)->value;
    XrXirType root_type=owner->kind==XIR_OBJECT_CLASS?owner->type:
        xr_xir_cell_element(xr_xir_compile_type_arena_types(owner->arena),owner->type);
    XrXirValuePlace place={root_type,&root->payload};XrXirValuePath empty={0};
    ValuePathRoute route=cell_projection_route(cell,&empty);ValuePathSlot slot={0};
    ValuePathWrite write={.module_publication=route.module_publication};uint32_t count=0;
    XrXirClassPublication publication={0};XrXirValue replacement={0},previous_leaf={0},previous_root={0};
    status=path_route_begin(&place,&route,&local,&slot,&count,fault);
    for(uint32_t i=0;status==XR_XIR_VALUE_OK && i<count;++i)
        status=path_step(&slot,path_route_step(&route,i,local.arena),&local,&write,fault,i?NULL:route.borrow_owner);
    XrXirValueAdmission storage={0};XrXirValuePlace actual={0};XirArray *array=NULL,*prepared=NULL;
    if(status==XR_XIR_VALUE_OK) {
        XrXirValueAdmission leaf=local;leaf.arena=slot.arena;
        status=storage_place_admission(&(XrXirValuePlace){slot.type,slot.bytes},&leaf,&storage,&actual);
        status=storage_admission_end(&local,&leaf,status);
    }
    if(status==XR_XIR_VALUE_OK) {
        XrXirValue current={0};int64_t payload=0;memcpy(&payload,actual.payload,sizeof(payload));
        if(!xr_xir_value_from_payload(storage.arena,actual.type,payload,&current))status=XR_XIR_VALUE_BAD_ARGUMENT;
        else status=crypto_array(&current,&storage,&array);
        if(status==XR_XIR_VALUE_OK && !cell_projection_work(&storage,array->length))status=XR_XIR_VALUE_LIMIT;
        if(status==XR_XIR_VALUE_OK) {
            bool unique=atomic_load_explicit(&array->object.references,memory_order_acquire)==1;
            prepared=array;
            if(!unique) {
                status=array_allocate(actual.type,array->capacity,&storage,&prepared);
                if(status==XR_XIR_VALUE_OK)replacement=array_value(prepared);
            }
            if(status==XR_XIR_VALUE_OK && write.module_publication)
                status=class_publication_prepare_route(&current,1,&storage,route.borrow_owner &&
                    route.borrow_owner->kind==XIR_OBJECT_CLASS?route.borrow_owner:NULL,&publication);
        }
        status=storage_admission_end(&local,&storage,status);
    }
    XrXirValue root_after=*root;bool root_changed=false;
    if(status==XR_XIR_VALUE_OK) {
        if(replacement.type && actual.payload==&root->payload){root_after=replacement;root_changed=true;}
        if((void *)write.publish==&root->payload){root_after=write.candidate;root_changed=true;}
        if(!commit->ready(commit->context))status=XR_XIR_VALUE_BAD_ARGUMENT;
    }
    if(status==XR_XIR_VALUE_OK) {
        /* Only the nonfailing provider and commit remain. No byte prefix can
         * become visible through a shared ancestor before preparation ends. */
        xr_random_bytes(prepared->data,array->length);prepared->length=array->length;
        if(publication.operation)xr_xir_class_publication_commit(&publication);
        if(replacement.type) {
            previous_leaf=array_value(array);memcpy(actual.payload,&replacement.payload,sizeof(replacement.payload));
            replacement=(XrXirValue){0};
        }
        if(write.publish) {
            previous_root=(XrXirValue){write.candidate.type,0,0};
            memcpy(&previous_root.payload,write.publish,sizeof(previous_root.payload));
            memcpy(write.publish,&write.candidate.payload,sizeof(write.candidate.payload));write.candidate=(XrXirValue){0};
        }
        if(root_changed)*root=root_after;
    }
    xr_xir_value_drop(&previous_leaf);xr_xir_value_drop(&previous_root);
    xr_xir_value_drop(&replacement);xr_xir_value_drop(&write.candidate);xr_xir_class_publication_drop(&publication);
    return storage_admission_end(admission,&local,status);
}
XR_FUNC XrXirValueStatus xir_cell_crypto_fill(const XrXirValue *value,const XrXirCellAuthority *authority,
    XrXirValueAdmission *admission,XrXirFaultDetail *fault,const XirCryptoCommit *commit) {
    if(fault)*fault=(XrXirFaultDetail){0};
    xr_xir_value_graph_begin();XrXirValueStatus status=crypto_fill_storage(value,authority,admission,fault,commit);
    xr_xir_value_graph_end();return status;
}
