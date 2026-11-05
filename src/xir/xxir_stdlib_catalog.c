/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_stdlib_catalog.c - Owned Catalog input from authenticated Checked bytes
 *
 * KEY CONCEPT: Checked library admission has no native code dependency.
 */
#include "xxir_stdlib_internal.h"
#include "xxir_compile_memory.h"
#include "../base/xfileio.h"
#include <string.h>
static XrXirStatus stdlib_catalog_root_status(XrOsIoStatus status) {
    switch(status){
    case XR_OS_IO_OK:return XR_XIR_OK;
    case XR_OS_IO_OUT_OF_MEMORY:return XR_XIR_OUT_OF_MEMORY;
    case XR_OS_IO_OVERFLOW:return XR_XIR_OVERFLOW;
    case XR_OS_IO_MEMORY_LIMIT:return XR_XIR_MEMORY_LIMIT;
    case XR_OS_IO_CANCELLED:return XR_XIR_CANCELLED;
    case XR_OS_IO_DEADLINE:return XR_XIR_DEADLINE;
    case XR_OS_IO_CLOCK_FAILURE:return XR_XIR_CLOCK_FAILURE;
    case XR_OS_IO_NOT_FOUND:return XR_XIR_UNRESOLVED;
    case XR_OS_IO_BAD_ARGUMENT:return XR_XIR_BAD_STRUCTURE;
    default:return XR_XIR_IO;
    }
}
static XrXirStatus stdlib_catalog_directory(const XrXirLibraryCatalog *catalog) {
    size_t count=0,begin=0;
    const XrModuleResourceBinding *resources=xr_xir_compile_library_catalog_resources_v2(catalog,&count);
    for(size_t c=0;c<xir_stdlib_checked_component_count;++c){
        const XirStdlibCheckedComponent *component=xir_stdlib_checked_components+c;
        if(begin>count||component->module_count>count-begin)return XR_XIR_BAD_STRUCTURE;
        for(size_t m=0;m<component->module_count;++m){
            const XirStdlibCheckedModule *expected=component->modules+m;
            const XrModuleResourceBinding *actual=resources+begin+m;
            if(!expected->canonical||strcmp(actual->canonical,expected->canonical)||actual->checked_module!=m||
                actual->checked!=resources[begin].checked||actual->dependency_count!=expected->dependency_count||
                (expected->dependency_count&&!expected->dependencies))return XR_XIR_BAD_STRUCTURE;
            for(uint32_t e=0;e<expected->dependency_count;++e)
                if(expected->dependencies[e]>=component->module_count||
                    actual->dependencies[e]!=resources+begin+expected->dependencies[e])return XR_XIR_BAD_STRUCTURE;
        }
        begin+=component->module_count;
    }
    return begin==count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
XR_FUNC XrXirStatus xir_stdlib_library_catalog_new(const XrXirCompileContext *context,
    const char *physical_root, XrXirLibraryCatalog **output) {
    if (!xir_compile_context_valid(context) || !physical_root || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    size_t count = xir_stdlib_checked_component_count;
    if (!count || count > SIZE_MAX / sizeof(XrXirLibraryInput)) return XR_XIR_BAD_STRUCTURE;
    size_t module_count=0;
    for(size_t c=0;c<count;++c){
        const XirStdlibCheckedComponent *component=xir_stdlib_checked_components+c;
        if(!component->modules||!component->module_count)return XR_XIR_BAD_STRUCTURE;
        if(component->module_count>UINT32_MAX||component->module_count>SIZE_MAX-module_count)return XR_XIR_BUDGET;
        module_count+=component->module_count;
    }
    if(module_count>SIZE_MAX/sizeof(XrXirLibraryModuleInput))return XR_XIR_BUDGET;
    XrXirStatus status = XR_XIR_OK;
    XrXirLibraryInput *inputs = xir_compile_calloc(context,count,sizeof(*inputs),&status);
    if (!inputs) return status;
    XrXirLibraryModuleInput *modules = xir_compile_calloc(context,module_count,sizeof(*modules),&status);
    if (!modules) { xr_compile_resources_free(inputs); return status; }
    XrOsIoPolicy policy=xr_compile_io_policy(context->resources);char *canonical_root=NULL;
    status=stdlib_catalog_root_status(xr_realpath_owned(&policy,physical_root,&canonical_root));
    if(status!=XR_XIR_OK){xr_compile_resources_free(canonical_root);xr_compile_resources_free(modules);xr_compile_resources_free(inputs);return status;}
    size_t begin=0;
    for (size_t i = 0; i < count; ++i) {
        const XirStdlibCheckedComponent *component = &xir_stdlib_checked_components[i];

        for(size_t m=0;m<component->module_count;++m){
            const XirStdlibCheckedModule *binding=component->modules+m;
            modules[begin+m]=(XrXirLibraryModuleInput){{XR_MODULE_IDENTITY_STDLIB,
                binding->namespace_id,canonical_root},binding->logical_path};
        }
        inputs[i] = (XrXirLibraryInput){component->bytes,component->length,{0},modules+begin,component->module_count};
        memcpy(inputs[i].sha256,component->sha256,sizeof(inputs[i].sha256));
        begin+=component->module_count;
    }
    /* Each complete owner and all its exact module views share one ledger. */
    XrXirLibraryCatalog *catalog=NULL;
    if (status == XR_XIR_OK)
        status = xr_xir_compile_library_catalog_new_v2(context,inputs,count,&catalog);
    if(status==XR_XIR_OK)status=stdlib_catalog_directory(catalog);
    if(status==XR_XIR_OK){*output=catalog;catalog=NULL;}
    xr_xir_compile_library_catalog_free(catalog);
    xr_compile_resources_free(canonical_root);
    xr_compile_resources_free(modules);
    xr_compile_resources_free(inputs);
    return status;
}
