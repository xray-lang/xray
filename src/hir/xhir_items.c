/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * One module namespace supplies both compiler binding and duplicate roots. */
#include "xhir_items.h"
#include <string.h>
typedef struct HirItem {char *name;uint32_t module,node;int line,column;void *binding;} HirItem;
typedef struct HirSource {
    const char *path,*text;size_t length;const uint8_t *revision;
    uint32_t module,report_source;
} HirSource;
typedef union HirMemory {struct {union HirMemory *next;} link;union {long double floating;uint64_t integer;void *pointer;} alignment;} HirMemory;
struct XrHirItems {
    XrCompileResources *resources;HirMemory *memory;
    HirItem *items;uint32_t count,capacity;
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
    if(count)memcpy(copy,*array,(size_t)count*size);*array=copy;*capacity=next;return true;
}
XrCompileResourceStatus xr_hir_items_new(XrCompileResources *resources,XrHirItems **out) {
    if(!resources||!out||*out)return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    XrHirItems *h=NULL;XrCompileResourceStatus s=xr_compile_resources_calloc(resources,1,sizeof(*h),(void **)&h);
    if(s==XR_COMPILE_RESOURCE_OK){h->resources=resources;*out=h;}return s;
}
void xr_hir_items_free(XrHirItems *h) {
    if(!h)return;xr_frontend_report_free(h->report);
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
XrCompileResourceStatus xr_hir_items_reserve(XrHirItems *h,uint32_t module,const char *name,
    uint32_t node,int line,int column,uint32_t *out) {
    if(!h||!name||!*name||!out)return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    uint32_t previous=UINT32_MAX;
    for(uint32_t i=0;i<h->count;++i)if(h->items[i].module==module&&!strcmp(h->items[i].name,name)){previous=i;break;}
    XrCompileResourceStatus s=XR_COMPILE_RESOURCE_OK;
    if(!hir_grow(h,(void **)&h->items,&h->capacity,h->count,sizeof(*h->items),&s))return s;
    size_t length=strlen(name);if(length==SIZE_MAX)return XR_COMPILE_RESOURCE_OVERFLOW;
    char *copy=hir_allocate(h,length+1,1,&s);if(!copy)return s;memcpy(copy,name,length+1);
    if(previous!=UINT32_MAX) {
        if(!h->report){s=xr_frontend_report_new(h->resources,&h->report);if(s!=XR_COMPILE_RESOURCE_OK)return s;}
        uint32_t source=0;s=hir_source_report(h,module,&source);if(s!=XR_COMPILE_RESOURCE_OK)return s;
        const HirItem *old=&h->items[previous];
        XrFrontendLabel label={"first declaration is here",xr_frontend_report_location(h->report,source,old->line,old->column,name)};
        s=xr_frontend_report_add(h->report,302,"duplicate declaration name",
            xr_frontend_report_location(h->report,source,line,column,name),&label,1);
        if(s!=XR_COMPILE_RESOURCE_OK)return s;++h->errors;
    }
    *out=h->count;h->items[h->count++]=(HirItem){copy,module,node,line,column,NULL};return s;
}
XrCompileResourceStatus xr_hir_items_claim(XrHirItems *h,uint32_t module,const char *name,
    uint32_t node,int line,int column,uint32_t *out) {
    if(!h||!name||!out)return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    for(uint32_t i=0;i<h->count;++i)if(h->items[i].module==module&&h->items[i].node==node&&!h->items[i].binding&&!strcmp(h->items[i].name,name)){*out=i;return XR_COMPILE_RESOURCE_OK;}
    return xr_hir_items_reserve(h,module,name,node,line,column,out);
}
bool xr_hir_items_find(const XrHirItems *h,uint32_t module,const char *name,uint32_t *out) {
    if(!h||!name||!out)return false;uint32_t found=UINT32_MAX;
    for(uint32_t i=0;i<h->count;++i)if(h->items[i].module==module&&!strcmp(h->items[i].name,name)) {
        if(found!=UINT32_MAX)return false;found=i;
    }
    if(found==UINT32_MAX)return false;*out=found;return true;
}
bool xr_hir_items_module(const XrHirItems *h,uint32_t item,uint32_t *out) {
    if(!h||!out||item>=h->count)return false;*out=h->items[item].module;return true;
}
bool xr_hir_items_bind(XrHirItems *h,uint32_t item,void *binding) {
    if(!h||!binding||item>=h->count||h->items[item].binding)return false;
    h->items[item].binding=binding;return true;
}
bool xr_hir_items_binding_module(const XrHirItems *h,const void *binding,uint32_t *out) {
    if(!h||!binding||!out)return false;
    for(uint32_t i=0;i<h->count;++i)if(h->items[i].binding==binding){*out=h->items[i].module;return true;}
    return false;
}
void *xr_hir_items_binding(const XrHirItems *h,uint32_t item) {return h&&item<h->count?h->items[item].binding:NULL;}
bool xr_hir_items_errors(const XrHirItems *h){return h&&h->errors!=0;}
bool xr_hir_items_has_report(const XrHirItems *h){return h&&h->report!=NULL;}
XrFrontendReport *xr_hir_items_take_report(XrHirItems *h) {
    if(!h)return NULL;XrFrontendReport *r=h->report;h->report=NULL;return r;
}
