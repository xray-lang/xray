/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_bindings.c - Own immutable canonical Source binding observations
 *
 * KEY CONCEPT:
 *   Canonical Source owns binding semantics. Consumers retain exact owned facts
 *   and reject unavailable evidence without publishing partial edits.
 */
#include "xlsp_source_bindings.h"
#include "../../api/xsource_paths.h"
#include "../../api/xsource_authority.h"
#include "../../xir/xxir_stdlib_internal.h"
#include "../../xir/xxir_library_catalog.h"
#include <string.h>

struct XlspSourceBindings {XrXirSourceSnapshot **snapshots;char **paths;size_t count;};
static XrXirStatus bindings_resource(XrCompileResourceStatus s) {
    return s==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:s==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        s==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
XR_FUNC void xlsp_source_bindings_free(XlspSourceBindings *owner) {
    if(!owner)return;
    if(owner->snapshots||owner->paths)for(size_t i=0;i<owner->count;++i){if(owner->snapshots)xr_xir_compile_source_snapshot_free(owner->snapshots[i]);if(owner->paths)xr_compile_resources_free(owner->paths[i]);}
    xr_compile_resources_free(owner->snapshots);xr_compile_resources_free(owner->paths);xr_compile_resources_free(owner);
}
XR_FUNC size_t xlsp_source_bindings_count(const XlspSourceBindings *owner){return owner?owner->count:0;}
XR_FUNC const XrXirSourceView *xlsp_source_bindings_view(const XlspSourceBindings *owner,size_t index){return owner&&index<owner->count?xr_xir_compile_source_snapshot_view(owner->snapshots[index]):NULL;}
XR_FUNC const XrXirSourceSyntaxView *xlsp_source_bindings_syntax(const XlspSourceBindings *owner,size_t index){return owner&&index<owner->count?xr_xir_compile_source_snapshot_syntax(owner->snapshots[index]):NULL;}
XR_FUNC const XrXirSourceCalls *xlsp_source_bindings_calls(const XlspSourceBindings *owner,size_t index){return owner&&index<owner->count?xr_xir_compile_source_snapshot_calls(owner->snapshots[index]):NULL;}
XR_FUNC const char *xlsp_source_bindings_path(const XlspSourceBindings *owner,size_t index){return owner&&index<owner->count?owner->paths[index]:NULL;}
static XrXirStatus bindings_manifest(XrManifestStatus s) {
    return s==XR_MANIFEST_OK?XR_XIR_OK:s==XR_MANIFEST_BUDGET?XR_XIR_BUDGET:s==XR_MANIFEST_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
        s==XR_MANIFEST_IO?XR_XIR_IO:s==XR_MANIFEST_NOT_FOUND?XR_XIR_UNRESOLVED:s==XR_MANIFEST_UNSUPPORTED?XR_XIR_UNSUPPORTED:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus bindings_path_copy(XrCompileResources *r,const char *text,char **out) {
    size_t length=0;for(;;++length){XrXirStatus s=bindings_resource(xr_compile_resources_work(r,1));if(s!=XR_XIR_OK)return s;if(!text[length])break;if(length==SIZE_MAX-1)return XR_XIR_BUDGET;}
    XrXirStatus s=bindings_resource(xr_compile_resources_alloc(r,length+1,(void **)out));
    if(s==XR_XIR_OK)s=bindings_resource(xr_compile_resources_work(r,length+1));if(s==XR_XIR_OK)memcpy(*out,text,length+1);return s;
}
XR_FUNC XrXirStatus xlsp_source_bindings_build(const XrXirCompileContext *context,const XlspSourceDocument *documents,size_t count,XlspSourceBindings **out) {
    if(!context||!context->resources||!documents||!count||count>SIZE_MAX/sizeof(XrModuleOverlayInput)||!out||*out)return XR_XIR_BAD_STRUCTURE;
    XrCompileResources *r=context->resources;XlspSourceBindings *owner=NULL;XrSourceAuthority **authorities=NULL;
    XrModuleOverlayInput *overlays=NULL;XrStdlibPath stdlib={0};XrXirLibraryCatalog *catalog=NULL;XrCompilerSession *session=NULL;
    XrXirStatus status=bindings_resource(xr_compile_resources_calloc(r,1,sizeof(*owner),(void **)&owner));if(status!=XR_XIR_OK)goto done;owner->count=count;
    status=bindings_resource(xr_compile_resources_calloc(r,count,sizeof(*owner->snapshots),(void **)&owner->snapshots));
    if(status==XR_XIR_OK)status=bindings_resource(xr_compile_resources_calloc(r,count,sizeof(*owner->paths),(void **)&owner->paths));
    if(status==XR_XIR_OK)status=bindings_resource(xr_compile_resources_calloc(r,count,sizeof(*authorities),(void **)&authorities));
    if(status==XR_XIR_OK)status=bindings_resource(xr_compile_resources_calloc(r,count,sizeof(*overlays),(void **)&overlays));if(status!=XR_XIR_OK)goto done;
    XrSourceCompileStatus selected=xr_source_stdlib_path(r,&stdlib,NULL);
    if(selected!=XR_SOURCE_COMPILE_OK){status=selected==XR_SOURCE_COMPILE_BUDGET?XR_XIR_BUDGET:selected==XR_SOURCE_COMPILE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:selected==XR_SOURCE_COMPILE_NOT_FOUND?XR_XIR_UNRESOLVED:XR_XIR_IO;goto done;}
    status=xir_stdlib_library_catalog_new(context,stdlib.path,&catalog);if(status!=XR_XIR_OK)goto done;
    XrTomlParseLimits limits=xr_source_compile_default_manifest_limits();
    for(size_t i=0;i<count;++i) {
        char *path=NULL;status=bindings_resource(xr_compile_resources_work(r,sizeof(documents[i])));if(status!=XR_XIR_OK)goto done;
        status=xlsp_source_uri_path(r,documents[i].uri,documents[i].uri_length,&path);if(status!=XR_XIR_OK)goto done;
        XrSourceEntryInput input={XR_SOURCE_ENTRY_TEXT,path,documents[i].text,documents[i].length};
        status=bindings_manifest(xr_source_authority_open_input(context,&input,catalog,&limits,&authorities[i],NULL));xr_compile_resources_free(path);if(status!=XR_XIR_OK)goto done;
        const XrModuleOverlayInput *overlay=xr_source_authority_overlay(authorities[i]);if(!overlay){status=XR_XIR_BAD_STRUCTURE;goto done;}
        status=bindings_resource(xr_compile_resources_work(r,sizeof(*overlay)));if(status!=XR_XIR_OK)goto done;overlays[i]=*overlay;
        status=bindings_path_copy(r,xr_source_authority_source_path(authorities[i]),&owner->paths[i]);if(status!=XR_XIR_OK)goto done;
    }
    XrCompilerSessionStatus opened=xr_compile_session_new(r,&session);
    if(opened!=XR_COMPILER_SESSION_OK){status=opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:opened==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;goto done;}
    for(size_t i=0;i<count;++i) {
        XrXirSourceRequest request={session,xr_source_authority_source_path(authorities[i]),xr_source_authority_entry(authorities[i]),context,
            stdlib.path,xr_source_authority_lockfile(authorities[i]),XR_XIR_PROGRAM,catalog};
        XrXirSourceResult result={0};status=xr_xir_compile_source_check_overlay(&request,overlays,count,&result,NULL,NULL);
        if(status==XR_XIR_OK) {
            const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);const XrXirSourceSyntaxView *syntax=xr_xir_compile_source_snapshot_syntax(result.snapshot);
            if(!view||!view->complete||!syntax||syntax->declaration_count!=view->declaration_count||syntax->reference_count!=view->reference_count)status=XR_XIR_UNRESOLVED;
            else {owner->snapshots[i]=result.snapshot;result.snapshot=NULL;}
        }
        xr_xir_compile_source_result_free(&result);if(status!=XR_XIR_OK)goto done;
    }
    *out=owner;owner=NULL;
done:
    xr_compile_session_free(session);if(authorities)for(size_t i=0;i<count;++i)xr_source_authority_close(authorities[i]);
    xr_compile_resources_free(authorities);xr_compile_resources_free(overlays);xr_xir_compile_library_catalog_free(catalog);xr_source_stdlib_path_free(&stdlib);
    xlsp_source_bindings_free(owner);return status;
}
