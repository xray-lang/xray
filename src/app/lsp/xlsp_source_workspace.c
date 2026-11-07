/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xlsp_source_workspace_internal.h"
#include "xlsp_source_signature.h"
#include "xlsp_source_inlay.h"
#include "../../api/xsource_paths.h"
#include "../../api/xsource_authority.h"
#include "../../xir/xxir_compile_memory.h"
#include <string.h>
#include <limits.h>
#include "xlsp_source_inlay_workspace.inc.c"
void xlsp_source_workspace_free(XlspSourceWorkspace *w) {
    if(!w)return;
    for(size_t i=0;i<w->count;++i) {
        if(w->snapshots)xlsp_source_snapshot_free(w->snapshots[i]);
        if(w->authorities)xr_source_authority_close(w->authorities[i]);
    }
    xr_compile_resources_free(w->snapshots);xr_compile_resources_free(w->authorities);
    xr_compile_session_free(w->session);
    xr_source_stdlib_path_free(&w->stdlib);
    xr_compile_resources_free(w);
}
XrXirStatus xlsp_source_workspace_build(const XrXirCompileContext *context,
    const XlspSourceDocument *documents,size_t count,XlspSourceWorkspace **output,unsigned *stage,XlspSourceWorkspaceFailure *failure,XrCompileRequestControl *control) {
    if((control&&!control->initialized)||!context||!context->resources||!documents||!count||!output||*output||(failure&&(failure->present||!xr_xir_compile_source_failure_empty(&failure->source)))||
        count>SIZE_MAX/sizeof(XrModuleIdentityAuthority))return XR_XIR_BAD_STRUCTURE;
    XlspSourceWorkspace *w=NULL;XrModuleIdentityAuthority *identities=NULL;char *path=NULL;
    XrXirStatus status=XR_XIR_OK;
    if(stage)*stage=1;
    if(control){status=xir_compile_resource_status(xr_compile_request_control_poll(control));if(status!=XR_XIR_OK)return status;}
    status=xir_compile_resource_status(xr_compile_resources_calloc(context->resources,1,sizeof(*w),(void **)&w));
    if(status!=XR_XIR_OK)return status;
    w->context=*context;w->count=count;
    status=xir_compile_resource_status(xr_compile_resources_calloc(context->resources,count,sizeof(*w->authorities),(void **)&w->authorities));
    if(status!=XR_XIR_OK)goto done;
    status=xir_compile_resource_status(xr_compile_resources_calloc(context->resources,count,sizeof(*w->snapshots),(void **)&w->snapshots));
    if(status!=XR_XIR_OK)goto done;
    status=xir_compile_resource_status(xr_compile_resources_calloc(context->resources,count,sizeof(*identities),(void **)&identities));
    if(status!=XR_XIR_OK)goto done;
    if(stage)*stage=2;
    if(control){status=xir_compile_resource_status(xr_compile_request_control_poll(control));if(status!=XR_XIR_OK)goto done;}
    XrSourceCompileStatus selected=xr_source_stdlib_path(context->resources,&w->stdlib,NULL);
    if(selected!=XR_SOURCE_COMPILE_OK) {
        switch(selected) {
        case XR_SOURCE_COMPILE_BUDGET: status=XR_XIR_BUDGET;break;
        case XR_SOURCE_COMPILE_MEMORY_LIMIT: status=XR_XIR_MEMORY_LIMIT;break;
        case XR_SOURCE_COMPILE_OVERFLOW: status=XR_XIR_OVERFLOW;break;
        case XR_SOURCE_COMPILE_CANCELLED: status=XR_XIR_CANCELLED;break;
        case XR_SOURCE_COMPILE_DEADLINE: status=XR_XIR_DEADLINE;break;
        case XR_SOURCE_COMPILE_CLOCK_FAILURE: status=XR_XIR_CLOCK_FAILURE;break;
        case XR_SOURCE_COMPILE_OUT_OF_MEMORY: status=XR_XIR_OUT_OF_MEMORY;break;
        case XR_SOURCE_COMPILE_NOT_FOUND: status=XR_XIR_UNRESOLVED;break;
        default: status=XR_XIR_IO;break;
        }
        goto done;
    }
    if(stage)*stage=3;
    if(control){status=xir_compile_resource_status(xr_compile_request_control_poll(control));if(status!=XR_XIR_OK)goto done;}
    if(stage)*stage=4;
    if(control){status=xir_compile_resource_status(xr_compile_request_control_poll(control));if(status!=XR_XIR_OK)goto done;}
    XrTomlParseLimits limits=xr_source_compile_default_manifest_limits();
    for(size_t i=0;i<count;++i) {
        if(control){status=xir_compile_resource_status(xr_compile_request_control_poll(control));if(status!=XR_XIR_OK)goto done;}
        status=xlsp_source_uri_path(context->resources,documents[i].uri,documents[i].uri_length,&path);
        if(status!=XR_XIR_OK)goto done;
        XrSourceEntryInput input={XR_SOURCE_ENTRY_TEXT,path,documents[i].text,documents[i].length};
        XrManifestStatus admitted=xr_source_authority_open_input(context,&input,NULL,&limits,&w->authorities[i],NULL);
        xr_compile_resources_free(path);path=NULL;
        if(admitted!=XR_MANIFEST_OK) {
            status=admitted==XR_MANIFEST_MEMORY_LIMIT?XR_XIR_MEMORY_LIMIT:
                admitted==XR_MANIFEST_OVERFLOW?XR_XIR_OVERFLOW:
                admitted==XR_MANIFEST_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
                admitted==XR_MANIFEST_IO?XR_XIR_IO:XR_XIR_BAD_STRUCTURE;
            goto done;
        }
        identities[i]=*xr_source_authority_entry(w->authorities[i]);
    }
    if(stage)*stage=5;
    if(control){status=xir_compile_resource_status(xr_compile_request_control_poll(control));if(status!=XR_XIR_OK)goto done;}
    XrCompileResourceStatus opened=xr_compile_session_new(context->resources,&w->session);
    status=xir_compile_resource_status(opened);if(status!=XR_XIR_OK)goto done;
    if(control){status=xir_compile_resource_status(xr_compile_session_bind_request(w->session,control));if(status!=XR_XIR_OK)goto done;}
    if(stage)*stage=6;
    for(size_t i=0;i<count;++i) {
        status=xir_compile_resource_status(xr_compile_session_poll(w->session));if(status!=XR_XIR_OK)goto done;
        XrXirSourceRequest request={w->session,xr_source_authority_source_path(w->authorities[i]),
            &identities[i],&w->context,w->stdlib.path,xr_source_authority_lockfile(w->authorities[i]),
            XR_XIR_PROGRAM,NULL};
        XrXirSourceDiagnostic diagnostic={0};XrXirSourceFailure source_failure={0};
        status=xlsp_source_snapshot_build_authorities(&request,documents,identities,count,i,&w->snapshots[i],&diagnostic,&source_failure);
        if(status!=XR_XIR_OK) {
            if(failure&&diagnostic.status==status&&diagnostic.message[0]&&
                status!=XR_XIR_BUDGET&&status!=XR_XIR_OUT_OF_MEMORY&&status!=XR_XIR_IO&&
                status!=XR_XIR_MEMORY_LIMIT&&status!=XR_XIR_OVERFLOW&&status!=XR_XIR_CANCELLED&&
                status!=XR_XIR_DEADLINE&&status!=XR_XIR_CLOCK_FAILURE) {
                *failure=(XlspSourceWorkspaceFailure){true,diagnostic,source_failure};source_failure=(XrXirSourceFailure){0};
            }
            xr_xir_compile_source_failure_free(&source_failure);goto done;
        }
        xr_xir_compile_source_failure_free(&source_failure);
    }
    status=xir_compile_resource_status(xr_compile_session_poll(w->session));if(status!=XR_XIR_OK)goto done;
    *output=w;w=NULL;if(stage)*stage=0;
done:
    xr_compile_resources_free(path);xr_compile_resources_free(identities);
    xlsp_source_workspace_free(w);return status;
}
XrXirStatus xlsp_source_workspace_navigation(XlspSourceWorkspace *w,const char *uri,size_t length,
    XrLspPosition position,unsigned mode,bool include_declaration,XrJsonValue **out) {
    if(!w)return XR_XIR_BAD_STRUCTURE;
    if(mode==4) {
        XlspSourceSignatureRequest request={w->context.resources,w->session,w->snapshots,w->count,uri,length,position};
        return xlsp_source_signature_many_json(&request,out);
    }
    if(mode==3)return xlsp_source_hover_many_json(w->context.resources,w->snapshots,w->count,uri,length,position,out);
    return xlsp_source_navigation_many_json(w->context.resources,w->snapshots,w->count,
        uri,length,position,mode,include_declaration,out);
}

void xlsp_source_workspace_failure_free(XlspSourceWorkspaceFailure *failure) {
    if(!failure)return;xr_xir_compile_source_failure_free(&failure->source);*failure=(XlspSourceWorkspaceFailure){0};
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
                if(x[n].line!=y[n].line||x[n].column!=y[n].column||x[n].length!=y[n].length||x[n].type!=y[n].type||x[n].modifiers!=y[n].modifiers){status=XR_XIR_BAD_STRUCTURE;goto done;}
            }
            xlsp_source_tokens_free(current);current=NULL;
        } else {accepted=current;current=NULL;}
    }
    if(!accepted){status=XR_XIR_UNRESOLVED;goto done;}*out=accepted;accepted=NULL;
done:
    xlsp_source_tokens_free(current);xlsp_source_tokens_free(accepted);return status;
}
