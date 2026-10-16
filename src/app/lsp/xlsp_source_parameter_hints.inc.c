/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "../../module/xmodule_fingerprint.h"
typedef struct ParameterHintsContext {
    const XlspSourceSnapshot *snapshot;
    const XrXirSourceView *view;
    const XrXirSourceSyntaxView *syntax;
    const LspSourceText *text;
    XrLspRange range;
    XlspSourceParameterHints hints;
    size_t capacity;
} ParameterHintsContext;
static void parameter_hint_free(XlspSourceParameterHint *hint) {
    xlsp_source_hover_free(&hint->label);xlsp_source_hover_free(&hint->target_identity);xlsp_source_hover_free(&hint->target_path);
    xlsp_source_hover_free(&hint->target_uri);
}
static XrXirStatus parameter_hint_location(ParameterHintsContext *ctx,const XrXirSourceDeclaration *target,
    XlspSourceParameterHint *hint) {
    if(target->native_identity)return XR_XIR_OK;
    XlspSourceLocation location={0};
    XrXirStatus status=lsp_location(ctx->snapshot,ctx->syntax->declarations[target->id-1].name,target->name,&location);
    if(status==XR_XIR_UNRESOLVED)return XR_XIR_OK;
    if(status!=XR_XIR_OK)return status;
    LspHoverBuilder uri={ctx->snapshot,NULL,0,0,XR_XIR_OK};hover_text(&uri,location.uri);
    hint->target_uri=(XlspSourceHover){uri.text,uri.length};if(uri.status!=XR_XIR_OK)return uri.status;
    hint->target_selection=location.range;hint->target_version=location.version;hint->has_target_location=true;return XR_XIR_OK;
}
XR_FUNC void xlsp_source_parameter_hints_free(XlspSourceParameterHints *hints) {
    if(!hints)return;
    for(size_t i=0;i<hints->count;++i)parameter_hint_free(&hints->items[i]);
    xr_compile_resources_free(hints->items);*hints=(XlspSourceParameterHints){0};
}
static XrXirStatus parameter_hint_same_variable(ParameterHintsContext *ctx,const XlspSourceArgumentSyntax *argument,
    const char *name,bool *same) {
    *same=false;if(!argument->has_variable)return XR_XIR_OK;
    XlspSourceCursor first={0},last={0};const XlspSourceSnapshot *s=ctx->snapshot;
    XrXirStatus status=xlsp_source_cursor(s->resources,ctx->text->text,ctx->text->length,argument->variable.start,&first);
    if(status==XR_XIR_OK)status=xlsp_source_cursor(s->resources,ctx->text->text,ctx->text->length,argument->variable.end,&last);
    if(status!=XR_XIR_OK)return status;if(first.offset>=last.offset)return XR_XIR_BAD_STRUCTURE;
    size_t length=0;if(!name)return XR_XIR_BAD_STRUCTURE;
    while(name[length]){status=lsp_work(s,1);if(status!=XR_XIR_OK)return status;++length;}
    if(length!=last.offset-first.offset)return XR_XIR_OK;
    return lsp_equal(s,ctx->text->text+first.offset,name,length,same);
}
static XrXirStatus parameter_hint_emit(ParameterHintsContext *ctx,const XrXirSourceDeclaration *target,
    const XrXirSourceDeclaration *parameter,const XlspSourceArgumentSyntax *argument,uint32_t mode) {
    if(type_hint_position_before(argument->range.start,ctx->range.start)||
        !type_hint_position_before(argument->range.start,ctx->range.end))return XR_XIR_OK;
    bool same=false;XrXirStatus status=parameter_hint_same_variable(ctx,argument,parameter->name,&same);
    if(status!=XR_XIR_OK||same)return status;
    const XlspSourceSnapshot *s=ctx->snapshot;XlspSourceSignature provenance={0};
    status=signature_target(s,ctx->view,target,ctx->syntax,&provenance);if(status!=XR_XIR_OK)goto done;
    LspHoverBuilder label={s,NULL,0,0,XR_XIR_OK};hover_text(&label,parameter->name);hover_text(&label,":");
    XlspSourceParameterHint hint={.position=argument->range.start,.label={label.text,label.length},.mode=mode,
        .target_identity=provenance.target_identity,.target_path=provenance.target_path,
        .target_fingerprint=provenance.target_fingerprint,.target_range=provenance.target_range,
        .target_native_identity=provenance.target_native_identity,.target_kind=target->kind};
    provenance.target_identity=(XlspSourceHover){0};provenance.target_path=(XlspSourceHover){0};status=label.status;
    if(status!=XR_XIR_OK)goto hint_done;
    status=parameter_hint_location(ctx,target,&hint);if(status!=XR_XIR_OK)goto hint_done;
    if(ctx->hints.count==ctx->capacity) {
        size_t capacity=ctx->capacity?ctx->capacity*2:8;
        if(capacity<ctx->capacity||capacity>SIZE_MAX/sizeof(*ctx->hints.items)){status=XR_XIR_BUDGET;goto hint_done;}
        status=lsp_resource_status(xr_compile_resources_resize(s->resources,(void **)&ctx->hints.items,capacity*sizeof(*ctx->hints.items)));
        if(status!=XR_XIR_OK)goto hint_done;ctx->capacity=capacity;
    }
    status=lsp_work(s,sizeof(hint));if(status!=XR_XIR_OK)goto hint_done;
    ctx->hints.items[ctx->hints.count++]=hint;hint=(XlspSourceParameterHint){0};
hint_done:
    parameter_hint_free(&hint);
done:
    xlsp_source_signature_free(&provenance);return status;
}
static XrXirStatus parameter_hint_parameters(ParameterHintsContext *ctx,const XrXirSourceDeclaration *target,
    const XrXirSourceCallSignature *signature,const XlspSourceArgumentCall *call) {
    uint32_t index=0;size_t explicit_count=0;bool variadic=false;
    if(signature->parameter_count!=target->parameter_count)return XR_XIR_BAD_STRUCTURE;
    for(uint32_t i=0;i<ctx->view->declaration_count;++i) {
        XrXirStatus status=lsp_work(ctx->snapshot,sizeof(XrXirSourceDeclaration));if(status!=XR_XIR_OK)return status;
        const XrXirSourceDeclaration *parameter=&ctx->view->declarations[i];
        if(parameter->kind!=XR_XIR_SOURCE_PARAMETER||parameter->parent!=target->id)continue;
        if(index>=target->parameter_count||!target->parameters||!signature->parameters[index].known||
            parameter->type.known!=target->parameters[index].known||parameter->type.type!=target->parameters[index].type||
            parameter->type.generic_owner!=target->parameters[index].generic_owner)return XR_XIR_BAD_STRUCTURE;
        const XrXirSourceDeclarationSyntax *fact=&ctx->syntax->declarations[i];
        bool receiver=fact->role==XR_XIR_SOURCE_SYNTAX_RECEIVER;
        if(receiver!=(index<signature->receiver_count))return XR_XIR_BAD_STRUCTURE;
        uint32_t flags=signature->parameter_flags?signature->parameter_flags[index]:0;++index;
        if(receiver)continue;
        bool governed=fact->role==XR_XIR_SOURCE_SYNTAX_NONE&&target->kind==XR_XIR_SOURCE_MEMBER&&target->native_identity;
        if(fact->role!=XR_XIR_SOURCE_SYNTAX_NAME&&!governed)return XR_XIR_UNSUPPORTED;
        uint32_t mode=0;status=signature_parameter_mode(ctx->snapshot,ctx->syntax,parameter,&mode);if(status!=XR_XIR_OK)return status;
        size_t end=explicit_count<call->call.argument_count?explicit_count+1:explicit_count;
        if(flags&XR_XIR_SOURCE_CALL_VARIADIC) {
            if(index!=target->parameter_count)return XR_XIR_BAD_STRUCTURE;
            variadic=true;end=call->call.argument_count;
        }
        for(size_t a=explicit_count;a<end;++a) {
            status=parameter_hint_emit(ctx,target,parameter,&call->arguments[a],mode);if(status!=XR_XIR_OK)return status;
        }
        ++explicit_count;
    }
    return index!=target->parameter_count?XR_XIR_BAD_STRUCTURE:
        call->call.argument_count>explicit_count&&!variadic?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
}
static XrXirStatus parameter_hint_call(ParameterHintsContext *ctx,const XlspSourceArgumentCall *call) {
    bool selected=false;
    for(size_t i=0;i<call->call.argument_count;++i) {
        XrXirStatus status=lsp_work(ctx->snapshot,sizeof(*call->arguments));if(status!=XR_XIR_OK)return status;
        XrLspPosition position=call->arguments[i].range.start;
        if(!type_hint_position_before(position,ctx->range.start)&&type_hint_position_before(position,ctx->range.end))selected=true;
    }
    if(!selected)return XR_XIR_OK;if(!call->call.has_callee)return XR_XIR_UNSUPPORTED;
    XlspSourceSignatureSelection selection={ctx->snapshot->resources,ctx->text->uri,ctx->text->uri_length,call->call};
    uint32_t target=0;XrXirStatus status=lsp_symbol(ctx->snapshot,selection.uri,selection.uri_length,call->call.callee.start,&target);
    if(status!=XR_XIR_OK)return status;
    const XrXirSourceDeclaration *declaration=&ctx->view->declarations[target-1];
    if(declaration->kind!=XR_XIR_SOURCE_FUNCTION&&declaration->kind!=XR_XIR_SOURCE_MEMBER&&
        declaration->kind!=XR_XIR_SOURCE_INTRINSIC)return XR_XIR_UNSUPPORTED;
    const XrXirSourceCallSignature *signature=NULL;status=signature_call_observation(ctx->snapshot,&selection,target,&signature);
    return status==XR_XIR_OK?parameter_hint_parameters(ctx,declaration,signature,call):status;
}
static XrXirStatus parameter_hints_sort(ParameterHintsContext *ctx) {
    for(size_t i=1;i<ctx->hints.count;++i) {
        size_t at=i;
        while(at) {
            XrXirStatus status=lsp_work(ctx->snapshot,sizeof(*ctx->hints.items));if(status!=XR_XIR_OK)return status;
            if(!type_hint_position_before(ctx->hints.items[at].position,ctx->hints.items[at-1].position))break;
            XlspSourceParameterHint swap=ctx->hints.items[at];ctx->hints.items[at]=ctx->hints.items[at-1];ctx->hints.items[--at]=swap;
        }
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xlsp_source_parameter_hints(const XlspSourceSnapshot *s,const XlspSourceCallArguments *arguments,
    XrLspRange range,XlspSourceParameterHints *out) {
    const XlspSourceCallArgumentsView *input=xlsp_source_call_arguments_view(arguments);
    if(!s||!input||input->resources!=s->resources||!out||out->items||out->count)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=xlsp_source_semantic_range(s,input->uri,input->uri_length,range);if(status!=XR_XIR_OK)return status;
    ParameterHintsContext ctx={0};ctx.snapshot=s;ctx.range=range;ctx.view=xr_xir_compile_source_snapshot_view(s->query);
    ctx.syntax=ctx.view?lsp_syntax(s,ctx.view):NULL;if(!ctx.syntax)return XR_XIR_UNRESOLVED;
    for(size_t i=0;i<s->count;++i) {
        status=lsp_work(s,1);if(status!=XR_XIR_OK)return status;if(s->texts[i].uri_length!=input->uri_length)continue;
        bool same=false;status=lsp_equal(s,s->texts[i].uri,input->uri,input->uri_length,&same);if(status!=XR_XIR_OK)return status;
        if(same){ctx.text=&s->texts[i];break;}
    }
    if(!ctx.text||ctx.text->version!=input->version||ctx.text->length!=input->source_length)return XR_XIR_BAD_STRUCTURE;
    XrFingerprint fingerprint={{0}};status=lsp_resource_status(xr_compile_module_source_fingerprint(s->resources,ctx.text->text,&fingerprint));
    if(status!=XR_XIR_OK)return status;
    status=lsp_work(s,sizeof(fingerprint));if(status!=XR_XIR_OK)return status;
    if(memcmp(&fingerprint,&input->fingerprint,sizeof(fingerprint)))return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;i<input->count;++i){status=parameter_hint_call(&ctx,&input->calls[i]);if(status!=XR_XIR_OK)goto done;}
    status=parameter_hints_sort(&ctx);
    if(status==XR_XIR_OK){*out=ctx.hints;ctx.hints=(XlspSourceParameterHints){0};}
done:
    xlsp_source_parameter_hints_free(&ctx.hints);return status;
}
