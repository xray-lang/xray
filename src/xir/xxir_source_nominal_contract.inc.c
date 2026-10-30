/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Source emits only the nominal action received by its real construction body. */
static bool source_hir_nominal_imports(SourceContext *,XrHirNominalImport *,uint32_t);
static XrXirStatus source_hir_nominal_resolve(void *owner,const XrXirType *arguments,uint32_t count,
    XrXirType type,XrXirType *out) {
    SourceContext *ctx=owner;SourceSubstitution substitution={arguments,count};
    if(source_substitute(ctx,&substitution,type,0,out))return XR_XIR_OK;
    return ctx->diagnostic.status==XR_XIR_OK?XR_XIR_BAD_TYPE:ctx->diagnostic.status;
}
static XrHirNominalPool source_hir_nominal_pool(SourceContext *ctx) {
    return (XrHirNominalPool){&ctx->compile,&ctx->types,ctx->request,ctx,source_hir_nominal_resolve};
}
static bool source_hir_nominal_status(SourceContext *ctx,AstNode *node,XrXirStatus status,const char *reason) {
    return status==XR_XIR_OK||source_fail(ctx,node,status,reason?reason:"nominal receiving failed");
}
static bool source_hir_nominal_shape(SourceContext *ctx,AstNode *node,XrXirType type,XrHirNominalShape *shape,XrHirNominalInfo *info) {
    const char *reason=NULL;XrXirStatus status=xr_hir_body_nominal_receive(ctx->typed_body,&ctx->types,type,ctx->request,shape,&reason);
    if(!source_hir_nominal_status(ctx,node,status,reason))return false;
    return !info||xr_hir_body_nominal_info(ctx->typed_body,*shape,info)||source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"nominal shape has no owner");
}
static bool source_hir_nominal_field(SourceContext *ctx,AstNode *node,XrHirNominalShape shape,uint32_t field,XrHirNominalField *out) {
    XrHirNominalPool pool=source_hir_nominal_pool(ctx);const char *reason=NULL;
    XrXirStatus status=xr_hir_body_nominal_field(ctx->typed_body,shape,&pool,field,out,&reason);
    return source_hir_nominal_status(ctx,node,status,reason);
}
static XrHirNominalDefaults source_hir_nominal_defaults(SourceContext *ctx,uint32_t declaration) {
    XrHirNominalDefaults result={0};
    if(!xr_hir_body_nominal_defaults(ctx->typed_body,declaration,&result))source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"nominal default declaration has no owner");
    return result;
}
static bool source_hir_nominal_bind(SourceContext *ctx,AstNode *node,uint32_t declaration,uint32_t function,
    XrHirNominalFunctionKind kind,uint32_t field) {
    XrHirNominalFunctionInput input={.declaration=declaration,.function=function,.field=field,
        .source_declaration=ctx->bodies[function].declaration,.kind=kind};
    return source_hir_nominal_status(ctx,node,xr_hir_body_nominal_bind(ctx->typed_body,&input,ctx->request),"nominal helper declaration identity changed");
}
static bool source_hir_nominal_emit(SourceContext *ctx,AstNode *node,const XrHirNominalAction *action,SourceValue *value) {
    uint32_t before=ctx->bodies[ctx->function].count;
    uint32_t initial_block=ctx->bodies[ctx->function].block_count?ctx->bodies[ctx->function].current_block.identity:0;
    if(action->operation==XR_XIR_INVALID)*value=(SourceValue){.id=UINT32_MAX,.type=action->type};
    else if(action->operation==XR_XIR_STRUCT_NEW||action->operation==XR_XIR_CLASS_NEW||action->operation==XR_XIR_ENUM_NEW) {
        SourceValue *values=action->count?source_recipe_storage(ctx,action->count,sizeof(*values)):NULL;
        if(action->count&&!values)return false;
        for(uint32_t f=0;f<action->count;++f) {
            if(!source_request_poll(ctx,node))return false;values[f]=(SourceValue){.id=action->values[f].value,.type=action->values[f].type};
        }
        if(!source_recipe_group(ctx,(XrXirInstruction){.op=action->operation,.type=action->type,.immediate=action->variant},values,action->count,value))return false;
    } else {
        XrXirInstruction op={.op=action->operation,.type=action->type,.immediate=action->function};
        if(action->operation==XR_XIR_CALL&&!source_type_arguments(ctx,node,action->arguments,action->argument_count,&op))return false;
        if(!source_recipe_record(ctx,op,value))return false;
    }
    uint32_t anchor=UINT32_MAX;
    if(action->operation==XR_XIR_CALL) {
        SourceInstructionRecipe recipe=ctx->bodies[ctx->function].recipes[before];anchor=recipe.value;
        XrHirNominalCallEmission emission={recipe.instruction,anchor};
        if(!source_hir_nominal_status(ctx,node,xr_hir_body_nominal_call_emitted(ctx->typed_body,action->use,&emission),"nominal helper emission changed its actual call"))return false;
    }
    if(!source_hir_binding_emitted(ctx,&(XrHirBindingAction){.use=action->use},value,before))return false;
    /* Unit has no returned value; its required use belongs to the call itself,
     * rather than the handler's last error-continuation recipe. */
    if(anchor!=UINT32_MAX&&action->type==XR_XIR_UNIT) {
        SourceHirProjection *projection=ctx->bodies[ctx->function].typed_projections;
        projection->recipe=anchor;projection->block=initial_block;
    }
    return true;
}
typedef struct SourceNominalEmission {XrXirType type;uint32_t variant;const SourceValue *values;uint32_t count;} SourceNominalEmission;
static bool source_hir_nominal_construct(SourceContext *ctx,AstNode *node,const SourceNominalEmission *emission,SourceValue *value) {
    XrXirType type=emission->type;const SourceValue *values=emission->values;uint32_t count=emission->count;
    XrHirNominalShape shape={0};XrHirBodyScope scope={0};
    if(!source_hir_nominal_shape(ctx,node,type,&shape,NULL)||!source_hir_current_scope(ctx,&scope))return false;
    XrHirNominalValue *fields=count?source_recipe_storage(ctx,count,sizeof(*fields)):NULL;if(count&&!fields)return false;
    for(uint32_t f=0;f<count;++f){if(!source_request_poll(ctx,node))return false;fields[f]=(XrHirNominalValue){.type=values[f].type,.value=values[f].id};}
    XrHirNominalPool pool=source_hir_nominal_pool(ctx);XrHirNominalAction action;const char *reason=NULL;
    XrHirNominalUseInput input={.pool=&pool,.shape=shape,.scope=scope,.origin=source_hir_origin(ctx,node,0),.type=type,.values=fields,.count=count,.variant=emission->variant};
    XrXirStatus status=xr_hir_body_nominal_construct(ctx->typed_body,&input,ctx->request,&action,&reason);
    return source_hir_nominal_status(ctx,node,status,reason)&&source_hir_nominal_emit(ctx,node,&action,value);
}
static bool source_hir_nominal_get(SourceContext *ctx,AstNode *node,SourceValue receiver,uint32_t field,uint32_t variant,SourceValue *value) {
    XrHirNominalShape shape={0};XrHirBodyScope scope={0};
    if(!source_hir_nominal_shape(ctx,node,receiver.type,&shape,NULL)||!source_hir_current_scope(ctx,&scope))return false;
    XrHirNominalPool pool=source_hir_nominal_pool(ctx);XrHirNominalAction action;const char *reason=NULL;
    XrHirNominalUseInput input={.pool=&pool,.shape=shape,.scope=scope,.origin=source_hir_origin(ctx,node,0),.type=receiver.type,
        .receiver=receiver.id,.field=field,.variant=variant};
    XrXirStatus status=xr_hir_body_nominal_get(ctx->typed_body,&input,ctx->request,&action,&reason);
    if(!source_hir_nominal_status(ctx,node,status,reason))return false;
    uint32_t before=ctx->bodies[ctx->function].count;
    XrXirInstruction op={.op=action.operation,.type=action.type,.args={receiver.id,action.operation==XR_XIR_ENUM_GET?action.field:0},
        .immediate=action.operation==XR_XIR_ENUM_GET?action.variant:action.field};
    return source_recipe_record(ctx,op,value)&&source_hir_binding_emitted(ctx,&(XrHirBindingAction){.use=action.use},value,before);
}
static bool source_hir_nominal_helper(SourceContext *ctx,AstNode *node,XrXirType type,uint32_t field,SourceValue *value,XrHirBodyUse *use) {
    XrHirNominalShape shape={0};XrHirBodyScope scope={0};
    if(!source_hir_nominal_shape(ctx,node,type,&shape,NULL)||!source_hir_current_scope(ctx,&scope))return false;
    XrHirNominalPool pool=source_hir_nominal_pool(ctx);XrHirNominalAction action;const char *reason=NULL;
    XrHirNominalUseInput input={.pool=&pool,.shape=shape,.scope=scope,.origin=source_hir_origin(ctx,node,0),.type=type,.field=field};
    XrXirStatus status=xr_hir_body_nominal_helper(ctx->typed_body,&input,ctx->request,&action,&reason);
    if(!source_hir_nominal_status(ctx,node,status,reason)||!source_hir_nominal_emit(ctx,node,&action,value))return false;
    if(use)*use=action.use;return true;
}
