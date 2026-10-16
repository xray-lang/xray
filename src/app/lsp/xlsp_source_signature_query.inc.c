/* Copyright (c) 2026 Xinglei Xu. MIT License. */
XR_FUNC void xlsp_source_signature_free(XlspSourceSignature *signature) {
    if(!signature)return;
    for(size_t i=0;i<signature->parameter_count;++i)xlsp_source_hover_free(&signature->parameters[i]);
    xr_compile_resources_free(signature->parameters);xlsp_source_hover_free(&signature->label);
    *signature=(XlspSourceSignature){0};
}

XR_FUNC XrXirStatus xlsp_source_snapshot_document(const XlspSourceSnapshot *s,
    const char *uri,size_t length,XlspSourceDocument *out) {
    if(!s||!uri||length==SIZE_MAX||!out)return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;i<s->count;++i) {
        XrXirStatus status=lsp_work(s,1);if(status!=XR_XIR_OK)return status;
        const LspSourceText *text=&s->texts[i];if(text->uri_length!=length)continue;
        bool equal=false;status=lsp_equal(s,text->uri,uri,length,&equal);
        if(status!=XR_XIR_OK)return status;
        if(equal){*out=(XlspSourceDocument){text->uri,text->text,text->uri_length,text->length,text->version};return XR_XIR_OK;}
    }
    return XR_XIR_UNRESOLVED;
}

static XrXirStatus signature_parameter_mode(const XlspSourceSnapshot *s,
    const XrXirSourceSyntaxView *syntax,const XrXirSourceDeclaration *parameter,uint32_t *out) {
    uint32_t mode=XR_PARAM_READ;bool found=false;
    for(uint32_t i=0;i<syntax->marker_count;++i) {
        XrXirStatus status=lsp_work(s,sizeof(XrXirSourceMarker));if(status!=XR_XIR_OK)return status;
        const XrXirSourceMarker *marker=&syntax->markers[i];
        if(marker->declaration!=parameter->id||
            (marker->role!=XR_XIR_SOURCE_MARKER_REF&&marker->role!=XR_XIR_SOURCE_MARKER_MOVE))continue;
        if(found)return XR_XIR_BAD_STRUCTURE;found=true;
        mode=marker->role==XR_XIR_SOURCE_MARKER_REF?XR_PARAM_REF:XR_PARAM_MOVE;
    }
    if(parameter->mutable!=(mode==XR_PARAM_REF))return XR_XIR_BAD_STRUCTURE;
    *out=mode;return XR_XIR_OK;
}

static XrXirStatus signature_parameters(const XlspSourceSnapshot *s,const XrXirSourceView *view,
    const XrXirSourceDeclaration *declaration,const XrXirSourceSyntaxView *syntax,XlspSourceSignature *result) {
    uint32_t index=0;
    for(uint32_t i=0;i<view->declaration_count;++i) {
        XrXirStatus status=lsp_work(s,sizeof(XrXirSourceDeclaration));if(status!=XR_XIR_OK)return status;
        const XrXirSourceDeclaration *parameter=&view->declarations[i];
        if(parameter->kind!=XR_XIR_SOURCE_PARAMETER||parameter->parent!=declaration->id)continue;
        if(index>=declaration->parameter_count||!declaration->parameters||!parameter->type.known||
            !declaration->parameters[index].known||parameter->type.type!=declaration->parameters[index].type||
            parameter->type.generic_owner!=declaration->parameters[index].generic_owner)return XR_XIR_BAD_STRUCTURE;
        ++index;
        if(syntax->declarations[i].role==XR_XIR_SOURCE_SYNTAX_RECEIVER)continue;
        if(syntax->declarations[i].role==XR_XIR_SOURCE_SYNTAX_UNRESOLVED)return XR_XIR_UNSUPPORTED;
        uint32_t mode=0;status=signature_parameter_mode(s,syntax,parameter,&mode);
        if(status!=XR_XIR_OK)return status;
        LspHoverBuilder builder={s,NULL,0,0,XR_XIR_OK};
        hover_text(&builder,parameter->name);hover_text(&builder,": ");hover_mode(&builder,mode);
        hover_type(&builder,view,parameter->type);
        if(builder.status!=XR_XIR_OK){xr_compile_resources_free(builder.text);return builder.status;}
        result->parameters[result->parameter_count++]=(XlspSourceHover){builder.text,builder.length};
    }
    return index==declaration->parameter_count?XR_XIR_OK:XR_XIR_UNSUPPORTED;
}

XR_FUNC XrXirStatus xlsp_source_signature(const XlspSourceSnapshot *s,
    const XlspSourceSignatureSelection *selection,XlspSourceSignature *out) {
    if(!s||!selection||selection->resources!=s->resources||!selection->uri||selection->uri_length==SIZE_MAX||!out||out->label.text||
        out->label.length||out->parameters||out->parameter_count||out->active_argument)return XR_XIR_BAD_STRUCTURE;
    if(!selection->call.has_callee)return XR_XIR_UNSUPPORTED;
    uint32_t id=0;
    XrXirStatus status=lsp_symbol(s,selection->uri,selection->uri_length,selection->call.callee.start,&id);
    if(status!=XR_XIR_OK)return status;
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(s->query);
    const XrXirSourceSyntaxView *syntax=lsp_syntax(s,view);
    if(!syntax||!id||id>view->declaration_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceDeclaration *declaration=&view->declarations[id-1];
    if(declaration->kind!=XR_XIR_SOURCE_FUNCTION||declaration->generic_parameter_count||
        declaration->generic_parent_count||declaration->native_identity||declaration->signature)
        return XR_XIR_UNSUPPORTED;
    XlspSourceSignature result={0};result.active_argument=selection->call.active_argument;
    if(declaration->parameter_count) {
        status=lsp_resource_status(xr_compile_resources_calloc(s->resources,declaration->parameter_count,
            sizeof(*result.parameters),(void **)&result.parameters));
        if(status!=XR_XIR_OK)return status;
    }
    status=signature_parameters(s,view,declaration,syntax,&result);
    if(status!=XR_XIR_OK)goto done;
    LspHoverBuilder builder={s,NULL,0,0,XR_XIR_OK};
    hover_text(&builder,"fn ");hover_qualified_name(&builder,view,declaration);hover_text(&builder,"(");
    for(size_t i=0;i<result.parameter_count&&builder.status==XR_XIR_OK;++i) {
        if(i)hover_text(&builder,", ");
        hover_bytes(&builder,result.parameters[i].text,result.parameters[i].length);
    }
    hover_text(&builder,") -> ");hover_type(&builder,view,declaration->type);
    status=builder.status;result.label=(XlspSourceHover){builder.text,builder.length};
    if(status==XR_XIR_OK){*out=result;result=(XlspSourceSignature){0};}
done:
    xlsp_source_signature_free(&result);return status;
}
