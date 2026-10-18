/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_handler_mutations.h - Reject foreign observations before publication
 */
#ifndef XIR_SOURCE_HANDLER_MUTATIONS_H
#define XIR_SOURCE_HANDLER_MUTATIONS_H
typedef struct HandlerMutable {
    XrXirSourceDebugFunction *functions;
    XrXirSourceDebugHandler *handlers;
    XrXirSourceDebugSelector *selectors;
    XrXirSourceDebugRoute *routes;
    size_t functions_bytes,handlers_bytes,selectors_bytes,routes_bytes;
} HandlerMutable;
static XrXirStatus handler_array(const XrXirCompileContext *context,const void *input,size_t bytes,void **output) {
    XrCompileResourceStatus status=xr_compile_resources_alloc(context->resources,bytes,output);
    if (status!=XR_COMPILE_RESOURCE_OK) return status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    if (bytes) memcpy(*output,input,bytes);return XR_XIR_OK;
}
static void handler_mutable_drop(HandlerMutable *copy) {
    if (copy->functions) memset(copy->functions,0xff,copy->functions_bytes);
    if (copy->handlers) memset(copy->handlers,0xff,copy->handlers_bytes);
    if (copy->selectors) memset(copy->selectors,0xff,copy->selectors_bytes);
    if (copy->routes) memset(copy->routes,0xff,copy->routes_bytes);
    xr_compile_resources_free(copy->functions);xr_compile_resources_free(copy->handlers);
    xr_compile_resources_free(copy->selectors);xr_compile_resources_free(copy->routes);*copy=(HandlerMutable){0};
}
static XrXirStatus handler_mutable(const XrXirCompileContext *context,const XrXirSourceDebugView *debug,
    uint32_t f,HandlerMutable *copy) {
    const XrXirSourceDebugFunction *function=&debug->functions[f];uint32_t count=0;
    for (uint32_t h=0;h<function->handler_count;++h) count+=function->handlers[h].selector_count;
    copy->functions_bytes=(size_t)debug->function_count*sizeof(*copy->functions);
    copy->handlers_bytes=(size_t)function->handler_count*sizeof(*copy->handlers);
    copy->selectors_bytes=(size_t)count*sizeof(*copy->selectors);
    copy->routes_bytes=(size_t)function->route_count*sizeof(*copy->routes);
    XrXirStatus status=handler_array(context,debug->functions,copy->functions_bytes,(void **)&copy->functions);
    if (status==XR_XIR_OK && copy->handlers_bytes) status=handler_array(context,function->handlers,copy->handlers_bytes,(void **)&copy->handlers);
    if (status==XR_XIR_OK && copy->routes_bytes) status=handler_array(context,function->routes,copy->routes_bytes,(void **)&copy->routes);
    if (status==XR_XIR_OK && copy->selectors_bytes) {
        XrCompileResourceStatus allocated=xr_compile_resources_alloc(context->resources,copy->selectors_bytes,(void **)&copy->selectors);
        if (allocated!=XR_COMPILE_RESOURCE_OK) status=allocated==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    }
    if (status!=XR_XIR_OK) return status;
    uint32_t at=0;
    for (uint32_t h=0;h<function->handler_count;++h) {
        uint32_t size=function->handlers[h].selector_count;
        memcpy(copy->selectors+at,function->handlers[h].selectors,(size_t)size*sizeof(*copy->selectors));
        copy->handlers[h].selectors=copy->selectors+at;at+=size;
    }
    copy->functions[f].handlers=copy->handlers;copy->functions[f].routes=copy->routes;return XR_XIR_OK;
}
static XrXirStatus handler_copy_poison(const XrXirCompileContext *context,XrXirSourceSnapshot *snapshot,
    const XrXirSourceDebugView *debug,const XrXirModule *module,uint32_t f) {
    HandlerMutable copy={0};XrXirStatus status=handler_mutable(context,debug,f,&copy);
    XrXirSourceDebugView input=*debug;input.functions=copy.functions;
    if (status==XR_XIR_OK) status=xr_xir_compile_source_snapshot_debug_copy(snapshot,&input,module);
    handler_mutable_drop(&copy);return status;
}
static void handler_corrupt_code(const XrXirCompileContext *context,XrXirSourceSnapshot *snapshot,
    const XrXirSourceDebugView *debug,const XrXirModule *module,uint32_t f) {
    const XrXirSourceDebugFunction *function=&debug->functions[f];
    const XrXirSourceDebugHandler *handler=&function->handlers[0];
    uint32_t first=handler->selectors[0].first;
    CHECK(first+1<handler->selectors[0].accepted && module->functions[f].instructions[first+1].op==XR_XIR_BRANCH);
    size_t function_bytes=(size_t)module->function_count*sizeof(*module->functions);
    size_t instruction_bytes=(size_t)function->instruction_count*sizeof(XrXirInstruction);
    for (uint32_t variant=0;variant<8;++variant) {
        XrXirModule changed=*module;XrXirFunction *bodies=NULL;XrXirInstruction *instructions=NULL;
        CHECK(handler_array(context,module->functions,function_bytes,(void **)&bodies)==XR_XIR_OK);
        CHECK(handler_array(context,module->functions[f].instructions,instruction_bytes,(void **)&instructions)==XR_XIR_OK);
        changed.functions=bodies;bodies[f].instructions=instructions;
        switch (variant) {
        case 0:instructions[first].op=XR_XIR_CALL;break;
        case 1:instructions[first+1].args[0]=0;break;
        case 2:instructions[first+1].targets[0]=0;break;
        case 3:instructions[handler->error-function->parameter_count].type=XR_XIR_I64;break;
        case 4:instructions[handler->propagation].targets[0]=0;break;
        case 5:instructions[function->handlers[1].propagation].args[0]=handler->error;break;
        case 6:bodies[f].blocks=NULL;break;
        case 7:instructions[first+1].op=XR_XIR_LOCAL_READ;break;
        }
        uint64_t live=library_compile_stats(context).live_bytes;
        CHECK(xr_xir_compile_source_snapshot_debug_copy(snapshot,debug,&changed)==XR_XIR_BAD_STRUCTURE);
        CHECK(!xr_xir_compile_source_snapshot_debug(snapshot) && library_compile_stats(context).live_bytes==live);
        memset(bodies,0xff,function_bytes);memset(instructions,0xff,instruction_bytes);
        xr_compile_resources_free(bodies);xr_compile_resources_free(instructions);
    }
}
static void handler_mutations(const XrXirCompileContext *context,const XrXirSourceResult *source,uint32_t f) {
    const XrXirModule *module=xr_xir_compile_artifact_module(source->checked);
    const XrXirSourceDebugView *debug=xr_xir_compile_source_snapshot_debug(source->snapshot);
    XrXirSourceSnapshot *snapshot=NULL;
    CHECK(xr_xir_compile_source_snapshot_copy_v2(context,xr_xir_compile_source_snapshot_view(source->snapshot),
        xr_xir_compile_source_snapshot_construction(source->snapshot),&snapshot)==XR_XIR_OK);
    for (uint32_t variant=0;variant<18;++variant) {
        HandlerMutable copy={0};CHECK(handler_mutable(context,debug,f,&copy)==XR_XIR_OK);
        XrXirSourceDebugFunction *function=&copy.functions[f];
        switch (variant) {
        case 0:function->handlers=NULL;break;
        case 1:function->routes=NULL;break;
        case 2:copy.handlers[0].parent=1;break;
        case 3:copy.handlers[0].parent=function->handler_count+1;break;
        case 4:copy.handlers[0].error=UINT32_MAX;break;
        case 5:copy.handlers[0].selectors=NULL;break;
        case 6:copy.selectors[0].first=copy.selectors[0].accepted+1;break;
        case 7:copy.selectors[0].accepted=function->instruction_count;break;
        case 8:copy.selectors[0].rejected=copy.selectors[0].accepted;break;
        case 9:copy.selectors[0].rejected=UINT32_MAX;break;
        case 10:copy.routes[0].handler=0;break;
        case 11:copy.routes[1]=copy.routes[0];break;
        case 12:copy.routes[0].instruction=copy.handlers[0].error-function->parameter_count;break;
        case 13:copy.routes[0].error=UINT32_MAX;break;
        case 14:copy.routes[0].edge=UINT32_MAX;break;
        case 15:copy.routes[0].input=UINT32_MAX;break;
        case 16:copy.routes[0].handler=2;break;
        case 17:CHECK(copy.handlers[0].propagation_input<2);copy.handlers[0].propagation_input^=1;break;
        }
        XrXirSourceDebugView input=*debug;input.functions=copy.functions;
        uint64_t live=library_compile_stats(context).live_bytes;
        CHECK(xr_xir_compile_source_snapshot_debug_copy(snapshot,&input,module)==XR_XIR_BAD_STRUCTURE);
        CHECK(!xr_xir_compile_source_snapshot_debug(snapshot) && library_compile_stats(context).live_bytes==live);
        handler_mutable_drop(&copy);
    }
    handler_corrupt_code(context,snapshot,debug,module,f);
    CHECK(handler_copy_poison(context,snapshot,debug,module,f)==XR_XIR_OK);
    handler_check(xr_xir_compile_source_snapshot_debug(snapshot),f,0);
    xr_xir_compile_source_snapshot_free(snapshot);
}
#endif // XIR_SOURCE_HANDLER_MUTATIONS_H
