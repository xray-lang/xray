/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * type_clone_probe.c - Canonical descriptor and nominal copy code, with real span observations.
 */
#include "probe_internal.h"
#include "os/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"Type clone probe %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static TcProbe *tc_current;
XR_FUNC TcProbe *tc_probe_current(void) {return tc_current;}
XR_FUNC void tc_probe_begin(TcProbe *probe) {CHECK(probe&&!tc_current);tc_current=probe;probe->requested=true;}
XR_FUNC void tc_probe_end(TcProbe *probe) {CHECK(tc_current==probe&&!probe->active&&!probe->destination&&!probe->control);tc_current=NULL;}
XR_FUNC int tc_cancel(const void *opaque) {
    TcProbe *p=(TcProbe *)opaque;CHECK(!p->retired);++p->polls;
    if(p->resources) {XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(p->resources,&stats)==XR_COMPILE_RESOURCE_OK);}
    return p->mode==TC_CANCEL&&p->wanted&&!p->disabled;
}
XR_FUNC bool tc_probe_select(TcProbe *p,const XrXirTypes *types) {
    if(!types)return false;
    if(p->phase==TC_NODES_ZERO) {p->zero_count=types->count;p->zero_size=sizeof(*types->nodes);return p->zero_count*p->zero_size>4096;}
    if(p->phase==TC_PARAMETERS)for(uint32_t i=0;i<types->count;++i)if(types->nodes[i].parameter_count==600) {
        p->target=types->nodes[i].parameters;p->bytes=600*sizeof(*types->nodes[i].parameters);return true;
    }
    if(p->phase==TC_ARGUMENTS||p->phase==TC_FIELDS)for(uint32_t i=0;i<types->count;++i) {
        const XrXirTypeNode *n=&types->nodes[i];uint32_t count=p->phase==TC_ARGUMENTS?n->nominal.argument_count:n->nominal.field_count;
        if(count>1024) {p->target=p->phase==TC_ARGUMENTS?(const void *)n->nominal.arguments:n->nominal.fields;p->bytes=(size_t)count*sizeof(XrXirType);return true;}
    }
    if(types->nominals&&types->nominals->declarations)for(uint32_t i=0;i<types->nominals->count;++i) {
        const XrXirNominalDeclaration *d=&types->nominals->declarations[i];
        if(p->phase==TC_FIELD_ROWS&&d->field_count==300) {p->fields=d;return true;}
        if(p->phase==TC_NAME) {
            if(d->name.length==4617) {p->target=d->name.bytes;p->bytes=d->name.length;return true;}
            for(uint32_t f=0;f<d->field_count;++f)if(d->fields[f].name.length==4617) {p->target=d->fields[f].name.bytes;p->bytes=d->fields[f].name.length;return true;}
        }
    }
    if(types->interfaces)for(uint32_t i=0;i<types->interfaces->count;++i)if(types->interfaces->declarations[i].method_count==192) {
        if(p->phase==TC_METHODS_ZERO) {p->zero_count=192;p->zero_size=sizeof(XrXirInterfaceMethod);return true;}
        if(p->phase==TC_PUBLICATION)return true;
    }
    return p->target!=NULL;
}
XR_FUNC bool tc_probe_copy_begin(const void *source,size_t count,size_t size,XrCompileRequestControl *control) {
    TcProbe *p=tc_current;if(!p||!p->active||p->disabled||control!=p->control||!p->target||source!=p->target)return false;
    if(p->watch)return false;
    CHECK(count&&size<=SIZE_MAX/count&&count*size==p->bytes);p->watch=true;p->zero=false;
    if(p->mode==TC_OOM) {tc_memory_fail_next();++p->attempts;}
    return true;
}
XR_FUNC bool tc_probe_zero_begin(size_t count,size_t size,XrCompileRequestControl *control) {
    TcProbe *p=tc_current;if(!p||!p->active||p->disabled||control!=p->control||count!=p->zero_count||size!=p->zero_size||p->watch)return false;
    p->watch=true;p->zero=true;p->bytes=count*size;
    if(p->mode==TC_OOM) {tc_memory_fail_next();++p->attempts;}
    return true;
}
XR_FUNC void tc_probe_span_end(bool watch,XrXirStatus status) {
    TcProbe *p=tc_current;if(!watch)return;CHECK(p&&p->watch);
    if(p->mode==TC_OOM) {CHECK(status==XR_XIR_OUT_OF_MEMORY);p->oom_seen=true;tc_memory_reset_failure();}
    p->watch=false;p->destination=NULL;
}
XR_FUNC void *tc_probe_alloc(const XrXirCompileContext *context,size_t bytes,XrXirStatus *status,const char *caller) {
    void *memory=xir_compile_alloc(context,bytes,status);TcProbe *p=tc_current;(void)caller;
    if(p&&p->active&&p->watch&&bytes==p->bytes&&memory) {CHECK(!p->destination);p->destination=memory;memset(memory,0xa5,bytes);}
    return memory;
}
XR_FUNC void *tc_probe_calloc(const XrXirCompileContext *context,size_t count,size_t size,XrXirStatus *status,const char *caller) {
    void *memory=xir_compile_calloc(context,count,size,status);TcProbe *p=tc_current;(void)caller;
    if(p&&p->active&&count==1&&memory) {
        if(size==sizeof(XrXirTypes)) {CHECK(!p->copy);p->copy=memory;}
        if(size==sizeof(XrXirNominalTable))p->nominals=memory;
    }
    return memory;
}
XR_FUNC void tc_probe_observe(XrCompileRequestControl *control) {
    TcProbe *p=tc_current;if(!p||!p->active||p->disabled||p->control!=control||p->wanted)return;
    bool witness=false;
    if(p->watch&&p->destination&&p->bytes>4096) {
        bool prefix=true;for(size_t i=0;i<4096;++i)if(p->destination[i]!=(p->zero?0:((const unsigned char *)p->target)[i])) {prefix=false;break;}
        witness=prefix&&p->destination[4096]==0xa5&&p->destination[p->bytes-1]==0xa5;
    }
    if(p->phase==TC_FIELD_ROWS&&p->nominals&&p->nominals->declarations)for(uint32_t i=0;i<p->nominals->count;++i) {
        const XrXirNominalDeclaration *d=&p->nominals->declarations[i];
        if(d->field_count==300&&d->fields&&d->fields[63].name.bytes&&!d->fields[64].name.bytes)witness=true;
    }
    if(p->phase==TC_PUBLICATION&&p->copy&&p->copy->nodes&&p->copy->nominals&&p->copy->interfaces) {
        CHECK(p->output&&*p->output==p->before_output);witness=true;
    }
    if(!witness)return;p->wanted=true;++p->witnesses;
    if(p->mode==TC_DEADLINE) {
        CHECK(control->has_deadline);uint64_t now=xr_time_monotonic_ns();CHECK(now);
        while(now<control->deadline_ns) {xr_time_sleep_ns(1000000);now=xr_time_monotonic_ns();CHECK(now);}
    }
}
static XrXirStatus tc_actual_types(const XrXirCompileContext *,const XrXirTypes *,XrCompileRequestControl *,XrXirTypes **);
#define xr_xir_compile_types_clone tc_actual_types
#define xir_compile_alloc(c,b,s) tc_probe_alloc(c,b,s,__func__)
#define xir_compile_calloc(c,n,b,s) tc_probe_calloc(c,n,b,s,__func__)
#define xir_type_copy_zero tc_zero
#define xir_type_copy_bytes tc_bytes
#define xir_lower_copy_span tc_span
#include "xir/xxir_types.c"
#undef xir_lower_copy_span
#undef xir_type_copy_bytes
#undef xir_type_copy_zero
#undef xir_compile_calloc
#undef xir_compile_alloc
#undef xr_xir_compile_types_clone
XR_FUNC XrXirStatus xr_xir_compile_types_clone(const XrXirCompileContext *context,const XrXirTypes *source,
    XrCompileRequestControl *control,XrXirTypes **output) {
    TcProbe *p=tc_current;bool active=p&&p->requested&&!p->completed&&!p->disabled&&control&&tc_probe_select(p,source);
    if(!active)return tc_actual_types(context,source,control,output);
    CHECK(!p->active&&!p->resources);p->active=true;p->source=source;p->control=control;p->output=output;p->before_output=*output;
    CHECK(xr_compile_resources_retain(context->resources)==XR_COMPILE_RESOURCE_OK);p->resources=context->resources;p->held_initial=tc_memory_initial(context->resources);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(p->resources,&before)==XR_COMPILE_RESOURCE_OK);p->held_before=before.held_bytes;
    XrXirStatus status=tc_actual_types(context,source,control,output);++p->calls;
    CHECK(xr_compile_resources_stats(p->resources,&after)==XR_COMPILE_RESOURCE_OK);p->held_after=after.held_bytes;
    if(status!=XR_XIR_OK)CHECK(*output==p->before_output&&p->held_after==p->held_before);
    p->active=false;p->completed=true;p->source=NULL;p->copy=NULL;p->nominals=NULL;p->output=NULL;p->control=NULL;p->destination=NULL;p->watch=false;
    return status;
}
