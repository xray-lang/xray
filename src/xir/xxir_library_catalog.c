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
#include "xxir_declarations.h"
#include "xxir_internal.h"
#include "../base/xmalloc.h"
#include "../base/xsha256.h"
#include "../os/os_fs.h"
struct XrXirLibraryCatalog { XrModuleResourceBinding resource; XrXirArtifact *artifact; };
XR_FUNC void xr_xir_library_catalog_free(XrXirLibraryCatalog *catalog) {
    if (!catalog) return;
    xr_xir_artifact_free(catalog->artifact);
    xr_free((void *)catalog->resource.canonical);xr_free((void *)catalog->resource.logical_path);
    xr_free((void *)catalog->resource.source_locator);xr_free((void *)catalog->resource.authority.namespace_id);
    xr_free((void *)catalog->resource.authority.physical_root);xr_free(catalog);
}
XR_FUNC const XrModuleResourceBinding *xr_xir_library_catalog_resource(const XrXirLibraryCatalog *catalog) {
    return catalog ? &catalog->resource : NULL;
}
static char *library_catalog_text(const char *text) {
    size_t n=strlen(text)+1;char *copy=xr_malloc(n);if(copy)memcpy(copy,text,n);return copy;
}
static XrXirStatus library_catalog_shape(const XrXirModule *m, const char *identity, XrXirBudget *budget) {
    const XrXirDeclarations *d=m->declarations;
    if(m->linkage_kind!=XR_XIR_LIBRARY || m->stage!=XR_XIR_CHECKED) return XR_XIR_BAD_STAGE;
    if(!d || d->module_count!=1 || !d->modules || m->types || m->provenance ||
        d->slots || d->slot_count || d->literals || d->literal_count || d->implementations ||
        d->modules[0].dependency_count) return XR_XIR_BAD_STAGE;
    if(d->modules[0].name_length!=strlen(identity)||memcmp(d->modules[0].name,identity,strlen(identity)))
        return XR_XIR_BAD_STRUCTURE;
    for(uint32_t f=0;f<m->function_count;++f){
        if (!budget->work) return XR_XIR_BUDGET; --budget->work;
        const XrXirFunction *fn=&m->functions[f];
        if(fn->parameter_count || (fn->result!=XR_XIR_UNIT&&fn->result!=XR_XIR_I64) ||
            (m->generics&&m->generics[f].parameter_count) || d->functions[f].nominal_owner ||
            d->functions[f].cleanup_owner || d->functions[f].method_kind!=XR_XIR_NON_MEMBER)
            return XR_XIR_BAD_STAGE;
        for(uint32_t i=0;i<fn->instruction_count;++i){
            if (!budget->work) return XR_XIR_BUDGET; --budget->work;
            const XrXirInstruction *op=&fn->instructions[i];
            if(op->op!=XR_XIR_CALL&&op->op!=XR_XIR_RETURN&&op->op!=XR_XIR_CONST_INT&&op->op!=XR_XIR_ADD_INT)
                return XR_XIR_BAD_STAGE;
        }
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_library_catalog_new(const XrXirLibraryInput *input,
    const XrXirBudget *budget, XrXirLibraryCatalog **output) {
    if(!output)return XR_XIR_BAD_STRUCTURE;*output=NULL;
    if(!input||!input->packet||!input->logical_path||!input->authority.physical_root ||
        !xr_module_identity_authority_valid(&input->authority))return XR_XIR_BAD_STRUCTURE;
    /* This slice accepts file-backed script authority only; package publication is not authenticated here. */
    if (input->authority.kind != XR_MODULE_IDENTITY_SCRIPT) return XR_XIR_BAD_STAGE;
    size_t logical=strlen(input->logical_path),root=strlen(input->authority.physical_root);
    if (!logical || logical>=XR_PATH_MAX || root>=XR_PATH_MAX || strchr(input->logical_path,'/') ||
        strchr(input->logical_path,'\\') || !strcmp(input->logical_path,".") || !strcmp(input->logical_path,"..")) return XR_XIR_BAD_STRUCTURE;
    int canonical=snprintf(NULL,0,"module-id-v1:kind=6:script:namespace=0::path=%zu:%s",logical,input->logical_path);
    if(canonical<0)return XR_XIR_BAD_STRUCTURE;
    size_t text_bytes=(size_t)canonical+1+logical+1+root+1+root+logical+2+(input->authority.namespace_id?1:0);
    XrXirBudget limits=budget?*budget:xr_xir_default_budget();
    if(text_bytes>limits.metadata_bytes || text_bytes>limits.work)return XR_XIR_BUDGET;
    limits.metadata_bytes-=text_bytes;limits.work-=text_bytes;
    if(input->length>limits.work)return XR_XIR_BUDGET;limits.work-=input->length;
    XrSHA256Context sha;uint8_t digest[32];xr_sha256_init(&sha);
    xr_sha256_update(&sha,input->packet,input->length);xr_sha256_final(&sha,digest);
    if(memcmp(digest,input->sha256,32))return XR_XIR_BAD_STRUCTURE;
    if(sizeof(XrXirLibraryCatalog)>limits.metadata_bytes)return XR_XIR_BUDGET;
    limits.metadata_bytes-=sizeof(XrXirLibraryCatalog);
    XrXirLibraryCatalog *catalog=xr_calloc(1,sizeof(*catalog));if(!catalog)return XR_XIR_OUT_OF_MEMORY;
    XrXirStatus status=XR_XIR_OUT_OF_MEMORY;char *identity=NULL;
    if(!xr_module_identity_from_logical(&input->authority,input->logical_path,&identity))goto done;
    catalog->resource.canonical=identity;catalog->resource.logical_path=library_catalog_text(input->logical_path);
    catalog->resource.authority.kind=input->authority.kind;
    catalog->resource.authority.physical_root=library_catalog_text(input->authority.physical_root);
    catalog->resource.authority.namespace_id=input->authority.namespace_id?library_catalog_text(input->authority.namespace_id):NULL;
    size_t n=strlen(input->authority.physical_root)+strlen(input->logical_path)+2;
    char *locator=xr_malloc(n);catalog->resource.source_locator=locator;
    if(!locator||!catalog->resource.logical_path||!catalog->resource.authority.physical_root ||
        (input->authority.namespace_id&&!catalog->resource.authority.namespace_id))goto done;
    snprintf(locator,n,"%s/%s",input->authority.physical_root,input->logical_path);
    status=xr_xir_checked_read_remaining(input->packet,input->length,&limits,&catalog->artifact,NULL);
    if(status!=XR_XIR_OK)goto done;
    status=library_catalog_shape(xr_xir_artifact_module(catalog->artifact),identity,&limits);
    if(status!=XR_XIR_OK)goto done;
    catalog->resource.checked=catalog->artifact;*output=catalog;return XR_XIR_OK;
done:
    xr_xir_library_catalog_free(catalog);return status;
}
