/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * declarations_copy_probe.c - Observe the canonical declaration copy TU
 */
#include "declarations_copy_probe.h"
#include "xir/xxir_compile_memory.h"
#include "xir/xxir_lower_control.h"
#include "xir/xxir_implementation.h"
#include "os/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DC_CHECK(c) do {if(!(c)){fprintf(stderr,"Declaration receiver %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static DeclarationsCopyProbe *dc_probe;
XR_FUNC void test_declarations_copy_begin(DeclarationsCopyProbe *probe) {DC_CHECK(probe&&!dc_probe);dc_probe=probe;}
XR_FUNC void test_declarations_copy_end(DeclarationsCopyProbe *probe) {
    DC_CHECK(probe==dc_probe&&!probe->source&&!probe->copy&&!probe->destination&&!probe->control);
    DC_CHECK(!probe->implementation_source&&!probe->implementation_copy&&!probe->argument_source);dc_probe=NULL;
}
#include "implementation_copy_probe.inc.c"
static XrCompileResourceStatus dc_poll(XrCompileRequestControl *control) {
    DeclarationsCopyProbe *p=dc_probe;
    if(p && p->control==control && p->phase==DC_PUBLICATION && p->implementations_returned) {
        DC_CHECK(p->copy && p->copy->functions && p->copy->functions!=p->source->functions);
        DC_CHECK(!memcmp(p->copy->functions,p->source->functions,p->bytes));p->wanted=true;
        if(p->mode==1) {
            DC_CHECK(control->has_deadline);p->witnesses=1;uint64_t now=xr_time_monotonic_ns();DC_CHECK(now);
            while(now<control->deadline_ns){xr_time_sleep_ns(1000000);now=xr_time_monotonic_ns();DC_CHECK(now);}
        }
    }
    return xr_compile_request_control_poll(control);
}
static void *dc_alloc(const XrXirCompileContext *context,size_t bytes,XrXirStatus *status,const char *caller) {
    DeclarationsCopyProbe *p=dc_probe;bool fail=p&&!p->implementation&&p->source&&p->mode==2&&!p->oom_seen&&
        bytes==p->bytes&&!strcmp(caller,"declaration_copy");
    if(fail)p->fail_next();void *memory=xir_compile_alloc(context,bytes,status);
    if(fail){DC_CHECK(!memory&&*status==XR_XIR_OUT_OF_MEMORY);p->oom_seen=true;p->wanted=true;}
    return memory;
}
static void *dc_calloc(const XrXirCompileContext *context,size_t count,size_t bytes,XrXirStatus *status) {
    void *memory=xir_compile_calloc(context,count,bytes,status);
    if(dc_probe&&dc_probe->source&&count==1&&bytes==sizeof(XrXirDeclarations)) {
        DC_CHECK(!dc_probe->copy);dc_probe->copy=memory;
    }
    return memory;
}
static XrCompileResourceStatus dc_span(XrCompileRequestControl *control,void *destination,const void *source,size_t bytes) {
    DeclarationsCopyProbe *p=dc_probe;bool functions=p&&p->source&&source==p->source->functions;
    if(functions&&!p->implementation){DC_CHECK(bytes==24240&&!p->destination);p->destination=destination;memset(destination,0xa5,bytes);++p->copies;}
    if(p&&p->source&&p->phase==DC_MODULE_NAME&&source==p->source->modules[0].name)p->wanted=true;
    XrCompileResourceStatus status=xir_lower_copy_span(control,destination,source,bytes);
    if(functions&&!p->implementation)p->destination=NULL;return status;
}
static XrXirStatus dc_implementations(const XrXirCompileContext *context,const XrXirImplementationTable *source,XrCompileRequestControl *control,XrXirImplementationTable **output) {
    XrXirStatus status=dc_probe&&dc_probe->implementation&&dc_probe->source?
        test_implementation_copy(context,source,control,output):xr_xir_compile_implementations_copy_verified(context,source,control,output);
    if(dc_probe&&dc_probe->source)dc_probe->implementations_returned=true;
    return status;
}
static XrXirStatus dc_actual(const XrXirCompileContext *,const XrXirDeclarations *,uint32_t,XrCompileRequestControl *,XrXirDeclarations **);
#define xr_xir_compile_declarations_clone dc_actual
#define xir_compile_alloc(c,b,s) dc_alloc(c,b,s,__func__)
#define xir_compile_calloc dc_calloc
#define xir_lower_copy_span dc_span
#define xr_compile_request_control_poll dc_poll
#define xr_xir_compile_implementations_copy_verified dc_implementations
#include "xir/xxir_declarations.c"
#undef xr_xir_compile_implementations_copy_verified
#undef xr_compile_request_control_poll
#undef xir_lower_copy_span
#undef xir_compile_calloc
#undef xir_compile_alloc
#undef xr_xir_compile_declarations_clone
XR_FUNC XrXirStatus xr_xir_compile_declarations_clone(const XrXirCompileContext *context,
    const XrXirDeclarations *source,uint32_t functions,XrCompileRequestControl *control,XrXirDeclarations **output) {
    DeclarationsCopyProbe *p=dc_probe;
    if(!p||!source||!control||(!p->implementation&&functions!=606))return dc_actual(context,source,functions,control,output);
    if(p->implementation) {
        bool found=false;if(source->implementations)for(uint32_t i=0;i<source->implementations->count;++i)
            if(source->implementations->records[i].binding_count==192)found=true;
        if(!found)return dc_actual(context,source,functions,control,output);
    }
    DC_CHECK(source->module_count && !p->source&&!p->control&&!p->copy);
    p->source=source;p->control=control;p->bytes=(size_t)functions*sizeof(*source->functions);if(!p->implementation)DC_CHECK(p->bytes==24240);
    if(!p->resources)p->capture(p,context->resources);else DC_CHECK(p->resources==context->resources);
    p->implementations_returned=false;XrCompileResourceStats before={0},after={0};
    DC_CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);p->held_before=before.held_bytes;
    XrXirStatus status=dc_actual(context,source,functions,control,output);
    DC_CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);p->held_after=after.held_bytes;
    if(status!=XR_XIR_OK)DC_CHECK(!*output&&p->held_before==p->held_after);
    p->source=NULL;p->copy=NULL;p->control=NULL;return status;
}
