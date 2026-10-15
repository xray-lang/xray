/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_lexical.c - Immutable lexical facts never grant execution or mutation
 */
#include "xdap_lexical.h"
#include <string.h>
XR_FUNC XrXirStatus xdap_lexical_open(const XdapLaunch *launch,const XrXirVmDebugSnapshot *snapshot,
    uint32_t frame,XdapLexicalView *output) {
    if (!launch || !snapshot || !output) return XR_XIR_BAD_STRUCTURE;
    XrXirVmDebugFrameInfo info={0};XrXirValue name={0};
    XrXirValueStatus observed=xr_xir_vm_debug_snapshot_frame(snapshot,frame,&info,&name);
    if (observed!=XR_XIR_VALUE_OK) return observed==XR_XIR_VALUE_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
    xr_xir_value_drop(&name);
    const XrXirSourceDebugView *debug=xr_xir_compile_source_product_debug(launch->source);
    const XrXirSourceView *source=xr_xir_compile_source_product_view(launch->source);
    if (!debug || !source || !source->complete || info.function>=debug->function_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceDebugFunction *function=&debug->functions[info.function];
    if (info.instruction==UINT32_MAX) return XR_XIR_UNRESOLVED;
    if (!function->points || info.instruction>=function->instruction_count ||
        function->points[info.instruction].environment>function->binding_count) return XR_XIR_BAD_STRUCTURE;
    *output=(XdapLexicalView){launch,function,source,snapshot,frame,function->points[info.instruction].environment,info.id,info.epoch};
    return XR_XIR_OK;
}
XR_FUNC XrXirValueStatus xdap_lexical_read(const XdapLexicalView *view,uint32_t id,
    XrXirValue *output,const char **name,size_t *length) {
    if (!view || !view->launch || !view->function || !view->source || !view->snapshot || !id ||
        id>view->function->binding_count || !output || output->type || output->reserved || output->payload || !name || !length)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    bool visible=false;
    for (uint32_t b=view->environment;b;b=view->function->bindings[b-1].parent) {
        if (xr_compile_resources_work(view->launch->resources,1)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_VALUE_LIMIT;
        if (b>view->function->binding_count || view->function->bindings[b-1].parent>=b) return XR_XIR_VALUE_BAD_ARGUMENT;
        if (b==id) { visible=true;break; }
    }
    if (!visible) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirSourceDebugBinding *binding=&view->function->bindings[id-1];
    if (!binding->declaration || binding->declaration>view->source->declaration_count) return XR_XIR_VALUE_BAD_ARGUMENT;
    const char *text=view->source->declarations[binding->declaration-1].name;
    if (!text) return XR_XIR_VALUE_BAD_ARGUMENT;
    size_t bytes=strlen(text);
    if (xr_compile_resources_work(view->launch->resources,(uint64_t)bytes+1)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    if (!(binding->flags & XR_XIR_SOURCE_DEBUG_UNIT))
        status=binding->flags & XR_XIR_SOURCE_DEBUG_CELL ?
            xr_xir_vm_debug_snapshot_cell_value(view->snapshot,view->frame,binding->value,output) :
            xr_xir_vm_debug_snapshot_value(view->snapshot,view->frame,binding->value,output);
    if (status==XR_XIR_VALUE_OK) { *name=text;*length=bytes; }
    return status;
}
