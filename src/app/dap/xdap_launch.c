/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_launch.c - Owned source admission and immutable debug code display
 */
#include "xdap_launch.h"
#include "../../xir/xxir_internal.h"
#include "../../xir/xxir_vm_debug_internal.h"
#include "../../xir/xxir_value_internal.h"
#include <string.h>
static void launch_diagnostic(XrSourceCompileDiagnostic *diagnostic,XrXirStatus status) {
    if (status==XR_XIR_OK || diagnostic->status!=XR_SOURCE_COMPILE_OK) return;
    diagnostic->status=status==XR_XIR_OUT_OF_MEMORY ? XR_SOURCE_COMPILE_OUT_OF_MEMORY :
        status==XR_XIR_MEMORY_LIMIT ? XR_SOURCE_COMPILE_MEMORY_LIMIT :
        status==XR_XIR_OVERFLOW ? XR_SOURCE_COMPILE_OVERFLOW :
        status==XR_XIR_CANCELLED ? XR_SOURCE_COMPILE_CANCELLED :
        status==XR_XIR_DEADLINE ? XR_SOURCE_COMPILE_DEADLINE :
        status==XR_XIR_CLOCK_FAILURE ? XR_SOURCE_COMPILE_CLOCK_FAILURE :
        status==XR_XIR_BUDGET ? XR_SOURCE_COMPILE_BUDGET : status==XR_XIR_UNSUPPORTED ? XR_SOURCE_COMPILE_UNSUPPORTED :
        status==XR_XIR_IO ? XR_SOURCE_COMPILE_IO : XR_SOURCE_COMPILE_REJECTED;
    diagnostic->stage=XR_SOURCE_COMPILE_PRODUCT;diagnostic->source.status=status;
}
static XrXirStatus launch_source_status(XrSourceCompileStatus status) {
    switch (status) {
    case XR_SOURCE_COMPILE_OK:return XR_XIR_OK;
    case XR_SOURCE_COMPILE_BUDGET:case XR_SOURCE_COMPILE_LIMIT:return XR_XIR_BUDGET;
    case XR_SOURCE_COMPILE_OUT_OF_MEMORY:return XR_XIR_OUT_OF_MEMORY;
    case XR_SOURCE_COMPILE_MEMORY_LIMIT:return XR_XIR_MEMORY_LIMIT;
    case XR_SOURCE_COMPILE_OVERFLOW:return XR_XIR_OVERFLOW;
    case XR_SOURCE_COMPILE_CANCELLED:return XR_XIR_CANCELLED;
    case XR_SOURCE_COMPILE_DEADLINE:return XR_XIR_DEADLINE;
    case XR_SOURCE_COMPILE_CLOCK_FAILURE:return XR_XIR_CLOCK_FAILURE;
    case XR_SOURCE_COMPILE_UNSUPPORTED:return XR_XIR_UNSUPPORTED;
    default:return XR_XIR_BAD_STRUCTURE;
    }
}
static XrXirStatus launch_value_status(XrXirValueStatus status) {
    return status==XR_XIR_VALUE_OK ? XR_XIR_OK : status==XR_XIR_VALUE_OOM ? XR_XIR_OUT_OF_MEMORY :
        status==XR_XIR_VALUE_MEMORY_LIMIT ? XR_XIR_MEMORY_LIMIT :
        status==XR_XIR_VALUE_OVERFLOW ? XR_XIR_OVERFLOW :
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus launch_allocation(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
        status==XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_MEMORY_LIMIT :
        status==XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_OVERFLOW :
        status==XR_COMPILE_RESOURCE_CANCELLED ? XR_XIR_CANCELLED :
        status==XR_COMPILE_RESOURCE_DEADLINE ? XR_XIR_DEADLINE :
        status==XR_COMPILE_RESOURCE_CLOCK_FAILURE ? XR_XIR_CLOCK_FAILURE : XR_XIR_BAD_STRUCTURE;
}
XR_FUNC XrXirCallStatus xdap_launch_free(XdapLaunch *launch) {
    if (!launch) return XR_XIR_CALL_READY;
    XrXirCallStatus status=xdap_current_free(launch->control);
    if (status==XR_XIR_CALL_BUSY) return status;
    xr_xir_compile_source_product_free(launch->source);
    if (launch->functions) for (uint32_t i=0;i<launch->function_count;++i) {
        xr_compile_resources_free(launch->functions[i].name);
        xr_compile_resources_free(launch->functions[i].source_name);
    }
    if (launch->modules) for (uint32_t i=0;i<launch->module_count;++i) {
        xr_compile_resources_free(launch->modules[i].identity);
        xr_compile_resources_free(launch->modules[i].path);
    }
    xr_compile_resources_free(launch->modules);
    xr_compile_resources_free(launch->functions);xr_compile_resources_free(launch->instructions);
    xr_compile_resources_free(launch->absolute_file);
    xr_compile_resources_free(launch);return status;
}
static XrXirStatus launch_text_copy(XdapLaunch *launch,const char *text,size_t length,char **output) {
    if (length==SIZE_MAX) return XR_XIR_OVERFLOW;
    XrXirStatus status=launch_allocation(xr_compile_resources_alloc(launch->resources,length+1,(void **)output));
    if (status==XR_XIR_OK) { memcpy(*output,text,length);(*output)[length]=0; }
    return status;
}
static XrXirStatus launch_text_length(const char *text,size_t *output) {
    size_t length=0;
    for (;;) {
        if (!text[length]) break;
        if (length==SIZE_MAX-1) return XR_XIR_OVERFLOW;
        ++length;
    }
    *output=length;return XR_XIR_OK;
}
static XrXirStatus launch_modules(XdapLaunch *launch,const XrXirModule *module) {
    const XrXirSourceView *view=xr_xir_compile_source_product_view(launch->source);
    const XrXirDeclarations *declarations=module->declarations;
    if (!view || !view->complete || !declarations || !declarations->functions || !declarations->modules ||
        !view->modules) return XR_XIR_BAD_STRUCTURE;
    uint32_t count=declarations->module_count;
    if (!count) return XR_XIR_BAD_STRUCTURE;
    if (sizeof(*launch->modules)>SIZE_MAX/(size_t)count) return XR_XIR_OVERFLOW;
    launch->module_count=count;
    XrXirStatus status=launch_allocation(xr_compile_resources_calloc(launch->resources,count,
        sizeof(*launch->modules),(void **)&launch->modules));
    if (status!=XR_XIR_OK) return status;
    for (uint32_t m=0;m<count;++m) {
        const XrXirSourceModule *owned=&declarations->modules[m];const XrXirSourceQueryModule *matched=NULL;
        for (uint32_t q=0;q<view->module_count;++q) {
            const XrXirSourceQueryModule *candidate=&view->modules[q];
            if (!candidate->identity) return XR_XIR_BAD_STRUCTURE;
            size_t length=0;status=launch_text_length(candidate->identity,&length);
            if (status!=XR_XIR_OK) return status;
            if (length!=owned->name_length || memcmp(candidate->identity,owned->name,length)) continue;
            if (matched) return XR_XIR_BAD_STRUCTURE;
            matched=candidate;launch->modules[m].query_module=q;
        }
        if (!matched) return XR_XIR_BAD_STRUCTURE;
        XdapCodeModule *target=&launch->modules[m];
        target->identity_length=owned->name_length;
        status=launch_text_copy(launch,matched->identity,owned->name_length,&target->identity);
        if (status!=XR_XIR_OK) return status;
        if (matched->path) {
            status=launch_text_length(matched->path,&target->path_length);
            if (status!=XR_XIR_OK) return status;
            status=launch_text_copy(launch,matched->path,target->path_length,&target->path);
            if (status!=XR_XIR_OK) return status;
            for (size_t i=0;i<target->path_length;++i) if (target->path[i]=='/' || target->path[i]=='\\') target->name_offset=i+1;
        }
        target->fingerprint=matched->fingerprint;
    }
    return XR_XIR_OK;
}
static XrXirStatus launch_code(XdapLaunch *launch,const XrXirModule *module) {
    XrCompileResources *resources=launch->resources;
    uint64_t instructions=0;
    for (uint32_t i=0;i<module->function_count;++i) {
        instructions+=module->functions[i].instruction_count;
        if (instructions>UINT32_MAX) return XR_XIR_BUDGET;
    }
    launch->function_count=module->function_count;launch->instruction_count=(uint32_t)instructions;
    XrXirStatus status=launch_allocation(xr_compile_resources_calloc(resources,module->function_count,
        sizeof(*launch->functions),(void **)&launch->functions));
    if (status!=XR_XIR_OK) return status;
    if (instructions>SIZE_MAX/sizeof(*launch->instructions)) return XR_XIR_OVERFLOW;
    status=launch_allocation(xr_compile_resources_alloc(resources,(size_t)instructions*sizeof(*launch->instructions),
        (void **)&launch->instructions));
    if (status!=XR_XIR_OK) return status;
    uint32_t first=0;
    for (uint32_t i=0;i<module->function_count;++i) {
        const XrXirFunction *source=&module->functions[i];XdapCodeFunction *target=&launch->functions[i];
        target->module=module->declarations->functions[i].module;
        if (target->module>=launch->module_count) return XR_XIR_BAD_STRUCTURE;
        const XrXirModule *origin_module=module;uint32_t origin=i;
        const XrXirProvenance *provenance=module->provenance;
        if (provenance && provenance->kind==XR_XIR_EVIDENCE_INSTANCE) {
            const XrXirCompileContext *origin_context=xr_xir_compile_artifact_context(provenance->source);
            if (!provenance->source || provenance->count!=module->function_count || !provenance->origins ||
                !origin_context || origin_context->resources!=resources) return XR_XIR_BAD_STRUCTURE;
            origin_module=xr_xir_compile_artifact_module(provenance->source);origin=provenance->origins[i].function;
        } else if (provenance && provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE) return XR_XIR_BAD_STRUCTURE;
        if (!origin_module || origin>=origin_module->function_count) return XR_XIR_BAD_STRUCTURE;
        const XrXirFunction *declaration=&origin_module->functions[origin];target->origin=origin;
        status=launch_allocation(xr_compile_resources_alloc(resources,(size_t)declaration->name_length+1,(void **)&target->source_name));
        if (status!=XR_XIR_OK) return status;
        if (declaration->name_length) memcpy(target->source_name,declaration->name,declaration->name_length);
        target->source_name[declaration->name_length]=0;
        target->first=first;target->instructions=source->instruction_count;target->parameters=source->parameter_count;
        target->values=source->parameter_count+source->instruction_count;
        status=launch_allocation(xr_compile_resources_alloc(resources,(size_t)source->name_length+1,(void **)&target->name));
        if (status!=XR_XIR_OK) return status;
        uint64_t bytes=(uint64_t)source->instruction_count*sizeof(*launch->instructions);
        if (source->name_length) memcpy(target->name,source->name,source->name_length);
        target->name[source->name_length]=0;
        memcpy(launch->instructions+first,source->instructions,(size_t)bytes);first+=source->instruction_count;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xdap_launch_source_point(const XdapLaunch *launch,uint32_t function,uint32_t instruction,
    XrXirSourceDebugPoint *output) {
    if (!launch || !output || function>=launch->function_count || instruction>=launch->functions[function].instructions)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceDebugView *debug=xr_xir_compile_source_product_debug(launch->source);
    if (!debug || debug->function_count!=launch->function_count || !debug->functions) return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceDebugFunction *mapped=&debug->functions[function];
    if (mapped->function!=function || mapped->instruction_count!=launch->functions[function].instructions)
        return XR_XIR_BAD_STRUCTURE;
    if (!mapped->points || !mapped->points[instruction].statement) return XR_XIR_UNRESOLVED;
    XrXirSourceDebugPoint point=mapped->points[instruction];
    uint32_t module=launch->functions[function].module;
    if (module>=launch->module_count || point.range.module!=launch->modules[module].query_module ||
        point.range.line<=0 || point.range.column<=0) return XR_XIR_BAD_STRUCTURE;
    *output=point;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xdap_launch_function_rules(const XdapLaunch *launch,const XdapFunctionSelection *selection,
    XrXirVmDebugRule *output,uint32_t capacity,uint32_t *count) {
    if (!launch || !selection || !selection->name || !count || (capacity && !output) || capacity>32) return XR_XIR_BAD_STRUCTURE;
    uint32_t origin=UINT32_MAX,matches=0;XrXirVmDebugRule fresh[32]={{0}};
    for (uint32_t f=0;f<launch->function_count;++f) {
        const XdapCodeFunction *function=&launch->functions[f];
        size_t length=strlen(function->source_name);
        if (selection->length!=length || memcmp(selection->name,function->source_name,length)) continue;
        if (origin!=UINT32_MAX && origin!=function->origin) return XR_XIR_UNRESOLVED;
        origin=function->origin;
        if (matches==capacity) return XR_XIR_BUDGET;
        fresh[matches]=selection->rule;fresh[matches++].location=(XrXirVmDebugLocation){f,0};
    }
    if (matches) memcpy(output,fresh,(size_t)matches*sizeof(*output));*count=matches;return XR_XIR_OK;
}
static XrXirStatus launch_program(const XdapLaunchRequest *request,const XrXirSourceProduct *source,
    XrXirArtifact **lowered,XrXirVmDebug *debug,XdapLaunch *launch,XrXirProgram **output) {
    if (!request->native.load) return xr_xir_compile_vm_debug_program_take(lowered,debug,output);
    XdapNativeCode native={0};
    XrXirStatus status=request->native.load(request->native.context,source,*lowered,&native);
    if (status==XR_XIR_OK) {
        XrXirVmDebugNative input={&native.program,native.source,native.mask,native.count};
        status=xr_xir_compile_vm_debug_mixed_program_take(lowered,&input,debug,output);
        if (status==XR_XIR_OK) {
            const XrXirDeclarations *declarations=native.program.declarations;
            if (status==XR_XIR_OK) {
                launch->native_startup=native.mask[launch->entry]!=0;launch->native_only=true;
                for(uint32_t m=0;m<declarations->module_count;++m)
                    if(native.mask[declarations->modules[m].initializer]) launch->native_startup=true;
                for(uint32_t f=0;f<native.count;++f) if(!native.mask[f]) launch->native_only=false;
            }
        }
    }
    if (native.program.code.release) native.program.code.release(native.program.code.owner);
    return status;
}
XR_FUNC XrXirStatus xdap_launch_build(const XdapLaunchRequest *request,XdapLaunch **output,
    XrSourceCompileDiagnostic *diagnostic) {
    if (!request || !request->compiler || !output || *output || !diagnostic ||
        (!!request->absolute_file == !!request->text) || (!request->native.load && request->native.context) ||
        !xr_xir_domain_current(request->observer_metadata))
        return XR_XIR_BAD_STRUCTURE;
    const XrXirCompileContext *context=xr_source_compiler_context(request->compiler);
    if (!context) return XR_XIR_BAD_STRUCTURE;
    XdapLaunch *launch=NULL;XrXirArtifact *lowered=NULL;XrXirVmDebug *debug=NULL;XrXirProgram *program=NULL;
    XrXirStatus status=launch_allocation(xr_compile_resources_calloc(context->resources,1,sizeof(*launch),(void **)&launch));
    if (status!=XR_XIR_OK) { launch_diagnostic(diagnostic,status);return status; }
    launch->resources=context->resources;
    if (request->absolute_file) {
        size_t length=0;
        while (request->absolute_file[length]) {
            ++length;
        }
        status=launch_allocation(xr_compile_resources_alloc(context->resources,length+1,(void **)&launch->absolute_file));
        if (status!=XR_XIR_OK) goto done;
        memcpy(launch->absolute_file,request->absolute_file,length+1);
    }
    status=launch_source_status(request->text ?
        xr_source_compiler_build_text(request->compiler,request->text,&launch->source,diagnostic) :
        xr_source_compiler_build_file(request->compiler,request->absolute_file,&launch->source,diagnostic));
    if (status!=XR_XIR_OK) goto done;
    status=xr_xir_compile_source_product_public_admit(launch->source,&diagnostic->source.source);
    if (status!=XR_XIR_OK) goto done;
    const XrXirSourceProductFacts *facts=xr_xir_compile_source_product_facts(launch->source);
    XrXirSourceProductPacketView packet={0};
    status=xr_xir_compile_source_product_packet(launch->source,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet);
    if (status!=XR_XIR_OK) goto done;
    status=xr_xir_compile_checked_read_lower(context,packet.bytes,packet.length,&facts->target,&lowered,&diagnostic->source.xir);
    if (status!=XR_XIR_OK) goto done;
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    if (module->function_count!=facts->function_count || module->declarations->entry_function!=facts->entry ||
        module->declarations->module_count!=facts->module_count) { status=XR_XIR_BAD_STRUCTURE;goto done; }
    launch->entry=facts->entry;
    status=launch_modules(launch,module);if (status!=XR_XIR_OK) goto done;
    status=launch_code(launch,module);if (status!=XR_XIR_OK) goto done;
    status=launch_value_status(xr_xir_vm_debug_new(request->observer_metadata,request->instance.depth_limit,&debug));
    if (status!=XR_XIR_OK) goto done;
    status=launch_program(request,launch->source,&lowered,debug,launch,&program);if (status!=XR_XIR_OK) goto done;
    status=xir_vm_debug_source_map(debug,context,xr_xir_compile_source_product_debug(launch->source));if (status!=XR_XIR_OK) goto done;
    status=launch_value_status(xdap_current_new(request->observer_metadata,&debug,program,&request->instance,&launch->control));
    if (status==XR_XIR_OK) { *output=launch;launch=NULL; }
done:
    launch_diagnostic(diagnostic,status);
    xr_xir_compile_program_drop(program);xr_xir_vm_debug_drop(debug);xr_xir_compile_artifact_free(lowered);
    xdap_launch_free(launch);return status;
}
