/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * One module namespace supplies both compiler binding and duplicate roots. */
#include "xhir_items.h"
#include "../base/xchecks.h"
#include "../base/xhash.h"
#include <string.h>
typedef struct HirItem {
    char *name;
    uint32_t module,parent,node,name_next,binding_next;
    int line,column;
    XrHirItemKind kind;
    void *binding;
} HirItem;
typedef struct HirSource {
    const char *path,*text;size_t length;const uint8_t *revision;
    uint32_t module,report_source;
} HirSource;
typedef union HirMemory {struct {union HirMemory *next;} link;union {long double floating;uint64_t integer;void *pointer;} alignment;} HirMemory;
struct XrHirItems {
    XrCompileResources *resources;HirMemory *memory;
    HirItem *items;uint32_t count,capacity;
    uint32_t *name_index,*binding_index,index_capacity;
    HirSource *sources;uint32_t source_count,source_capacity;
    XrFrontendReport *report;uint32_t errors;
};
static void *hir_allocate(XrHirItems *h,size_t count,size_t size,XrCompileResourceStatus *status) {
    if(!size || count>(SIZE_MAX-sizeof(HirMemory))/size){*status=XR_COMPILE_RESOURCE_OVERFLOW;return NULL;}
    HirMemory *m=NULL;*status=xr_compile_resources_calloc(h->resources,1,sizeof(*m)+count*size,(void **)&m);
    if(*status!=XR_COMPILE_RESOURCE_OK)return NULL;
    m->link.next=h->memory;h->memory=m;return m+1;
}
static bool hir_grow(XrHirItems *h,void **array,uint32_t *capacity,uint32_t count,
    size_t size,XrCompileResourceStatus *status) {
    if(count<*capacity)return true;
    if(count==UINT32_MAX){*status=XR_COMPILE_RESOURCE_OVERFLOW;return false;}
    uint32_t next=*capacity?(*capacity>UINT32_MAX/2?UINT32_MAX:*capacity*2):8;
    void *copy=hir_allocate(h,next,size,status);if(!copy)return false;
    if (count)
        memcpy(copy, *array, (size_t) count * size);
    *array = copy;
    *capacity = next;
    return true;
}
static uint32_t hir_name_hash(uint32_t module,uint32_t parent,const char *name) {
    uint32_t key[] = {module,parent};
    return xr_hash_bytes_update(xr_hash_bytes(key,sizeof(key)),name,strlen(name));
}
static uint32_t hir_name_head(const XrHirItems *h,uint32_t module,uint32_t parent,const char *name) {
    return h->index_capacity ? h->name_index[hir_name_hash(module,parent,name)%h->index_capacity] : 0;
}
static void hir_index_item(XrHirItems *h,uint32_t index,uint32_t *names,
    uint32_t *bindings,uint32_t capacity) {
    HirItem *item = &h->items[index];
    uint32_t bucket = hir_name_hash(item->module,item->parent,item->name)%capacity;
    item->name_next = names[bucket];
    names[bucket] = index+1;
    if(item->kind==XR_HIR_ITEM_DECLARATION && !item->parent && item->binding) {
        bucket = xr_hash_bytes(&item->binding,sizeof(item->binding))%capacity;
        item->binding_next = bindings[bucket];
        bindings[bucket] = index+1;
    }
}
static XrCompileResourceStatus hir_indices_reserve(XrHirItems *h) {
    if(h->count<h->index_capacity || h->index_capacity==UINT32_MAX)
        return XR_COMPILE_RESOURCE_OK;
    uint32_t capacity = h->index_capacity ?
        (h->index_capacity>UINT32_MAX/2 ? UINT32_MAX : h->index_capacity*2) : 8;
    XrCompileResourceStatus status = XR_COMPILE_RESOURCE_OK;
    uint32_t *names = hir_allocate(h,capacity,2*sizeof(*names),&status);
    if(!names)return status;
    uint32_t *bindings = names+capacity;
    /* No fallible operation follows allocation, so links and heads publish together. */
    for(uint32_t i=0;i<h->count;++i)
        hir_index_item(h,i,names,bindings,capacity);
    h->name_index = names;
    h->binding_index = bindings;
    h->index_capacity = capacity;
    return XR_COMPILE_RESOURCE_OK;
}
XrCompileResourceStatus xr_hir_items_new(XrCompileResources *resources,XrHirItems **out) {
    if(!resources||!out||*out)return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    XrHirItems *h=NULL;XrCompileResourceStatus s=xr_compile_resources_calloc(resources,1,sizeof(*h),(void **)&h);
    if(s==XR_COMPILE_RESOURCE_OK){h->resources=resources;*out=h;}return s;
}
void xr_hir_items_free(XrHirItems *h) {
    if (!h)
        return;
    xr_frontend_report_free(h->report);
    while(h->memory){HirMemory *m=h->memory;h->memory=m->link.next;xr_compile_resources_free(m);}
    xr_compile_resources_free(h);
}
XrCompileResourceStatus xr_hir_items_source(XrHirItems *h,uint32_t module,const char *path,
    const char *text,size_t length,const uint8_t *revision) {
    if(!h||(!text&&length))return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    for(uint32_t i=0;i<h->source_count;++i)if(h->sources[i].module==module)return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    XrCompileResourceStatus s=XR_COMPILE_RESOURCE_OK;
    if(!hir_grow(h,(void **)&h->sources,&h->source_capacity,h->source_count,sizeof(*h->sources),&s))return s;
    h->sources[h->source_count++]=(HirSource){path,text,length,revision,module,UINT32_MAX};return s;
}
static XrCompileResourceStatus hir_source_report(XrHirItems *h,uint32_t module,uint32_t *out) {
    for(uint32_t i=0;i<h->source_count;++i) {
        HirSource *s=&h->sources[i];if(s->module!=module)continue;
        if(s->report_source==UINT32_MAX) {
            XrCompileResourceStatus status=xr_frontend_report_source(h->report,s->path,s->text,s->length,s->revision,&s->report_source);
            if(status!=XR_COMPILE_RESOURCE_OK)return status;
        }
        *out=s->report_source;return XR_COMPILE_RESOURCE_OK;
    }
    return xr_frontend_report_source(h->report,NULL,NULL,0,NULL,out);
}
static bool hir_item_spec_valid(const XrHirItems *h,const XrHirItemSpec *spec) {
    return h&&spec&&spec->name&&*spec->name&&spec->parent<=h->count&&
        (!spec->parent||h->items[spec->parent-1].module==spec->module)&&
        (spec->kind==XR_HIR_ITEM_DECLARATION||
         (spec->kind==XR_HIR_ITEM_PARAMETER&&spec->parent)||
         (spec->kind==XR_HIR_ITEM_PARAMETER_SCOPE&&spec->node)||
         (spec->kind==XR_HIR_ITEM_METHOD&&spec->parent&&spec->node));
}
XrCompileResourceStatus xr_hir_items_reserve(XrHirItems *h,const XrHirItemSpec *spec,uint32_t *out) {
    if(!out||!hir_item_spec_valid(h,spec))return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    uint32_t previous=UINT32_MAX;
    for(uint32_t link=hir_name_head(h,spec->module,spec->parent,spec->name);
        spec->kind!=XR_HIR_ITEM_PARAMETER_SCOPE&&link;link=h->items[link-1].name_next) {
        uint32_t i=link-1;
        if(h->items[i].module==spec->module&&
        h->items[i].kind!=XR_HIR_ITEM_PARAMETER_SCOPE&&
        h->items[i].parent==spec->parent&&!strcmp(h->items[i].name,spec->name)&&i<previous)previous=i;
    }
    XrCompileResourceStatus s=XR_COMPILE_RESOURCE_OK;
    if(!hir_grow(h,(void **)&h->items,&h->capacity,h->count,sizeof(*h->items),&s))return s;
    s=hir_indices_reserve(h);if(s!=XR_COMPILE_RESOURCE_OK)return s;
    size_t length=strlen(spec->name);if(length==SIZE_MAX)return XR_COMPILE_RESOURCE_OVERFLOW;
    char *copy=hir_allocate(h,length+1,1,&s);if(!copy)return s;memcpy(copy,spec->name,length+1);
    if(previous!=UINT32_MAX) {
        if(!h->report){s=xr_frontend_report_new(h->resources,&h->report);if(s!=XR_COMPILE_RESOURCE_OK)return s;}
        uint32_t source=0;s=hir_source_report(h,spec->module,&source);if(s!=XR_COMPILE_RESOURCE_OK)return s;
        const HirItem *old=&h->items[previous];
        XrFrontendLabel label={"first declaration is here",xr_frontend_report_location(h->report,source,old->line,old->column,spec->name)};
        s=xr_frontend_report_add(h->report,302,spec->kind==XR_HIR_ITEM_PARAMETER?
            "duplicate parameter name":"duplicate declaration name",
            xr_frontend_report_location(h->report,source,spec->line,spec->column,spec->name),&label,1);
        if (s != XR_COMPILE_RESOURCE_OK)
            return s;
        ++h->errors;
    }
    *out=h->count;
    h->items[h->count]=(HirItem){.name=copy,.module=spec->module,.parent=spec->parent,
        .node=spec->node,.line=spec->line,.column=spec->column,.kind=spec->kind};
    hir_index_item(h,h->count,h->name_index,h->binding_index,h->index_capacity);
    ++h->count;
    return s;
}
XrCompileResourceStatus xr_hir_items_claim(XrHirItems *h,const XrHirItemSpec *spec,uint32_t *out) {
    if(!out||!hir_item_spec_valid(h,spec))return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    uint32_t found=UINT32_MAX;
    for(uint32_t link=hir_name_head(h,spec->module,spec->parent,spec->name);link;link=h->items[link-1].name_next) {
        uint32_t i=link-1;
        if(h->items[i].module==spec->module&&
        h->items[i].parent==spec->parent&&h->items[i].node==spec->node&&
        h->items[i].kind==spec->kind&&!h->items[i].binding&&!strcmp(h->items[i].name,spec->name)&&i<found)found=i;
    }
    if(found!=UINT32_MAX){*out=found;return XR_COMPILE_RESOURCE_OK;}
    if(spec->parent)return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    return xr_hir_items_reserve(h,spec,out);
}
bool xr_hir_items_find(const XrHirItems *h,uint32_t module,uint32_t parent,const char *name,uint32_t *out) {
    if (!h || !name || !out)
        return false;
    uint32_t found = UINT32_MAX;
    for(uint32_t link=hir_name_head(h,module,parent,name);link;link=h->items[link-1].name_next) {
        uint32_t i=link-1;
        if(h->items[i].kind!=XR_HIR_ITEM_PARAMETER_SCOPE&&
        h->items[i].module==module&&h->items[i].parent==parent&&!strcmp(h->items[i].name,name)) {
            if (found != UINT32_MAX)
                return false;
            found = i;
        }
    }
    if (found == UINT32_MAX)
        return false;
    *out = found;
    return true;
}
bool xr_hir_items_find_scope(const XrHirItems *h,uint32_t module,uint32_t node,uint32_t *out) {
    if (!h || !node || !out)
        return false;
    uint32_t found = UINT32_MAX;
    for(uint32_t i=0;i<h->count;++i)if((h->items[i].kind==XR_HIR_ITEM_PARAMETER_SCOPE||
        h->items[i].kind==XR_HIR_ITEM_METHOD)&&h->items[i].module==module&&h->items[i].node==node) {
            if (found != UINT32_MAX)
                return false;
            found = i;
    }
    if (found == UINT32_MAX)
        return false;
    *out = found;
    return true;
}
bool xr_hir_items_module(const XrHirItems *h,uint32_t item,uint32_t *out) {
    if (!h || !out || item >= h->count)
        return false;
    *out = h->items[item].module;
    return true;
}
bool xr_hir_items_bind(XrHirItems *h,uint32_t item,void *binding) {
    if(!h||!binding||item>=h->count||h->items[item].binding)return false;
    h->items[item].binding=binding;
    if(h->items[item].kind==XR_HIR_ITEM_DECLARATION&&!h->items[item].parent) {
        XR_DCHECK(h->index_capacity,"reserved items have binding index capacity");
        uint32_t bucket=xr_hash_bytes(&binding,sizeof(binding))%h->index_capacity;
        h->items[item].binding_next=h->binding_index[bucket];
        h->binding_index[bucket]=item+1;
    }
    return true;
}
bool xr_hir_items_binding_module(const XrHirItems *h,const void *binding,uint32_t *out) {
    if(!h||!binding||!out||!h->index_capacity)return false;
    uint32_t found=UINT32_MAX;
    uint32_t bucket=xr_hash_bytes(&binding,sizeof(binding))%h->index_capacity;
    for(uint32_t link=h->binding_index[bucket];link;link=h->items[link-1].binding_next)
        if(h->items[link-1].binding==binding && link-1<found)found=link-1;
    if(found==UINT32_MAX)return false;
    *out=h->items[found].module;
    return true;
}
void *xr_hir_items_binding(const XrHirItems *h,uint32_t item) {return h&&item<h->count?h->items[item].binding:NULL;}
bool xr_hir_items_errors(const XrHirItems *h){return h&&h->errors!=0;}
bool xr_hir_items_has_report(const XrHirItems *h){return h&&h->report!=NULL;}
XrFrontendReport *xr_hir_items_take_report(XrHirItems *h) {
    if (!h)
        return NULL;
    XrFrontendReport *r = h->report;
    h->report = NULL;
    return r;
}
