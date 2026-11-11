/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_index_store.c - Atomic transfer of immutable syntax index owners
 */
#include "xlsp_source_index_store.h"
#include "../../xir/xxir_compile_memory.h"
#include <string.h>
typedef struct StoredIndex {
    XlspSourceIndex *owner;
    size_t uri_length,symbol_count;
} StoredIndex;
struct XlspSourceIndexStore {
    XrCompileResources *resources;
    StoredIndex *files;
    size_t count,capacity,symbol_count;
};
static XrXirStatus store_length(const char *text,size_t *out) {
    if(!text)return XR_XIR_BAD_STRUCTURE;
    for(size_t length=0;;++length) {
        if(!text[length]){*out=length;return XR_XIR_OK;}
        if(length==SIZE_MAX-1)return XR_XIR_OVERFLOW;
    }
}
static size_t store_find_file(const XlspSourceIndexStore *store,const char *uri,size_t length) {
    for(size_t i=0;i<store->count;++i) {
        if(store->files[i].uri_length==length&&!memcmp(xlsp_source_index_uri(store->files[i].owner),uri,length))return i;
    }
    return store->count;
}
XR_FUNC XrXirStatus xlsp_source_index_store_new(XrCompileResources *resources,XlspSourceIndexStore **out) {
    if(!resources||!out||*out)return XR_XIR_BAD_STRUCTURE;
    XlspSourceIndexStore *store=NULL;
    XrXirStatus status=xir_compile_resource_status(xr_compile_resources_calloc(resources,1,sizeof(*store),(void **)&store));
    if(status==XR_XIR_OK){store->resources=resources;*out=store;}return status;
}
XR_FUNC void xlsp_source_index_store_free(XlspSourceIndexStore *store) {
    if(!store)return;
    for(size_t i=0;i<store->count;++i)xlsp_source_index_free(store->files[i].owner);
    xr_compile_resources_free(store->files);xr_compile_resources_free(store);
}
XR_FUNC size_t xlsp_source_index_store_count(const XlspSourceIndexStore *store){return store?store->symbol_count:0;}
XR_FUNC XrXirStatus xlsp_source_index_store_replace(XlspSourceIndexStore *store,XlspSourceIndex **input) {
    if(!store||!input||!*input)return XR_XIR_BAD_STRUCTURE;
    size_t length=0,slot=0,symbols=0;XlspSourceIndex *owner=*input;
    const char *uri=xlsp_source_index_uri(owner);
    XrXirStatus status=store_length(uri,&length);if(status!=XR_XIR_OK)return status;
    if(!length)return XR_XIR_BAD_STRUCTURE;
    xlsp_source_index_symbols(owner,&symbols);
    slot=store_find_file(store,uri,length);
    size_t previous=slot<store->count?store->files[slot].symbol_count:0;
    if(store->symbol_count<previous||symbols>SIZE_MAX-(store->symbol_count-previous))return XR_XIR_OVERFLOW;
    if(slot==store->count&&store->count==store->capacity) {
        size_t next=store->capacity?store->capacity*2:8;
        if(next<store->capacity||next>SIZE_MAX/sizeof(*store->files))return XR_XIR_OVERFLOW;
        status=xir_compile_resource_status(xr_compile_resources_resize(store->resources,(void **)&store->files,next*sizeof(*store->files)));
        if(status!=XR_XIR_OK)return status;
        store->capacity=next;
    }
    XlspSourceIndex *retired=slot<store->count?store->files[slot].owner:NULL;
    store->files[slot]=(StoredIndex){owner,length,symbols};
    if(slot==store->count)++store->count;
    store->symbol_count=store->symbol_count-previous+symbols;*input=NULL;
    xlsp_source_index_free(retired);return XR_XIR_OK;
}
XR_FUNC XrXirStatus xlsp_source_index_store_remove(XlspSourceIndexStore *store,const char *uri) {
    if(!store||!uri)return XR_XIR_BAD_STRUCTURE;
    size_t length=0,slot=0;XrXirStatus status=store_length(uri,&length);
    if(status!=XR_XIR_OK)return status;
    slot=store_find_file(store,uri,length);if(slot==store->count)return XR_XIR_OK;
    size_t bytes=(store->count-slot-1)*sizeof(*store->files);
    StoredIndex retired=store->files[slot];memmove(store->files+slot,store->files+slot+1,bytes);
    --store->count;store->symbol_count-=retired.symbol_count;xlsp_source_index_free(retired.owner);return XR_XIR_OK;
}
static bool store_prefix_match(StoredIndex file,const char *prefix,size_t length) {
    if(file.uri_length<length)return false;
    const char *uri=xlsp_source_index_uri(file.owner);
    return !memcmp(uri,prefix,length)&&(file.uri_length==length||prefix[length-1]=='/'||uri[length]=='/');
}
XR_FUNC XrXirStatus xlsp_source_index_store_remove_prefix(XlspSourceIndexStore *store,const char *prefix) {
    if(!store||!prefix)return XR_XIR_BAD_STRUCTURE;
    size_t length=0;XrXirStatus status=store_length(prefix,&length);if(status!=XR_XIR_OK)return status;
    if(!length)return XR_XIR_BAD_STRUCTURE;
    if(!store->count)return XR_XIR_OK;
    bool *remove=NULL;status=xir_compile_resource_status(xr_compile_resources_calloc(store->resources,store->count,sizeof(*remove),(void **)&remove));
    if(status!=XR_XIR_OK)return status;
    for(size_t i=0;i<store->count;++i)remove[i]=store_prefix_match(store->files[i],prefix,length);
    size_t retained=0;
    for(size_t i=0;i<store->count;++i) {
        if(remove[i]){store->symbol_count-=store->files[i].symbol_count;xlsp_source_index_free(store->files[i].owner);}
        else store->files[retained++]=store->files[i];
    }
    store->count=retained;
    xr_compile_resources_free(remove);return status;
}
XR_FUNC XrXirStatus xlsp_source_index_store_find(XlspSourceIndexStore *store,const char *name,XlspIndexedSymbol *out) {
    if(!store||!name||!out)return XR_XIR_BAD_STRUCTURE;
    size_t length=0;XrXirStatus status=store_length(name,&length);if(status!=XR_XIR_OK)return status;
    XlspIndexedSymbol fallback={0};
    for(size_t i=0;i<store->count;++i) {
        size_t count=0;const XlspSourceIndexSymbol *items=xlsp_source_index_symbols(store->files[i].owner,&count);
        for(size_t j=0;j<count;++j) {
            size_t candidate=0;status=store_length(items[j].name,&candidate);if(status!=XR_XIR_OK)return status;
            if(candidate!=length||memcmp(items[j].name,name,length))continue;
            XlspIndexedSymbol found={xlsp_source_index_uri(store->files[i].owner),items+j,xlsp_source_index_version(store->files[i].owner)};
            if(items[j].exported){*out=found;return XR_XIR_OK;}if(!fallback.symbol)fallback=found;
        }
    }
    if(!fallback.symbol)return XR_XIR_UNRESOLVED;
    *out=fallback;
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xlsp_source_index_store_visit(const XlspSourceIndexStore *store,XlspIndexVisitor visitor,void *context) {
    if(!store||!visitor)return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;i<store->count;++i) {
        size_t count=0;const XlspSourceIndexSymbol *items=xlsp_source_index_symbols(store->files[i].owner,&count);
        for(size_t j=0;j<count;++j) {
            XlspIndexedSymbol view={xlsp_source_index_uri(store->files[i].owner),items+j,xlsp_source_index_version(store->files[i].owner)};
            XrXirStatus status=visitor(&view,context);if(status!=XR_XIR_OK)return status;
        }
    }
    return XR_XIR_OK;
}
