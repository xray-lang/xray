/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_library_catalog.c - Exact artifact binding and catalog ownership
 *
 * KEY CONCEPT: Packet identity and declared owner must match the trusted binding.
 */
#include "xxir_library_catalog.h"
#include "xxir_compile_memory.h"
#include "xxir_effect_contract_internal.h"
#include "../base/xsha256.h"
#include "../base/xio_policy.h"
#include "../os/os_fs.h"
struct XrXirLibraryCatalog {
    XrXirCompileContext context;
    XrModuleResourceBinding *resources;
    XrXirArtifact **artifacts;
    size_t count, artifact_count;
};
XR_FUNC void xr_xir_compile_library_catalog_free(XrXirLibraryCatalog *catalog) {
    if (!catalog) return;
    for (size_t i = 0; i < catalog->count; ++i) {
        XrModuleResourceBinding *resource = &catalog->resources[i];
        xr_compile_resources_free((void *)resource->dependencies);
        xr_compile_resources_free((void *)resource->canonical);
        xr_compile_resources_free((void *)resource->logical_path);
        xr_compile_resources_free((void *)resource->source_locator);
        xr_compile_resources_free((void *)resource->authority.namespace_id);
        xr_compile_resources_free((void *)resource->authority.physical_root);
    }
    for (size_t i = 0; i < catalog->artifact_count; ++i)
        xr_xir_compile_artifact_free(catalog->artifacts[i]);
    xr_compile_resources_free(catalog->resources);
    xr_compile_resources_free(catalog->artifacts);
    xr_compile_resources_free(catalog);
}
XR_FUNC const XrModuleResourceBinding *xr_xir_compile_library_catalog_resources_v2(
    const XrXirLibraryCatalog *catalog, size_t *count) {
    if (!count) return NULL;
    *count = catalog ? catalog->count : 0;
    return catalog ? catalog->resources : NULL;
}
XR_FUNC const XrXirCompileContext *xr_xir_compile_library_catalog_context(const XrXirLibraryCatalog *catalog) {
    return catalog ? &catalog->context : NULL;
}
static XrXirStatus catalog_module_status(XrModuleStatus status) {
    switch (status) {
    case XR_MODULE_OK: return XR_XIR_OK;
    case XR_MODULE_BUDGET: return XR_XIR_BUDGET;
    case XR_MODULE_OUT_OF_MEMORY: return XR_XIR_OUT_OF_MEMORY;
    case XR_MODULE_IO: return XR_XIR_IO;
    case XR_MODULE_NOT_FOUND: return XR_XIR_UNRESOLVED;
    default: return XR_XIR_BAD_STRUCTURE;
    }
}
static bool catalog_length(const XrXirCompileContext *context, const char *text, size_t *length) {
    if (!text) return false;
    size_t n = 0;
    for (;;) {
        if (!xir_compile_work(context,1)) return false;
        if (!text[n]) { *length = n; return true; }
        if (n == SIZE_MAX-1) return false;
        ++n;
    }
}
static char *catalog_text(const XrXirCompileContext *context, const char *text, size_t length, XrXirStatus *status) {
    if (length == SIZE_MAX) { *status = XR_XIR_BUDGET; return NULL; }
    return xir_compile_copy(context,text,length+1,status);
}
#include "xxir_library_import_shape.inc.c"
#include "xxir_library_prelude.inc.c"
static XrXirStatus catalog_path(const XrXirCompileContext *context,
    const XrXirLibraryModuleInput *input, size_t length, size_t prefix,bool governed) {
    const char *path = input->logical_path;
    if (!length || length >= XR_PATH_MAX) return XR_XIR_BAD_STRUCTURE;
    /* The canonical identity constructor already checked every script path
     * component, including empty, dot and parent segments and root escape. */
    if (input->authority.kind == XR_MODULE_IDENTITY_SCRIPT) return XR_XIR_OK;
    /* Only the already verified native owner admits this non-source path.
     * Other stdlib modules retain ordinary identifier and .xr validation. */
    if (governed) {
        static const char logical[]="prelude/builtin_symbols.def";
        if (!xir_compile_work(context,sizeof(logical)+7)) return XR_XIR_BUDGET;
        return prefix==7 && length==sizeof(logical)-1 &&
            !memcmp(input->authority.namespace_id,"prelude",7) &&
            !memcmp(path,logical,sizeof(logical)-1) ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
    }
    if (prefix > length || length-prefix < 5) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,prefix+4)) return XR_XIR_BUDGET;
    if (memcmp(path,input->authority.namespace_id,prefix) || path[prefix] != '/' ||
        memcmp(path+length-3,".xr",3)) return XR_XIR_BAD_STRUCTURE;
    bool first = true;
    for (size_t i = prefix+1; i < length-3; ++i) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        char c = path[i];
        if (c == '/') { if (first) return XR_XIR_BAD_STRUCTURE; first = true; continue; }
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!letter && (first || c < '0' || c > '9')) return XR_XIR_BAD_STRUCTURE;
        first = false;
    }
    return first ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
}
static XrXirStatus catalog_io_status(XrOsIoStatus status) {
    switch (status) {
    case XR_OS_IO_OK: return XR_XIR_OK;
    case XR_OS_IO_BUDGET: return XR_XIR_BUDGET;
    case XR_OS_IO_OUT_OF_MEMORY: return XR_XIR_OUT_OF_MEMORY;
    case XR_OS_IO_NOT_FOUND: return XR_XIR_UNRESOLVED;
    case XR_OS_IO_BAD_ARGUMENT: return XR_XIR_BAD_STRUCTURE;
    default: return XR_XIR_IO;
    }
}
static XrXirStatus catalog_module(XrXirLibraryCatalog *catalog, size_t index,
    const XrXirLibraryModuleInput *input, XrXirArtifact *artifact, uint32_t ordinal) {
    const XrXirCompileContext *context = &catalog->context;
    if (!input->logical_path || !input->authority.physical_root) return XR_XIR_BAD_STRUCTURE;
    if (input->authority.kind != XR_MODULE_IDENTITY_SCRIPT &&
        input->authority.kind != XR_MODULE_IDENTITY_STDLIB) return XR_XIR_BAD_STAGE;
    XrModuleResourceBinding *resource = &catalog->resources[index];
    char *identity = NULL;
    XrXirStatus status = catalog_module_status(xr_compile_module_identity_from_logical(
        context->resources,&input->authority,input->logical_path,&identity));
    if (status != XR_XIR_OK) return status;
    resource->canonical = identity;
    size_t logical, root, name = 0, canonical;
    if (!catalog_length(context,input->logical_path,&logical) ||
        !catalog_length(context,input->authority.physical_root,&root) ||
        !catalog_length(context,identity,&canonical) ||
        (input->authority.namespace_id && !catalog_length(context,input->authority.namespace_id,&name)))
        return XR_XIR_BUDGET;
    if (!root || root >= XR_PATH_MAX) return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *checked=xr_xir_compile_artifact_module(artifact);bool governed=false;
    status=library_prelude_module(context,checked,ordinal,&governed);
    if (status!=XR_XIR_OK) return status;
    status = catalog_path(context,input,logical,name,governed);
    if (status != XR_XIR_OK) return status;
    const XrXirSourceModule *module = &checked->declarations->modules[ordinal];
    if (!xir_compile_work(context,canonical)) return XR_XIR_BUDGET;
    if (module->name_length != canonical || memcmp(module->name,identity,canonical)) return XR_XIR_BAD_STRUCTURE;
    resource->logical_path = catalog_text(context,input->logical_path,logical,&status);
    resource->authority.kind = input->authority.kind;
    resource->authority.physical_root = catalog_text(context,input->authority.physical_root,root,&status);
    if (input->authority.namespace_id)
        resource->authority.namespace_id = catalog_text(context,input->authority.namespace_id,name,&status);
    if (status != XR_XIR_OK) return status;
    XrOsIoPolicy policy = xr_compile_io_policy(context->resources);
    char *locator = NULL;
    status = catalog_io_status(xr_path_join_owned(&policy,input->authority.physical_root,input->logical_path,&locator));
    if (status != XR_XIR_OK) return status;
    resource->source_locator = locator;
    resource->checked = artifact; resource->checked_module = ordinal;
    return XR_XIR_OK;
}
static XrXirStatus catalog_artifact(XrXirLibraryCatalog *catalog, size_t index,
    const XrXirLibraryInput *input) {
    const XrXirCompileContext *context = &catalog->context;
    if (!input->packet || !input->modules || !input->module_count) return XR_XIR_BAD_STRUCTURE;
    if (input->module_count > UINT32_MAX) return XR_XIR_BUDGET;
    if (!xir_compile_work(context,input->length)) return XR_XIR_BUDGET;
    XrSHA256Context sha; uint8_t digest[32]; xr_sha256_init(&sha);
    xr_sha256_update(&sha,input->packet,input->length); xr_sha256_final(&sha,digest);
    if (!xir_compile_work(context,sizeof(digest))) return XR_XIR_BUDGET;
    if (memcmp(digest,input->sha256,sizeof(digest))) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = xr_xir_compile_checked_read(context,input->packet,input->length,&catalog->artifacts[index],NULL);
    if (status != XR_XIR_OK) return status;
    if (xr_xir_compile_artifact_context(catalog->artifacts[index])->resources != context->resources)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *module = xr_xir_compile_artifact_module(catalog->artifacts[index]);
    status = library_catalog_shape(module,xr_xir_compile_artifact_construction(catalog->artifacts[index]),context);
    if (status == XR_XIR_OK && module->declarations->module_count != input->module_count) return XR_XIR_BAD_STRUCTURE;
    return status;
}
/* Every edge addresses the same immutable artifact's ordinal table. The
 * Checked reader has already rejected dependency cycles and invalid IDs. */
static XrXirStatus catalog_dependencies(XrXirLibraryCatalog *catalog, size_t begin, size_t count) {
    const XrXirCompileContext *context = &catalog->context;
    for (size_t m = 0; m < count; ++m) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        XrModuleResourceBinding *resource = &catalog->resources[begin+m];
        const XrXirSourceModule *module = &xr_xir_compile_artifact_module(resource->checked)->declarations->modules[m];
        if (!module->dependency_count) continue;
        XrXirStatus status = XR_XIR_OK;
        const XrModuleResourceBinding **edges = xir_compile_calloc(context,module->dependency_count,sizeof(*edges),&status);
        if (!edges) return status;
        resource->dependencies = edges; resource->dependency_count = module->dependency_count;
        for (uint32_t e = 0; e < module->dependency_count; ++e) {
            if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
            if (module->dependencies[e] >= count) return XR_XIR_BAD_STRUCTURE;
            edges[e] = &catalog->resources[begin+module->dependencies[e]];
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus catalog_unique(const XrXirLibraryCatalog *catalog, size_t index) {
    const char *identity = catalog->resources[index].canonical;
    for (size_t prior = 0; prior < index; ++prior) {
        if (!xir_compile_work(&catalog->context,1)) return XR_XIR_BUDGET;
        const char *other = catalog->resources[prior].canonical;
        for (size_t byte = 0;; ++byte) {
            if (!xir_compile_work(&catalog->context,1)) return XR_XIR_BUDGET;
            if (identity[byte] != other[byte]) break;
            if (!identity[byte]) return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_library_catalog_new_v2(const XrXirCompileContext *context,
    const XrXirLibraryInput *inputs, size_t count, XrXirLibraryCatalog **output) {
    if (!xir_compile_context_valid(context) || !output || *output || !inputs || !count) return XR_XIR_BAD_STRUCTURE;
    if (count > SIZE_MAX/sizeof(XrModuleResourceBinding) || count > SIZE_MAX/sizeof(XrXirArtifact *))
        return XR_XIR_BUDGET;
    XrXirStatus status = XR_XIR_OK;
    XrXirLibraryCatalog *catalog = xir_compile_calloc(context,1,sizeof(*catalog),&status);
    if (!catalog) return status;
    catalog->context = *context;
    size_t modules = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!xir_compile_work(context,1)) { status = XR_XIR_BUDGET; goto failed; }
        if (!inputs[i].modules || !inputs[i].module_count) { status = XR_XIR_BAD_STRUCTURE; goto failed; }
        if (inputs[i].module_count > UINT32_MAX || inputs[i].module_count > SIZE_MAX - modules) {
            status = XR_XIR_BUDGET; goto failed;
        }
        modules += inputs[i].module_count;
    }
    if (modules > SIZE_MAX/sizeof(*catalog->resources)) { status = XR_XIR_BUDGET; goto failed; }
    catalog->resources = xir_compile_calloc(context,modules,sizeof(*catalog->resources),&status);
    catalog->artifacts = xir_compile_calloc(context,count,sizeof(*catalog->artifacts),&status);
    if (status != XR_XIR_OK) goto failed;
    catalog->artifact_count = count;
    size_t begin = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!xir_compile_work(context,1)) { status = XR_XIR_BUDGET; goto failed; }
        status = catalog_artifact(catalog,i,&inputs[i]);
        if (status != XR_XIR_OK) goto failed;
        for (size_t m = 0; m < inputs[i].module_count; ++m) {
            if (!xir_compile_work(context,1)) { status = XR_XIR_BUDGET; goto failed; }
            catalog->count = begin+m+1;
            status = catalog_module(catalog,begin+m,&inputs[i].modules[m],catalog->artifacts[i],(uint32_t)m);
            if (status != XR_XIR_OK) goto failed;
            status = catalog_unique(catalog,begin+m);
            if (status != XR_XIR_OK) goto failed;
        }
        status = catalog_dependencies(catalog,begin,inputs[i].module_count);
        if (status != XR_XIR_OK) goto failed;
        begin += inputs[i].module_count;
    }
    *output = catalog;
    return XR_XIR_OK;
failed:
    xr_xir_compile_library_catalog_free(catalog);
    return status;
}
