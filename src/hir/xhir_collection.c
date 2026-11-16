/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_collection.c - One exact type and duplicate-key check before emission
 */
#include "xhir_collection.h"
#include "../xir/xxir_types.h"
#include "../xir/xxir_nominal.h"
#include "../xir/xxir_key.h"
#include "../xir/xxir_constant_key.h"
#include "../xir/xxir_compile_memory.h"
#include "../base/xhash.h"
#include "../shared/xr_utf8_core.h"
#include <string.h>
static XrXirStatus collection_poll(XrCompileRequestControl *control) {
    return control?xir_compile_resource_status(xr_compile_request_control_poll(control)):XR_XIR_OK;
}
static XrXirStatus collection_error(XrHirCollectionError *out,XrHirCollectionErrorKind kind,
    XrXirType expected,XrXirType actual,uint32_t first,uint32_t second) {
    *out=(XrHirCollectionError){kind,expected,actual,first,second};return XR_XIR_BAD_TYPE;
}
static bool collection_storable(const XrXirTypes *types,XrXirType type) {
    if(type==XR_XIR_JSON_VALUE || type==XR_XIR_BOOL || type==XR_XIR_STRING || type==XR_XIR_RUNE || type==XR_XIR_ERROR ||
        type==XR_XIR_PANIC_INFO || xr_xir_type_is_number(type))return true;
    if((uint32_t)type>=XR_XIR_TYPE_PARAMETER_BASE && (uint32_t)type<XR_XIR_TYPE_PARAMETER_LIMIT)return true;
    return xr_xir_type_node(types,type) && !xr_xir_type_is_cell(types,type);
}
static uint64_t collection_key_bits(const XrHirCollectionEntry *entry) {
    uint64_t bits=entry->constant.bits;
    if(entry->key==XR_XIR_F32) {
        uint32_t word=(uint32_t)bits;
        if(!(word&UINT32_C(0x7fffffff)))return 0;
        if((word&UINT32_C(0x7f800000))==UINT32_C(0x7f800000) && (word&UINT32_C(0x007fffff)))return UINT64_C(0x7fc00000);
    } else if(entry->key==XR_XIR_F64) {
        if(!(bits&UINT64_C(0x7fffffffffffffff)))return 0;
        if((bits&UINT64_C(0x7ff0000000000000))==UINT64_C(0x7ff0000000000000) &&
            (bits&UINT64_C(0x000fffffffffffff)))return UINT64_C(0x7ff8000000000000);
    }
    return bits;
}
static XrXirStatus collection_key_hash(const XrHirCollectionEntry *entry,XrCompileRequestControl *control,uint32_t *output) {
    uint64_t words[]={entry->key,collection_key_bits(entry)};
    uint32_t hash=xr_hash_bytes(words,sizeof(words));
    if(entry->constant.kind==XR_XIR_KEY_STRING)for(size_t offset=0;offset<entry->constant.string.length;) {
        XrXirStatus status=collection_poll(control);if(status!=XR_XIR_OK)return status;
        size_t count=entry->constant.string.length-offset;if(count>4096)count=4096;
        hash=xr_hash_bytes_update(hash,entry->constant.string.bytes+offset,count);offset+=count;
    }
    *output=hash;return XR_XIR_OK;
}
static XrXirStatus collection_key_equal(const XrHirCollectionEntry *a,const XrHirCollectionEntry *b,XrCompileRequestControl *control,bool *output) {
    if(a->key!=b->key || a->constant.kind!=b->constant.kind){*output=false;return XR_XIR_OK;}
    if(a->constant.kind!=XR_XIR_KEY_STRING){*output=collection_key_bits(a)==collection_key_bits(b);return XR_XIR_OK;}
    XrXirLiteral x=a->constant.string,y=b->constant.string;
    if(x.length!=y.length){*output=false;return XR_XIR_OK;}
    int order=0;XrXirStatus status=xir_receive_compare(control,x.bytes,y.bytes,x.length,&order);
    if(status==XR_XIR_OK)*output=!order;return status;
}
XR_FUNC XrXirStatus xr_hir_collection_entry_types(const XrXirTypes *types,XrHirCollectionKind kind,
    const XrHirCollectionEntry *input,XrHirCollection *out,XrHirCollectionError *error) {
    if(!types || (types->count&&!types->nodes) || !input || !out || !error ||
        (kind!=XR_HIR_COLLECTION_MAP&&kind!=XR_HIR_COLLECTION_ARRAY))return XR_XIR_BAD_STRUCTURE;
    bool map=kind==XR_HIR_COLLECTION_MAP;XrHirCollectionEntry entry=*input;
    XrXirType key=entry.key,value=entry.value;
    if(entry.spread) {
        const XrXirTypeNode *source=xr_xir_type_node(types,value);
        uint32_t expected=map?XR_XIR_TYPE_MAP:XR_XIR_TYPE_ARRAY;
        if(key!=XR_XIR_UNIT || !source || source->kind!=expected || entry.constant.kind!=XR_XIR_KEY_DYNAMIC)
            return collection_error(error,XR_HIR_COLLECTION_SPREAD_SOURCE,XR_XIR_UNIT,value,0,0);
        key=map?source->element:XR_XIR_UNIT;value=map?source->result:source->element;
    } else if(!map && (key!=XR_XIR_UNIT || entry.constant.kind!=XR_XIR_KEY_DYNAMIC))return XR_XIR_BAD_STRUCTURE;
    if(!collection_storable(types,value) || (map&&!collection_storable(types,key)))
        return collection_error(error,XR_HIR_COLLECTION_NONVALUE,XR_XIR_UNIT,value,0,0);
    *out=(XrHirCollection){.kind=kind,.key=key,.value=value,.count=1,.operands=map&&!entry.spread?2u:1u};
    *error=(XrHirCollectionError){0};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_collection_check(const XrXirCompileContext *compile,const XrXirTypes *types,
    const XrHirCollectionInput *input,XrCompileRequestControl *control,XrHirCollection *out,XrHirCollectionError *error) {
    if(!xir_compile_context_valid(compile) || !types || (types->count && !types->nodes) || !input || !out || !error || input->count>65536 ||
        (input->count && !input->entries) || (input->kind!=XR_HIR_COLLECTION_ARRAY && input->kind!=XR_HIR_COLLECTION_MAP))
        return XR_XIR_BAD_STRUCTURE;
    if(input->count>compile->limits.parameters)return XR_XIR_BUDGET;
    XrXirStatus status=collection_poll(control);if(status!=XR_XIR_OK)return status;
    XrHirCollection result={.kind=input->kind,.count=input->count};bool map=input->kind==XR_HIR_COLLECTION_MAP;
    bool known=false;uint32_t first_key=UINT32_MAX,first_value=UINT32_MAX,*slots=NULL,capacity=0;
    if(input->has_expected) {
        const XrXirTypeNode *node=xr_xir_type_node(types,input->expected);
        if(!node || node->kind!=(uint32_t)(map?XR_XIR_TYPE_MAP:XR_XIR_TYPE_ARRAY))
            return collection_error(error,XR_HIR_COLLECTION_CONTEXT,XR_XIR_UNIT,input->expected,0,0);
        result.key=map?node->element:XR_XIR_UNIT;result.value=map?node->result:node->element;known=true;
        if(!collection_storable(types,result.value) || (map && !collection_storable(types,result.key)))
            return collection_error(error,XR_HIR_COLLECTION_NONVALUE,XR_XIR_UNIT,result.value,0,0);
    }
    if(!input->count && !known)return collection_error(error,XR_HIR_COLLECTION_EMPTY,XR_XIR_UNIT,XR_XIR_UNIT,0,0);
    for(uint32_t n=0;n<input->count;++n) {
        status=collection_poll(control);if(status!=XR_XIR_OK)break;
        XrHirCollectionEntry entry=input->entries[n];XrXirType key=entry.key,value=entry.value;
        status=xir_constant_key_check(types,key,entry.constant,control);if(status!=XR_XIR_OK)break;
        XrHirCollection projected={0};XrHirCollectionError projected_error={0};
        status=xr_hir_collection_entry_types(types,input->kind,&entry,&projected,&projected_error);
        if(status!=XR_XIR_OK) {
            if(status==XR_XIR_BAD_TYPE){*error=projected_error;error->first=n;error->second=n;}
            break;
        }
        key=projected.key;value=projected.value;
        if(!known){result.key=key;result.value=value;known=true;}
        if(first_key==UINT32_MAX)first_key=n;
        if(first_value==UINT32_MAX)first_value=n;
        if(map && key!=result.key){status=collection_error(error,XR_HIR_COLLECTION_KEY_TYPE,result.key,key,first_key,n);break;}
        if(value!=result.value){status=collection_error(error,XR_HIR_COLLECTION_VALUE_TYPE,result.value,value,first_value,n);break;}
        result.operands+=map && !entry.spread?2u:1u;
        if(!map || entry.spread || entry.constant.kind==XR_XIR_KEY_DYNAMIC)continue;
        if(!slots) {
            capacity=4;while(capacity/2<input->count)capacity*=2;
            slots=xir_compile_calloc(compile,capacity,sizeof(*slots),&status);if(!slots)break;
        }
        uint32_t hash=0;status=collection_key_hash(&entry,control,&hash);if(status!=XR_XIR_OK)break;
        uint32_t slot=hash&(capacity-1);
        while(slots[slot]) {
            status=collection_poll(control);if(status!=XR_XIR_OK)break;
            uint32_t first=slots[slot]-1;
            bool equal=false;status=collection_key_equal(&input->entries[first],&entry,control,&equal);if(status!=XR_XIR_OK)break;
            if(equal) {
                status=collection_error(error,XR_HIR_COLLECTION_DUPLICATE,result.key,result.key,first,n);break;
            }
            slot=(slot+1)&(capacity-1);
        }
        if(status!=XR_XIR_OK)break;
        slots[slot]=n+1;
    }
    xr_compile_resources_free(slots);
    if(status==XR_XIR_OK)status=collection_poll(control);
    if(status==XR_XIR_OK){*out=result;*error=(XrHirCollectionError){0};}
    return status;
}
