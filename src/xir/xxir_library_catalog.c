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
#include "../base/xsha256.h"
#include "../base/xio_policy.h"
#include "../os/os_fs.h"
struct XrXirLibraryCatalog {
    XrXirCompileContext context;
    XrModuleResourceBinding *resources;
    XrXirArtifact **artifacts;
    size_t count;
};
XR_FUNC void xr_xir_compile_library_catalog_free(XrXirLibraryCatalog *catalog) {
    if (!catalog) return;
    for (size_t i = 0; i < catalog->count; ++i) {
        XrModuleResourceBinding *resource = &catalog->resources[i];
        xr_xir_compile_artifact_free(catalog->artifacts[i]);
        xr_compile_resources_free((void *)resource->canonical);
        xr_compile_resources_free((void *)resource->logical_path);
        xr_compile_resources_free((void *)resource->source_locator);
        xr_compile_resources_free((void *)resource->authority.namespace_id);
        xr_compile_resources_free((void *)resource->authority.physical_root);
    }
    xr_compile_resources_free(catalog->resources);
    xr_compile_resources_free(catalog->artifacts);
    xr_compile_resources_free(catalog);
}
XR_FUNC const XrModuleResourceBinding *xr_xir_compile_library_catalog_resources(
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
static XrXirStatus library_catalog_shape(const XrXirModule *m, const char *identity, size_t identity_length, const XrXirCompileContext *context) {
    const XrXirDeclarations *d=m->declarations;
    if(m->linkage_kind!=XR_XIR_LIBRARY || m->stage!=XR_XIR_CHECKED) return XR_XIR_BAD_STAGE;
    if(!d || d->module_count!=1 || !d->modules || m->types || m->provenance ||
        d->slots || d->slot_count || d->implementations ||
        d->modules[0].dependency_count) return XR_XIR_BAD_STAGE;
    if (!xir_compile_work(context, identity_length)) return XR_XIR_BUDGET;
    if(d->modules[0].name_length!=identity_length||memcmp(d->modules[0].name,identity,identity_length))
        return XR_XIR_BAD_STRUCTURE;
    for(uint32_t f=0;f<m->function_count;++f){
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirFunction *fn=&m->functions[f];
        if((fn->result!=XR_XIR_UNIT&&fn->result!=XR_XIR_I64&&fn->result!=XR_XIR_STRING&&fn->result!=XR_XIR_BOOL) ||
            (m->generics&&m->generics[f].parameter_count) || d->functions[f].nominal_owner ||
            d->functions[f].cleanup_owner || d->functions[f].method_kind!=XR_XIR_NON_MEMBER)
            return XR_XIR_BAD_STAGE;
        for (uint32_t p = 0; p < fn->parameter_count; ++p) {
            if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
            if (fn->parameters[p] != XR_XIR_I64 && fn->parameters[p] != XR_XIR_STRING &&
                fn->parameters[p] != XR_XIR_BOOL)
                return XR_XIR_BAD_STAGE;
        }
        for(uint32_t i=0;i<fn->instruction_count;++i){
            if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
            const XrXirInstruction *op=&fn->instructions[i];
            if(op->op!=XR_XIR_CALL&&op->op!=XR_XIR_CALL_DEFAULT&&op->op!=XR_XIR_RETURN&&op->op!=XR_XIR_CONST_INT&&op->op!=XR_XIR_ADD_INT&&
                op->op!=XR_XIR_CONST_STRING&&op->op!=XR_XIR_CONCAT_STRING&&
                op->op!=XR_XIR_CONST_BOOL&&op->op!=XR_XIR_WRITE_STREAM)
                return XR_XIR_BAD_STAGE;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus catalog_path(const XrXirCompileContext *context,
    const XrXirLibraryInput *input, size_t length, size_t prefix) {
    const char *path = input->logical_path;
    if (!length || length >= XR_PATH_MAX) return XR_XIR_BAD_STRUCTURE;
    if (input->authority.kind == XR_MODULE_IDENTITY_SCRIPT) {
        for (size_t i = 0; i < length; ++i) {
            if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
            if (path[i] == '/') return XR_XIR_BAD_STRUCTURE;
        }
        return XR_XIR_OK;
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
static XrXirStatus catalog_item(XrXirLibraryCatalog *catalog, size_t index,
    const XrXirLibraryInput *input) {
    const XrXirCompileContext *context = &catalog->context;
    if (!input->packet || !input->logical_path || !input->authority.physical_root)
        return XR_XIR_BAD_STRUCTURE;
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
    status = catalog_path(context,input,logical,name);
    if (status != XR_XIR_OK) return status;
    if (!xir_compile_work(context,input->length)) return XR_XIR_BUDGET;
    XrSHA256Context sha; uint8_t digest[32]; xr_sha256_init(&sha);
    xr_sha256_update(&sha,input->packet,input->length); xr_sha256_final(&sha,digest);
    if (!xir_compile_work(context,sizeof(digest))) return XR_XIR_BUDGET;
    if (memcmp(digest,input->sha256,sizeof(digest))) return XR_XIR_BAD_STRUCTURE;
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
    status = xr_xir_compile_checked_read(context,input->packet,input->length,&catalog->artifacts[index],NULL);
    if (status != XR_XIR_OK) return status;
    if (xr_xir_compile_artifact_context(catalog->artifacts[index])->resources != context->resources)
        return XR_XIR_BAD_STRUCTURE;
    status = library_catalog_shape(xr_xir_compile_artifact_module(catalog->artifacts[index]),identity,canonical,context);
    if (status == XR_XIR_OK) resource->checked = catalog->artifacts[index];
    return status;
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
XR_FUNC XrXirStatus xr_xir_compile_library_catalog_new(const XrXirCompileContext *context,
    const XrXirLibraryInput *inputs, size_t count, XrXirLibraryCatalog **output) {
    if (!xir_compile_context_valid(context) || !output || !inputs || !count) return XR_XIR_BAD_STRUCTURE;
    if (count > SIZE_MAX/sizeof(XrModuleResourceBinding) || count > SIZE_MAX/sizeof(XrXirArtifact *))
        return XR_XIR_BUDGET;
    XrXirStatus status = XR_XIR_OK;
    XrXirLibraryCatalog *catalog = xir_compile_calloc(context,1,sizeof(*catalog),&status);
    if (!catalog) return status;
    catalog->context = *context;
    catalog->resources = xir_compile_calloc(context,count,sizeof(*catalog->resources),&status);
    catalog->artifacts = xir_compile_calloc(context,count,sizeof(*catalog->artifacts),&status);
    if (status != XR_XIR_OK) goto failed;
    for (size_t i = 0; i < count; ++i) {
        if (!xir_compile_work(context,1)) { status = XR_XIR_BUDGET; goto failed; }
        catalog->count = i+1;
        status = catalog_item(catalog,i,&inputs[i]);
        if (status != XR_XIR_OK) goto failed;
        status = catalog_unique(catalog,i);
        if (status != XR_XIR_OK) goto failed;
    }
    *output = catalog;
    return XR_XIR_OK;
failed:
    xr_xir_compile_library_catalog_free(catalog);
    return status;
}
