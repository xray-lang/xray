/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * nominal_projection_probe.c - Observe canonical type projection without hooks
 */
#include "nominal_projection_probe.h"
#include "xir/xxir_compile_memory.h"
#include "xir/xxir_lower_control.h"
#include "os/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NP_CHECK(c) do {if(!(c)){fprintf(stderr,"Nominal projection %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static NominalProjectionProbe *np_probe;
XR_FUNC void test_nominal_projection_begin(NominalProjectionProbe *probe) {
    NP_CHECK(probe && !np_probe);np_probe=probe;
}
XR_FUNC void test_nominal_projection_end(NominalProjectionProbe *probe) {
    NP_CHECK(probe==np_probe && !probe->table && !probe->identities && !probe->control && !probe->destination);
    np_probe=NULL;
}
static bool np_literal_equal(XrXirLiteral a,XrXirLiteral b) {
    return a.bytes && a.bytes!=b.bytes && a.length==b.length && !memcmp(a.bytes,b.bytes,a.length);
}
static bool np_complete(void) {
    NominalProjectionProbe *p=np_probe;
    if(!p || !p->table || !p->identities)return false;
    for(uint32_t i=0;i<p->table->count;++i) {
        const XrXirNominalDeclaration *in=&p->table->declarations[i];
        const XrXirNominalIdentity *out=&p->identities[i];
        if(!np_literal_equal(out->module,in->module) || !np_literal_equal(out->name,in->name) ||
            out->field_count!=in->field_count || out->variant_count!=in->variant_count ||
            (out->field_count && !out->fields) || (out->variant_count && !out->variants))return false;
        for(uint32_t v=0;v<out->variant_count;++v)
            if(!np_literal_equal(out->variants[v].name,in->variants[v].name))return false;
        for(uint32_t f=0;f<out->field_count;++f)
            if(!np_literal_equal(out->fields[f].name,in->fields[f].name))return false;
    }
    return true;
}
static XrCompileResourceStatus np_poll(XrCompileRequestControl *control) {
    if(np_probe && np_probe->control==control && np_probe->phase==NP_PUBLICATION && np_complete()) {
        np_probe->wanted=true;
        if(np_probe->mode==1) {
            NP_CHECK(control->has_deadline);np_probe->witnesses=1;
            uint64_t now=xr_time_monotonic_ns();NP_CHECK(now);
            while(now<control->deadline_ns){xr_time_sleep_ns(1000000);now=xr_time_monotonic_ns();NP_CHECK(now);}
        }
    }
    return xr_compile_request_control_poll(control);
}
static void *np_scan(const void *source,int value,size_t length) {
    void *result=memchr(source,value,length);NominalProjectionProbe *p=np_probe;
    if(p && p->table && !p->copying) {
        if(source==p->long_name && length==4096){p->scanned=4096;if(p->phase==NP_LONG_SCAN)p->wanted=true;}
        if(source==p->long_name+4096)p->tail_scanned+=(unsigned)length;
        if(source==p->last_field && p->phase==NP_FIELD_SCAN)p->wanted=true;
        if(source==p->variant_name && p->phase==NP_VARIANT_SCAN)p->wanted=true;
    }
    return result;
}
static void *np_alloc(const XrXirCompileContext *context,size_t bytes,XrXirStatus *status,const char *caller) {
    NominalProjectionProbe *p=np_probe;
    bool fail=p && p->table && p->mode==2 && p->phase==NP_LONG_COPY &&
        !p->oom_seen && bytes==4618 && !strcmp(caller,"nominal_copy_name");
    if(fail)p->fail_next();
    void *memory=xir_compile_alloc(context,bytes,status);
    if(fail){NP_CHECK(!memory && *status==XR_XIR_OUT_OF_MEMORY);p->oom_seen=true;p->wanted=true;}
    return memory;
}
static void *np_calloc(const XrXirCompileContext *context,size_t count,size_t bytes,XrXirStatus *status) {
    void *memory=xir_compile_calloc(context,count,bytes,status);
    if(np_probe && np_probe->table && count==np_probe->table->count && bytes==sizeof(XrXirNominalIdentity)) {
        NP_CHECK(!np_probe->identities);np_probe->identities=memory;
    }
    return memory;
}
static XrCompileResourceStatus np_span(XrCompileRequestControl *control,void *destination,const void *source,size_t bytes) {
    NominalProjectionProbe *p=np_probe;
    bool observe=p && p->table && source==p->long_name;
    if(observe){NP_CHECK(bytes==4617 && !p->destination);p->destination=destination;memset(destination,0xa5,bytes);p->copying=true;}
    if(p && p->table && source==p->variant_name && p->phase==NP_VARIANT_COPY)p->wanted=true;
    XrCompileResourceStatus status=xir_lower_copy_span(control,destination,source,bytes);
    if(observe){p->destination=NULL;p->copying=false;}
    return status;
}
static XrXirStatus np_project_actual(const XrXirCompileContext *,const XrXirNominalTable *,XrCompileRequestControl *,XrXirNominalTable **);
#define xr_xir_compile_nominal_project np_project_actual
#define xir_compile_alloc(c,b,s) np_alloc(c,b,s,__func__)
#define xir_compile_calloc np_calloc
#define xir_lower_copy_span np_span
#define xr_compile_request_control_poll np_poll
#define memchr np_scan
#include "xir/xxir_types.c"
#undef memchr
#undef xr_compile_request_control_poll
#undef xir_lower_copy_span
#undef xir_compile_calloc
#undef xir_compile_alloc
#undef xr_xir_compile_nominal_project
XR_FUNC XrXirStatus xr_xir_compile_nominal_project(const XrXirCompileContext *context,
    const XrXirNominalTable *table,XrCompileRequestControl *control,XrXirNominalTable **output) {
    NominalProjectionProbe *p=np_probe;const XrXirNominalDeclaration *wide=NULL;
    if(p)for(uint32_t i=0;i<table->count;++i) {
        const XrXirNominalDeclaration *d=&table->declarations[i];
        if(d->name.length==10 && !memcmp(d->name.bytes,"WideRecord",10)){NP_CHECK(!wide);wide=d;}
    }
    if(!wide)return np_project_actual(context,table,control,output);
    NP_CHECK(control && wide->field_count==300 && wide->fields[0].name.length==4617 && wide->variant_count==1);
    NP_CHECK(!p->table && !p->control && !p->identities && !p->resources);
    p->table=table;p->control=control;p->long_name=wide->fields[0].name.bytes;
    p->last_field=wide->fields[299].name.bytes;p->variant_name=wide->variants[0].name.bytes;
    p->capture(p,context->resources);
    XrCompileResourceStats before={0};NP_CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    p->held_before=before.held_bytes;
    XrXirStatus status=np_project_actual(context,table,control,output);
    XrCompileResourceStats after={0};NP_CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    p->held_after=after.held_bytes;
    if(status!=XR_XIR_OK){NP_CHECK(!*output && p->held_before==p->held_after);}
    p->table=NULL;p->control=NULL;p->identities=NULL;p->long_name=NULL;p->last_field=NULL;p->variant_name=NULL;
    return status;
}
