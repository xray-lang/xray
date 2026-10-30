/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Actual Source declarations and emissions receive owned typed handles. */
static bool source_hir_body_begin(SourceContext *ctx) {
    XrXirStatus status=xr_hir_body_new(ctx->compile.resources,&ctx->types,&ctx->typed_body);
    if(status!=XR_XIR_OK)return source_fail(ctx,NULL,status,"typed body allocation failed");
    XrHirBodyDeclarations declarations={&ctx->functions,&ctx->identities,&ctx->function_count,
        &ctx->module_count,&ctx->defaults,&ctx->interfaces,&ctx->slots,&ctx->slot_count};
    status=xr_hir_body_declarations(ctx->typed_body,&declarations);
    if(status!=XR_XIR_OK){xr_hir_body_end(ctx->typed_body);return source_fail(ctx,NULL,status,"typed declaration owner is missing");}
    return true;
}
static XrHirBodyOrigin source_hir_origin(SourceContext *ctx,AstNode *node,uint32_t binding) {
    XrXirSourceRange range=source_query_range(ctx,node,NULL);
    return (XrHirBodyOrigin){ctx->function,ctx->module,node?node->node_id:0,binding,
        range.line,range.column,range.end_line,range.end_column};
}
static bool source_hir_scope_enter(SourceContext *ctx,AstNode *node,XrHirBodyScope *previous) {
    SourceFunction *function=&ctx->bodies[ctx->function];*previous=function->typed_scope;
    XrHirBodyScope scope={0};XrXirStatus status=xr_hir_body_scope(ctx->typed_body,*previous,
        source_hir_origin(ctx,node,0),ctx->request,&scope);
    if(status!=XR_XIR_OK)return source_fail(ctx,node,status,"typed scope receive failed");
    function->typed_scope=scope;return true;
}
static void source_hir_scope_leave(SourceContext *ctx,XrHirBodyScope previous) {
    ctx->bodies[ctx->function].typed_scope=previous;
}
static bool source_hir_current_scope(SourceContext *ctx,XrHirBodyScope *output) {
    SourceFunction *function=&ctx->bodies[ctx->function];
    if(!function->typed_scope.owner){XrHirBodyScope previous={0};if(!source_hir_scope_enter(ctx,function->node,&previous))return false;}
    *output=function->typed_scope;return true;
}
static bool source_history_binding_lookup(SourceContext *,const SourceName *,SourceBindingAccessor,uint32_t *);
static bool source_hir_binding_register(SourceContext *ctx,SourceName *symbol,XrHirBodyBinding *out) {
    if(!symbol||!ctx->bodies||ctx->function>=ctx->function_count)return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"typed binding has no declaration owner");
    uint32_t parent=symbol->declaration&&symbol->declaration<=ctx->query.declaration_count?
        ctx->query.declarations[symbol->declaration-1].parent:0;
    uint32_t binding_function=symbol->kind==SOURCE_SLOT||symbol->kind==SOURCE_UNIT_SLOT?UINT32_MAX:ctx->function;
    /* Local parameters/captures historically left this unused field zero.
     * Their real producer now records its actual physical declaration module. */
    if(binding_function!=UINT32_MAX)symbol->module=ctx->bodies[binding_function].module;
    XrHirBindingKey key={symbol->module,symbol->declaration,symbol->node?symbol->node->node_id:0,parent,binding_function,binding_function==UINT32_MAX?symbol->index:UINT32_MAX};
    bool found=false;XrXirStatus lookup=xr_hir_body_binding_find(ctx->typed_body,key,ctx->request,out,&found);
    if(lookup!=XR_XIR_OK)return source_fail(ctx,symbol->node,lookup,"typed declaration lookup interrupted");
    if(found){symbol->binding_function=binding_function;symbol->typed_binding=*out;return true;}
    SourceFunction *function=&ctx->bodies[ctx->function];
    if(binding_function!=UINT32_MAX&&!function->typed_scope.owner) {XrHirBodyScope previous={0};if(!source_hir_scope_enter(ctx,function->node,&previous))return false;}
    XrXirType physical=symbol->type;
    if(symbol->mutable&&!symbol->direct_borrowed&&!source_cell_type(ctx,symbol->type,&physical))return false;
    size_t length=source_text_size(ctx,symbol->name);if(ctx->diagnostic.status!=XR_XIR_OK)return false;
    XrHirBodyOrigin definition=source_hir_origin(ctx,symbol->node,symbol->declaration);definition.module=symbol->module;
    if(symbol->declaration&&symbol->declaration<=ctx->query.declaration_count) {
        XrXirSourceRange range=ctx->query.declarations[symbol->declaration-1].range;
        definition.line=range.line;definition.column=range.column;definition.end_line=range.end_line;definition.end_column=range.end_column;
    }
    XrHirBodyScope declaration_scope=function->typed_scope;
    if(symbol->kind==SOURCE_SLOT||symbol->kind==SOURCE_UNIT_SLOT) {
        definition.function=UINT32_MAX;
        XrHirBodyOrigin module={.function=UINT32_MAX,.module=symbol->module};
        XrXirStatus status=xr_hir_body_scope(ctx->typed_body,(XrHirBodyScope){0},module,ctx->request,&declaration_scope);
        if(status!=XR_XIR_OK)return source_fail(ctx,symbol->node,status,"typed module scope has no declaration owner");
    }
    XrHirBindingInput input={.types=&ctx->types,.key=key,.scope=declaration_scope,
        .origin=definition,.name=symbol->name,.name_length=length,
        .logical=symbol->type,.physical=physical,.annotation=symbol->annotation,.has_annotation=symbol->has_annotation,
        .storage=source_hir_storage(symbol),.mode=symbol->declared_mode,
        .rebindable=symbol->mutable,.construction=symbol->construction,.direct_borrowed=symbol->direct_borrowed,
        .is_parameter=symbol->is_parameter,.parameter=symbol->index,.location=symbol->index};
    input.history=ctx->history!=NULL;
    if(binding_function==UINT32_MAX&&ctx->history) {
        uint32_t accessor=0;
        if(source_history_binding_lookup(ctx,symbol,SOURCE_ACCESS_READ,&accessor))input.reader=accessor+1;
        if(symbol->mutable&&source_history_binding_lookup(ctx,symbol,SOURCE_ACCESS_WRITE,&accessor))input.writer=accessor+1;
        if(ctx->diagnostic.status!=XR_XIR_OK)return false;
    }
    const char *reason=NULL;XrXirStatus status=xr_hir_body_binding_declare(ctx->typed_body,&input,ctx->request,out,&reason);
    if(status==XR_XIR_OK){symbol->binding_function=binding_function;symbol->typed_binding=*out;}
    return status==XR_XIR_OK||source_fail(ctx,symbol->node,status,reason?reason:"typed binding receive failed");
}
static bool source_hir_binding_receive(SourceContext *ctx,const SourceName *symbol,XrHirBodyBinding *out) {
    if(!symbol)return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"typed binding has no declaration");
    if(symbol->typed_binding.owner!=ctx->typed_body)
        return source_fail(ctx,symbol->node,XR_XIR_BAD_STRUCTURE,"typed binding was not registered by its declaration producer");
    *out=symbol->typed_binding;return true;
}
static bool source_hir_binding_use(SourceContext *ctx,AstNode *node,const SourceName *symbol,
    XrHirBindingUseKind kind,SourceValue value,XrHirBindingAction *out) {
    XrHirBodyBinding binding={0};if(!source_hir_binding_receive(ctx,symbol,&binding))return false;
    XrHirBodyScope use_scope={0};if(!source_hir_current_scope(ctx,&use_scope))return false;
    XrHirBindingUseInput input={.types=&ctx->types,.scope=use_scope,.origin=source_hir_origin(ctx,node,symbol->declaration),.kind=kind,.value=value.type,.value_id=value.id};const char *reason=NULL;
    XrXirStatus status=xr_hir_body_binding_use(ctx->typed_body,binding,&input,ctx->request,out,&reason);
    return status==XR_XIR_OK||source_fail(ctx,node,status,reason?reason:"typed binding use failed");
}
static bool source_hir_binding_emitted(SourceContext *,const XrHirBindingAction *,const SourceValue *,uint32_t);
static bool source_hir_reference(SourceContext *ctx,AstNode *node,SourceName *symbol,bool projected,const char *failure) {
    if(!symbol)return source_fail(ctx,node,XR_XIR_BAD_TYPE,failure);
    XrHirBodyBinding binding={0};if(!source_hir_binding_receive(ctx,symbol,&binding))return false;
    XrHirBodyScope use_scope={0};if(!source_hir_current_scope(ctx,&use_scope))return false;
    XrHirBindingUseInput input={.types=&ctx->types,.scope=use_scope,.origin=source_hir_origin(ctx,node,symbol->declaration),
        .kind=XR_HIR_BINDING_REFERENCE,.root_type=source_symbol_type(ctx,symbol),.projected=projected,.type_failure=failure};
    XrHirBindingAction action;const char *reason=NULL;
    XrXirStatus status=xr_hir_body_binding_use(ctx->typed_body,binding,&input,ctx->request,&action,&reason);
    if(status!=XR_XIR_OK)return source_fail(ctx,node,status,reason?reason:failure);
    return source_hir_binding_emitted(ctx,&action,NULL,ctx->bodies[ctx->function].count);
}
static bool source_hir_binding_emitted(SourceContext *ctx,const XrHirBindingAction *action,
    const SourceValue *value,uint32_t before) {
    XrHirBodyAction checked;
    XrXirStatus status=xr_hir_body_consume(ctx->typed_body,action->use,&checked);
    if(status!=XR_XIR_OK)return source_fail(ctx,NULL,status,"typed binding emitter has no current use");
    XrXirType type=checked.target;
    if(value&&xr_hir_body_emit(ctx->typed_body,action->use,value->type,value->id)!=XR_XIR_OK)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"typed emission changed its actual storage or place owner");
    if(value&&value->type!=type)return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"typed binding emission changed its type");
    SourceFunction *function=&ctx->bodies[ctx->function];
    SourceHirProjection *projection=source_alloc(ctx,1,sizeof(*projection));if(!projection)return false;
    bool instruction=function->count>before;
    uint32_t id=instruction&&(!value||value->type==XR_XIR_UNIT)?function->recipes[function->count-1].value:
        value?value->id:UINT32_MAX;
    *projection=(SourceHirProjection){function->typed_projections,action->use,id,instruction?id:UINT32_MAX,
        function->block_count?function->current_block.identity:UINT32_MAX,checked.formal,checked.argument,type,
        instruction,value&&value->type!=XR_XIR_UNIT&&value->id!=UINT32_MAX};
    function->typed_projections=projection;return true;
}
static bool source_hir_discard_to(SourceContext *ctx,const SourceHirProjection *stop) {
    SourceFunction *body=&ctx->bodies[ctx->function];
    for(SourceHirProjection *use=body->typed_projections;use!=stop;use=use->next) {
        if(!use||xr_hir_body_retire(ctx->typed_body,use->use)!=XR_XIR_OK)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"discarded region has no actual typed use");
    }
    return true;
}
static bool source_hir_place_check(SourceContext *ctx,AstNode *node,XrHirPlaceInput input,XrHirPlaceAction *action) {
    SourceFunction *function=&ctx->bodies[ctx->function];
    if(!function->typed_scope.owner){XrHirBodyScope previous={0};if(!source_hir_scope_enter(ctx,function->node,&previous))return false;}
    input.types=&ctx->types;input.scope=function->typed_scope;input.origin=source_hir_origin(ctx,node,0);
    const char *reason=NULL;XrXirStatus status=xr_hir_body_place(ctx->typed_body,&input,ctx->request,action,&reason);
    return status==XR_XIR_OK||source_fail(ctx,node,status,reason?reason:"typed place has no current owner");
}
static bool source_hir_place_emitted(SourceContext *ctx,const XrHirPlaceAction *action,const SourceValue *value,uint32_t before) {
    XrHirBindingAction use={.use=action->use};return source_hir_binding_emitted(ctx,&use,value,before);
}
static bool source_hir_parameter_declared(SourceContext *ctx,SourceName *symbol) {
    symbol->is_parameter=true;XrHirBodyBinding binding={0};if(!source_hir_binding_register(ctx,symbol,&binding))return false;
    XrHirBindingAction action;
    if(!source_hir_binding_use(ctx,symbol->node,symbol,XR_HIR_BINDING_PARAMETER,(SourceValue){.id=symbol->index,.type=symbol->type,.place={0}},&action))return false;
    ctx->bodies[ctx->function].parameters[symbol->index]=action.physical;
    SourceValue parameter={.id=symbol->index,.type=action.physical,.place={0}};
    return source_hir_binding_emitted(ctx,&action,&parameter,ctx->bodies[ctx->function].count);
}
static bool source_hir_value_declared(SourceContext *ctx,AstNode *node,SourceName *symbol,SourceValue value) {
    XrHirBodyBinding binding={0};if(!source_hir_binding_register(ctx,symbol,&binding))return false;
    XrHirBindingAction action;
    if(!source_hir_binding_use(ctx,node,symbol,XR_HIR_BINDING_INITIALIZE,value,&action))return false;
    return source_hir_binding_emitted(ctx,&action,&value,ctx->bodies[ctx->function].count);
}
