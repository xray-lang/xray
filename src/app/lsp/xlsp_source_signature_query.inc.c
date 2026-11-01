/* Copyright (c) 2026 Xinglei Xu. MIT License. */
XR_FUNC void xlsp_source_signature_free(XlspSourceSignature *signature) {
    if(!signature)return;
    for(size_t i=0;i<signature->parameter_count;++i)xlsp_source_hover_free(&signature->parameters[i]);
    xr_compile_resources_free(signature->parameters);xlsp_source_hover_free(&signature->label);
    xlsp_source_hover_free(&signature->target_identity);xlsp_source_hover_free(&signature->target_path);
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

static XrXirStatus signature_call_equal(const XlspSourceSnapshot *s,const XrXirSourceCallSignature *a,
    const XrXirSourceCallSignature *b) {
    if(a->parameter_count!=b->parameter_count||a->receiver_count!=b->receiver_count||
        a->result.type!=b->result.type||a->result.generic_owner!=b->result.generic_owner||
        a->type_parameter_count!=b->type_parameter_count||a->type_parameter_owner!=b->type_parameter_owner||
        a->owner_type.known!=b->owner_type.known||a->owner_type.type!=b->owner_type.type||
        a->owner_type.generic_owner!=b->owner_type.generic_owner)
        return XR_XIR_BAD_STRUCTURE;
    for(uint32_t p=0;p<a->type_parameter_count;++p)for(size_t c=0;;++c) {
        XrXirStatus status=lsp_work(s,2);if(status!=XR_XIR_OK)return status;
        if(a->type_parameters[p][c]!=b->type_parameters[p][c])return XR_XIR_BAD_STRUCTURE;
        if(!a->type_parameters[p][c])break;
    }
    for(uint32_t i=0;i<a->parameter_count;++i) {
        XrXirStatus status=lsp_work(s,sizeof(XrXirSourceType));if(status!=XR_XIR_OK)return status;
        if(a->parameters[i].type!=b->parameters[i].type||
            a->parameters[i].generic_owner!=b->parameters[i].generic_owner||
            (a->parameter_flags?a->parameter_flags[i]:0)!=(b->parameter_flags?b->parameter_flags[i]:0))return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus signature_call_observation(const XlspSourceSnapshot *s,
    const XlspSourceSignatureSelection *selection,uint32_t target,const XrXirSourceCallSignature **out) {
    const LspSourceText *text=NULL;
    for(size_t i=0;i<s->count;++i) {
        XrXirStatus status=lsp_work(s,1);if(status!=XR_XIR_OK)return status;
        if(s->texts[i].uri_length!=selection->uri_length)continue;
        bool equal=false;status=lsp_equal(s,s->texts[i].uri,selection->uri,selection->uri_length,&equal);
        if(status!=XR_XIR_OK)return status;if(equal){text=&s->texts[i];break;}
    }
    if(!text)return XR_XIR_UNRESOLVED;
    XlspSourceCursor start={0},end={0};
    XrXirStatus status=xlsp_source_cursor(s->resources,text->text,text->length,selection->call.callee.start,&start);
    if(status==XR_XIR_OK)status=xlsp_source_cursor(s->resources,text->text,text->length,selection->call.callee.end,&end);
    if(status!=XR_XIR_OK)return status;
    const XrXirSourceCalls *calls=xr_xir_compile_source_snapshot_calls(s->query);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(s->query);
    if(!calls)return XR_XIR_UNSUPPORTED;
    const XrXirSourceCallSignature *accepted=NULL;
    for(uint32_t i=0;i<calls->count;++i) {
        status=lsp_work(s,sizeof(XrXirSourceCall));if(status!=XR_XIR_OK)return status;
        const XrXirSourceCall *call=&calls->items[i];
        if(!call->selection_known||call->range.line!=start.line||call->range.column!=start.column||
            call->range.end_line!=end.line||call->range.end_column!=end.column)continue;
        bool matches=false;status=lsp_text_has_module(s,text,call->range.module,&matches);
        if(status!=XR_XIR_OK)return status;if(!matches)continue;
        if(call->reference>=view->reference_count||!call->signature.known)return XR_XIR_UNSUPPORTED;
        uint32_t candidate=view->references[call->reference].target;
        status=lsp_target(s,view,&candidate);if(status!=XR_XIR_OK)return status;
        status=lsp_symbol_equal(s,view,target,candidate,&matches);if(status!=XR_XIR_OK)return status;
        if(!matches)return XR_XIR_BAD_STRUCTURE;
        if(accepted){status=signature_call_equal(s,accepted,&call->signature);if(status!=XR_XIR_OK)return status;}
        else accepted=&call->signature;
    }
    if(!accepted)return XR_XIR_UNSUPPORTED;*out=accepted;return XR_XIR_OK;
}
static XrXirStatus signature_target(const XlspSourceSnapshot *s,const XrXirSourceView *view,
    const XrXirSourceDeclaration *declaration,const XrXirSourceSyntaxView *syntax,XlspSourceSignature *result) {
    if(declaration->range.module>=view->module_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceQueryModule *module=&view->modules[declaration->range.module];
    XrXirSourceRange range=syntax->declarations[declaration->id-1].name;
    if(!module->identity||(!module->path&&declaration->kind!=XR_XIR_SOURCE_INTRINSIC)||
        range.line<=0||range.column<=0||range.end_line!=range.line||range.end_column<=range.column)
        return XR_XIR_UNRESOLVED;
    LspHoverBuilder identity={s,NULL,0,0,XR_XIR_OK},path={s,NULL,0,0,XR_XIR_OK};
    hover_text(&identity,module->identity);if(module->path)hover_text(&path,module->path);
    result->target_identity=(XlspSourceHover){identity.text,identity.length};
    result->target_path=(XlspSourceHover){path.text,path.length};
    if(identity.status!=XR_XIR_OK)return identity.status;if(path.status!=XR_XIR_OK)return path.status;
    XrXirStatus status=lsp_work(s,sizeof(module->fingerprint)+sizeof(range)+sizeof(uint32_t));
    if(status!=XR_XIR_OK)return status;
    result->target_fingerprint=module->fingerprint;range.module=0;result->target_range=range;
    result->target_native_identity=declaration->native_identity;return XR_XIR_OK;
}
static XrXirStatus signature_parameters(const XlspSourceSnapshot *s,const XrXirSourceView *view,
    const XrXirSourceDeclaration *declaration,const XrXirSourceSyntaxView *syntax,
    const XrXirSourceCallSignature *call,XlspSourceSignature *result) {
    uint32_t index=0;
    for(uint32_t i=0;i<view->declaration_count;++i) {
        XrXirStatus status=lsp_work(s,sizeof(XrXirSourceDeclaration));if(status!=XR_XIR_OK)return status;
        const XrXirSourceDeclaration *parameter=&view->declarations[i];
        if(parameter->kind!=XR_XIR_SOURCE_PARAMETER||parameter->parent!=declaration->id)continue;
        if(index>=declaration->parameter_count||!declaration->parameters||
            parameter->type.known!=declaration->parameters[index].known||parameter->type.type!=declaration->parameters[index].type||
            parameter->type.generic_owner!=declaration->parameters[index].generic_owner)return XR_XIR_BAD_STRUCTURE;
        if(index>=call->parameter_count||!call->parameters[index].known)return XR_XIR_BAD_STRUCTURE;
        bool receiver=syntax->declarations[i].role==XR_XIR_SOURCE_SYNTAX_RECEIVER;
        if(receiver!=(index<call->receiver_count))return XR_XIR_BAD_STRUCTURE;
        uint32_t flags=call->parameter_flags?call->parameter_flags[index]:0;
        XrXirSourceType shown=call->parameters[index++];
        if(receiver)continue;
        if(syntax->declarations[i].role==XR_XIR_SOURCE_SYNTAX_UNRESOLVED)return XR_XIR_UNSUPPORTED;
        uint32_t mode=0;status=signature_parameter_mode(s,syntax,parameter,&mode);
        if(status!=XR_XIR_OK)return status;
        LspHoverBuilder builder={s,NULL,0,0,XR_XIR_OK};
        if(flags&XR_XIR_SOURCE_CALL_VARIADIC)hover_text(&builder,"...");
        hover_text(&builder,parameter->name);if(flags&XR_XIR_SOURCE_CALL_OPTIONAL)hover_text(&builder,"?");
        hover_text(&builder,": ");hover_mode(&builder,mode);
        hover_type_named(&builder,view,shown,call->type_parameters,call->type_parameter_count,call->type_parameter_owner);
        if(builder.status!=XR_XIR_OK){xr_compile_resources_free(builder.text);return builder.status;}
        result->parameters[result->parameter_count++]=(XlspSourceHover){builder.text,builder.length};
        if(flags&XR_XIR_SOURCE_CALL_VARIADIC) {
            if(index!=call->parameter_count)return XR_XIR_BAD_STRUCTURE;result->variadic=true;
        }
    }
    return index==declaration->parameter_count?XR_XIR_OK:XR_XIR_UNSUPPORTED;
}

XR_FUNC XrXirStatus xlsp_source_signature(const XlspSourceSnapshot *s,
    const XlspSourceSignatureSelection *selection,XlspSourceSignature *out) {
    const XrFingerprint empty={{0}};
    if(!s||!selection||selection->resources!=s->resources||!selection->uri||selection->uri_length==SIZE_MAX||!out||out->label.text||
        out->label.length||out->parameters||out->parameter_count||out->active_argument||out->variadic||
        out->target_identity.text||out->target_identity.length||out->target_path.text||out->target_path.length||
        out->target_native_identity||out->target_range.module||out->target_range.line||out->target_range.column||
        out->target_range.end_line||out->target_range.end_column||
        memcmp(&out->target_fingerprint,&empty,sizeof(empty)))return XR_XIR_BAD_STRUCTURE;
    if(!selection->call.has_callee)return XR_XIR_UNSUPPORTED;
    uint32_t id=0;
    XrXirStatus status=lsp_symbol(s,selection->uri,selection->uri_length,selection->call.callee.start,&id);
    if(status!=XR_XIR_OK)return status;
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(s->query);
    const XrXirSourceSyntaxView *syntax=lsp_syntax(s,view);
    if(!syntax||!id||id>view->declaration_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceDeclaration *declaration=&view->declarations[id-1];
    if((declaration->kind!=XR_XIR_SOURCE_FUNCTION&&declaration->kind!=XR_XIR_SOURCE_MEMBER)||
        (declaration->kind==XR_XIR_SOURCE_FUNCTION&&(declaration->native_identity||declaration->signature)))
        return XR_XIR_UNSUPPORTED;
    const XrXirSourceCallSignature *call=NULL;
    status=signature_call_observation(s,selection,id,&call);if(status!=XR_XIR_OK)return status;
    if(call->parameter_count!=declaration->parameter_count)return XR_XIR_BAD_STRUCTURE;
    XlspSourceSignature result={0};result.active_argument=selection->call.active_argument;
    if(declaration->parameter_count) {
        status=lsp_resource_status(xr_compile_resources_calloc(s->resources,declaration->parameter_count,
            sizeof(*result.parameters),(void **)&result.parameters));
        if(status!=XR_XIR_OK)return status;
    }
    status=signature_target(s,view,declaration,syntax,&result);
    if(status==XR_XIR_OK)status=signature_parameters(s,view,declaration,syntax,call,&result);
    if(status!=XR_XIR_OK)goto done;
    LspHoverBuilder builder={s,NULL,0,0,XR_XIR_OK};
    hover_text(&builder,"fn ");
    if(call->owner_type.known) {
        hover_type_named(&builder,view,call->owner_type,call->type_parameters,call->type_parameter_count,call->type_parameter_owner);
        hover_text(&builder,".");hover_text(&builder,declaration->name);
    } else hover_qualified_name(&builder,view,declaration);
    hover_text(&builder,"(");
    for(size_t i=0;i<result.parameter_count&&builder.status==XR_XIR_OK;++i) {
        if(i)hover_text(&builder,", ");
        hover_bytes(&builder,result.parameters[i].text,result.parameters[i].length);
    }
    hover_text(&builder,") -> ");
    hover_type_named(&builder,view,call->result,call->type_parameters,call->type_parameter_count,call->type_parameter_owner);
    status=builder.status;result.label=(XlspSourceHover){builder.text,builder.length};
    if(status==XR_XIR_OK){*out=result;result=(XlspSourceSignature){0};}
done:
    xlsp_source_signature_free(&result);return status;
}
