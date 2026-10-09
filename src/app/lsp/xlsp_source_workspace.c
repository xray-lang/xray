/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xlsp_source_workspace.h"
#include "../cli/xcli_source_paths.h"
#include "../cli/xcli_graph_authority.h"
#include "../../xir/xxir_native_cache_internal.h"
#include "../../xir/xxir_library_catalog.h"
#include <string.h>
struct XlspSourceWorkspace {
    XrXirCompileContext context;
    XrCompilerSession *session;
    XrCliStdlibPath stdlib;
    XrXirLibraryCatalog *catalog;
    XrCliGraphAuthority **authorities;
    XlspSourceSnapshot **snapshots;
    size_t count;
};
static XrXirStatus workspace_resource(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:
        status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
void xlsp_source_workspace_free(XlspSourceWorkspace *w) {
    if(!w)return;
    for(size_t i=0;i<w->count;++i) {
        if(w->snapshots)xlsp_source_snapshot_free(w->snapshots[i]);
        if(w->authorities)xr_cli_compile_graph_authority_close(w->authorities[i]);
    }
    xr_compile_resources_free(w->snapshots);xr_compile_resources_free(w->authorities);
    xr_compile_session_free(w->session);
    xr_xir_compile_library_catalog_free(w->catalog);
    xr_cli_compile_stdlib_path_free(&w->stdlib);
    xr_compile_resources_free(w);
}
XrXirStatus xlsp_source_workspace_build(const XrXirCompileContext *context,
    const XlspSourceDocument *documents,size_t count,XlspSourceWorkspace **output,unsigned *stage,XlspSourceWorkspaceFailure *failure) {
    if(!context||!context->resources||!documents||!count||!output||*output||(failure&&(failure->present||failure->path))||
        count>SIZE_MAX/sizeof(XrModuleIdentityAuthority))return XR_XIR_BAD_STRUCTURE;
    XlspSourceWorkspace *w=NULL;XrModuleIdentityAuthority *identities=NULL;char *path=NULL;
    XrXirStatus status=workspace_resource(xr_compile_resources_calloc(context->resources,1,sizeof(*w),(void **)&w));
    if(stage)*stage=1;
    if(status!=XR_XIR_OK)return status;
    w->context=*context;w->count=count;
    status=workspace_resource(xr_compile_resources_calloc(context->resources,count,sizeof(*w->authorities),(void **)&w->authorities));
    if(status!=XR_XIR_OK)goto done;
    status=workspace_resource(xr_compile_resources_calloc(context->resources,count,sizeof(*w->snapshots),(void **)&w->snapshots));
    if(status!=XR_XIR_OK)goto done;
    status=workspace_resource(xr_compile_resources_calloc(context->resources,count,sizeof(*identities),(void **)&identities));
    if(status!=XR_XIR_OK)goto done;
    if(stage)*stage=2;
    XrCliCompileSourceStatus selected=xr_cli_compile_stdlib_path(context->resources,&w->stdlib,NULL);
    if(selected!=XR_CLI_COMPILE_SOURCE_OK) {
        status=selected==XR_CLI_COMPILE_SOURCE_BUDGET?XR_XIR_BUDGET:
            selected==XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
            selected==XR_CLI_COMPILE_SOURCE_NOT_FOUND?XR_XIR_UNRESOLVED:XR_XIR_IO;
        goto done;
    }
    if(stage)*stage=3;
    status=xir_native_cache_library_catalog_new(context,w->stdlib.path,&w->catalog);
    if(status!=XR_XIR_OK)goto done;
    if(stage)*stage=4;
    XrTomlParseLimits limits=xr_cli_compile_default_manifest_limits();
    for(size_t i=0;i<count;++i) {
        status=workspace_resource(xr_compile_resources_work(context->resources,sizeof(documents[i])));
        if(status!=XR_XIR_OK)goto done;
        status=xlsp_source_uri_path(context->resources,documents[i].uri,documents[i].uri_length,&path);
        if(status!=XR_XIR_OK)goto done;
        XrCliGraphEntryInput input={XR_CLI_GRAPH_ENTRY_TEXT,path,documents[i].text,documents[i].length};
        XrManifestStatus admitted=xr_cli_compile_graph_authority_open_input(context,&input,w->catalog,&limits,&w->authorities[i],NULL);
        xr_compile_resources_free(path);path=NULL;
        if(admitted!=XR_MANIFEST_OK) {
            status=admitted==XR_MANIFEST_BUDGET?XR_XIR_BUDGET:
                admitted==XR_MANIFEST_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
                admitted==XR_MANIFEST_IO?XR_XIR_IO:XR_XIR_BAD_STRUCTURE;
            goto done;
        }
        status=workspace_resource(xr_compile_resources_work(context->resources,sizeof(identities[i])));
        if(status!=XR_XIR_OK)goto done;
        identities[i]=*xr_cli_compile_graph_authority_entry(w->authorities[i]);
    }
    if(stage)*stage=5;
    XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&w->session);
    if(opened!=XR_COMPILER_SESSION_OK) {
        status=opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:
            opened==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
        goto done;
    }
    if(stage)*stage=6;
    for(size_t i=0;i<count;++i) {
        XrXirSourceRequest request={w->session,xr_cli_compile_graph_authority_source_path(w->authorities[i]),
            &identities[i],&w->context,w->stdlib.path,xr_cli_compile_graph_authority_lockfile(w->authorities[i]),
            XR_XIR_PROGRAM,w->catalog};
        XrXirSourceDiagnostic diagnostic={0};char *failure_path=NULL;
        status=xlsp_source_snapshot_build_authorities(&request,documents,identities,count,i,&w->snapshots[i],&diagnostic,&failure_path);
        if(status!=XR_XIR_OK) {
            if(failure&&diagnostic.status==status&&diagnostic.message[0]&&
                status!=XR_XIR_BUDGET&&status!=XR_XIR_OUT_OF_MEMORY&&status!=XR_XIR_IO) {
                XrXirStatus copied=workspace_resource(xr_compile_resources_work(context->resources,sizeof(*failure)));
                if(copied==XR_XIR_OK){*failure=(XlspSourceWorkspaceFailure){true,diagnostic,failure_path};failure_path=NULL;}
                else status=copied;
            }
            xr_compile_resources_free(failure_path);goto done;
        }
        xr_compile_resources_free(failure_path);
    }
    *output=w;w=NULL;if(stage)*stage=0;
done:
    xr_compile_resources_free(path);xr_compile_resources_free(identities);
    xlsp_source_workspace_free(w);return status;
}
XrXirStatus xlsp_source_workspace_navigation(XlspSourceWorkspace *w,const char *uri,size_t length,
    XrLspPosition position,unsigned mode,bool include_declaration,XrJsonValue **out) {
    if(!w)return XR_XIR_BAD_STRUCTURE;
    if(mode==3)return xlsp_source_hover_many_json(w->context.resources,w->snapshots,w->count,uri,length,position,out);
    return xlsp_source_navigation_many_json(w->context.resources,w->snapshots,w->count,
        uri,length,position,mode,include_declaration,out);
}

void xlsp_source_workspace_failure_free(XlspSourceWorkspaceFailure *failure) {
    if(!failure)return;xr_compile_resources_free(failure->path);*failure=(XlspSourceWorkspaceFailure){0};
}

XrXirStatus xlsp_source_workspace_tokens(XlspSourceWorkspace *w,const char *uri,size_t length,
    const XrLspRange *range,XlspSourceTokens **out) {
    if(!w||!uri||length==SIZE_MAX||!out||*out)return XR_XIR_BAD_STRUCTURE;
    XlspSourceTokens *accepted=NULL,*current=NULL;XrXirStatus status=XR_XIR_OK;
    for(size_t i=0;i<w->count;++i) {
        bool contains=false;status=xlsp_source_snapshot_contains(w->snapshots[i],uri,length,&contains);
        if(status!=XR_XIR_OK)goto done;if(!contains)continue;
        if(range){status=xlsp_source_semantic_range(w->snapshots[i],uri,length,*range);if(status!=XR_XIR_OK)goto done;}
        status=xlsp_source_semantic_tokens(w->snapshots[i],uri,length,&current);if(status!=XR_XIR_OK)goto done;
        if(accepted) {
            size_t a=0,b=0;const XlspSourceToken *x=xlsp_source_tokens_items(accepted,&a),*y=xlsp_source_tokens_items(current,&b);
            if(a!=b){status=XR_XIR_BAD_STRUCTURE;goto done;}
            for(size_t n=0;n<a;++n) {
                if(xr_compile_resources_work(w->context.resources,10)!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
                if(x[n].line!=y[n].line||x[n].column!=y[n].column||x[n].length!=y[n].length||x[n].type!=y[n].type||x[n].modifiers!=y[n].modifiers){status=XR_XIR_BAD_STRUCTURE;goto done;}
            }
            xlsp_source_tokens_free(current);current=NULL;
        } else {accepted=current;current=NULL;}
    }
    if(!accepted){status=XR_XIR_UNRESOLVED;goto done;}*out=accepted;accepted=NULL;
done:
    xlsp_source_tokens_free(current);xlsp_source_tokens_free(accepted);return status;
}
