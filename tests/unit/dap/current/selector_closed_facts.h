/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * selector_closed_facts.h - Exact closed ordinals and hostile map rejection
 */
#ifndef DAP_SELECTOR_CLOSED_FACTS_H
#define DAP_SELECTOR_CLOSED_FACTS_H
static void selector_closed_facts(HandlerFixture *fixture,bool generic) {
    const XrXirCompileContext *context=xr_source_compiler_context(fixture->compiler);
    XrXirSourceProductPacketView packet={0};XrXirArtifact *closed=NULL,*lowered=NULL;XrXirProgram *program=NULL;XrXirVmDebug *observer=NULL;
    CHECK(xr_xir_compile_source_product_packet(fixture->launch->source,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&closed,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(closed);
    const XrXirSourceDebugView *debug=xr_xir_compile_source_product_debug(fixture->launch->source);CHECK(debug && debug->function_count==module->function_count);
    uint32_t selected=UINT32_MAX,generics=0;XrXirType targets[2]={0};
    for (uint32_t f=0;f<debug->function_count;++f) {
        const XrXirSourceDebugFunction *function=&debug->functions[f];
        CHECK(function->parameter_count==module->functions[f].parameter_count && function->instruction_count==module->functions[f].instruction_count);
        if (!function->handler_count) continue;
        if (selected==UINT32_MAX) selected=f;
        for (uint32_t h=0;h<function->handler_count;++h) {
            const XrXirSourceDebugHandler *handler=&function->handlers[h];
            CHECK(module->functions[f].instructions[handler->error-function->parameter_count].op==XR_XIR_PHI);
            for (uint32_t s=0;s<handler->selector_count;++s) {
                const XrXirSourceDebugSelector *selector=&handler->selectors[s];
                CHECK(selector->accepted<function->instruction_count && function->points[selector->accepted].flags&XR_XIR_SOURCE_DEBUG_HANDLER_ACCEPT);
                if (generic && !s && !h) {
                    CHECK(generics<2 && module->functions[f].instructions[selector->first].op==XR_XIR_ERROR_IS);
                    targets[generics++]=(XrXirType)module->functions[f].instructions[selector->first].immediate;
                }
            }
        }
    }
    CHECK(selected!=UINT32_MAX);
    if (generic) CHECK(generics==2 && targets[0]!=targets[1]);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_vm_debug_new(fixture->domain,64,&observer)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_vm_debug_program_take(&lowered,observer,&program)==XR_XIR_OK && !lowered);
    const XrXirSourceDebugFunction *original=&debug->functions[selected];
    XrXirSourceDebugFunction *functions=NULL;XrXirSourceDebugHandler *handlers=NULL;XrXirSourceDebugRoute *routes=NULL;
    size_t function_bytes=(size_t)debug->function_count*sizeof(*functions),handler_bytes=(size_t)original->handler_count*sizeof(*handlers);
    size_t route_bytes=(size_t)original->route_count*sizeof(*routes);
    CHECK(route_bytes && xr_compile_resources_alloc(context->resources,function_bytes,(void **)&functions)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_alloc(context->resources,handler_bytes,(void **)&handlers)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_alloc(context->resources,route_bytes,(void **)&routes)==XR_COMPILE_RESOURCE_OK);
    for (uint32_t variant=0;variant<9;++variant) {
        memcpy(functions,debug->functions,function_bytes);memcpy(handlers,original->handlers,handler_bytes);memcpy(routes,original->routes,route_bytes);
        functions[selected].handlers=handlers;functions[selected].routes=routes;
        if (!variant) handlers[0].parent=1;
        else if (variant==1) handlers[0].error=UINT32_MAX;
        else if (variant==2) handlers[0].selectors=NULL;
        else if (variant==3) routes[0].handler=original->handler_count+1;
        else if (variant==4) functions[selected].handler_count=0;
        else if (variant==5) routes[0].error=UINT32_MAX;
        else if (variant==6) routes[0].edge=UINT32_MAX;
        else if (variant==7) routes[0].input=UINT32_MAX;
        else if (original->handler_count>1) routes[0].handler=original->handler_count;
        else routes[0].error=handlers[0].error;
        XrXirSourceDebugView input=*debug;input.functions=functions;
        XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
        CHECK(xir_vm_debug_source_map(observer,context,&input)==XR_XIR_BAD_STRUCTURE);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==before.live_bytes);
    }
    memcpy(functions,debug->functions,function_bytes);memcpy(handlers,original->handlers,handler_bytes);memcpy(routes,original->routes,route_bytes);
    functions[selected].handlers=handlers;functions[selected].routes=routes;XrXirSourceDebugView input=*debug;input.functions=functions;
    size_t begin=runtime_attempts;runtime_fail_at=begin;
    CHECK(xir_vm_debug_source_map(observer,context,&input)==XR_XIR_OUT_OF_MEMORY && runtime_attempts==begin+1);runtime_fail_at=SIZE_MAX;
    CHECK(xir_vm_debug_source_map(observer,context,&input)==XR_XIR_OK);
    memset(functions,0xff,function_bytes);memset(handlers,0xff,handler_bytes);memset(routes,0xff,route_bytes);
    xr_compile_resources_free(functions);xr_compile_resources_free(handlers);xr_compile_resources_free(routes);
    xr_xir_compile_program_drop(program);xr_xir_vm_debug_drop(observer);
}
#endif // DAP_SELECTOR_CLOSED_FACTS_H
