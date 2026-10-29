/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
/* Observe the canonical implementation TU; installed SDK clients separately
 * link the archive. No request or resource owner is stored in ready metadata. */
static XrCompileResourceStatus ic_poll(XrCompileRequestControl *control) {
    DeclarationsCopyProbe *p=dc_probe;
    if(p&&p->implementation_source&&p->implementation_copy&&p->control==control) {
        const XrXirImplementationTable *source=p->implementation_source,*copy=p->implementation_copy;
        if(copy->records) {
            if(p->phase==IC_RECORD&&!copy->records[0].interface.arguments)p->wanted=true;
            for(uint32_t i=0;i<source->count;++i) {
                const XrXirImplementation *record=&copy->records[i];
                if(p->phase==IC_BINDING&&record->binding_count==192&&record->bindings&&
                    record->bindings[0].requirement.arguments&&!record->bindings[1].requirement.arguments)p->wanted=true;
            }
            if(p->phase==IC_PUBLICATION) {
                const XrXirImplementation *last=&copy->records[copy->count-1],*original=&source->records[source->count-1];
                if(last->binding_count==original->binding_count&&last->binding_count&&last->bindings&&
                    last->bindings[last->binding_count-1].requirement.arguments)p->wanted=true;
            }
        }
    }
    if(p&&p->wanted&&p->mode==1&&!p->witnesses) {
        DC_CHECK(control->has_deadline);p->witnesses=1;uint64_t now=xr_time_monotonic_ns();DC_CHECK(now);
        while(now<control->deadline_ns){xr_time_sleep_ns(1000000);now=xr_time_monotonic_ns();DC_CHECK(now);}
    }
    return xr_compile_request_control_poll(control);
}
static void *ic_alloc(const XrXirCompileContext *context,size_t bytes,XrXirStatus *status) {
    return xir_compile_alloc(context,bytes,status);
}
static void *ic_calloc(const XrXirCompileContext *context,size_t count,size_t bytes,XrXirStatus *status) {
    DeclarationsCopyProbe *p=dc_probe;
    bool fail=p&&p->implementation_source&&p->mode==2&&!p->oom_seen&&
        count==192&&bytes==sizeof(XrXirImplementationBinding);
    if(fail) {
        DC_CHECK(p->implementation_copy&&p->implementation_copy->records[0].interface.arguments);
        p->fail_next();
    }
    void *memory=xir_compile_calloc(context,count,bytes,status);
    if(p&&p->implementation_source&&count==1&&bytes==sizeof(XrXirImplementationTable)) {
        DC_CHECK(!p->implementation_copy);p->implementation_copy=memory;
    }
    if(fail){DC_CHECK(!memory&&*status==XR_XIR_OUT_OF_MEMORY);p->oom_seen=true;p->wanted=true;}
    return memory;
}
static XrCompileResourceStatus ic_span(XrCompileRequestControl *control,void *destination,const void *source,size_t bytes) {
    DeclarationsCopyProbe *p=dc_probe;bool observe=p&&p->implementation_source&&p->phase==IC_ARGUMENT&&bytes>4096;
    if(observe){DC_CHECK(!p->destination);p->destination=destination;p->argument_source=source;p->argument_bytes=bytes;memset(destination,0xa5,bytes);++p->copies;}
    XrCompileResourceStatus status=xir_lower_copy_span(control,destination,source,bytes);
    if(observe){p->destination=NULL;p->argument_source=NULL;}return status;
}
static XrXirStatus ic_actual(const XrXirCompileContext *,const XrXirImplementationTable *,XrCompileRequestControl *,XrXirImplementationTable **);
static void ic_free_actual(XrXirImplementationTable *);
#define xr_xir_compile_implementations_copy_verified ic_actual
#define xr_xir_compile_implementations_free ic_free_actual
#define xir_compile_alloc ic_alloc
#define xir_compile_calloc ic_calloc
#define xir_lower_copy_span ic_span
#define xr_compile_request_control_poll ic_poll
#include "xir/xxir_implementation.c"
#undef xr_compile_request_control_poll
#undef xir_lower_copy_span
#undef xir_compile_calloc
#undef xir_compile_alloc
#undef xr_xir_compile_implementations_free
#undef xr_xir_compile_implementations_copy_verified
XR_FUNC XrXirStatus test_implementation_copy(const XrXirCompileContext *context,
    const XrXirImplementationTable *source,XrCompileRequestControl *control,XrXirImplementationTable **output) {
    DeclarationsCopyProbe *p=dc_probe;DC_CHECK(p&&p->implementation&&!p->implementation_source&&!p->implementation_copy);
    bool own_control=!p->control;if(own_control)p->control=control;else DC_CHECK(p->control==control);
    p->implementation_source=source;XrCompileResourceStats before={0},after={0};
    DC_CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    p->implementation_held_before=before.held_bytes;
    XrXirStatus status=ic_actual(context,source,control,output);
    DC_CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    p->implementation_held_after=after.held_bytes;
    if(status!=XR_XIR_OK)DC_CHECK(p->implementation_held_before==p->implementation_held_after);
    p->implementation_source=NULL;p->implementation_copy=NULL;if(own_control)p->control=NULL;return status;
}
