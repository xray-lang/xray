/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Default declarations are received once; Source installs the scheduled IDs. */
static bool source_hir_nominal_declarations(SourceContext *ctx) {
    XrHirNominalImport *imports=ctx->nominals.count?source_alloc(ctx,ctx->nominals.count,sizeof(*imports)):NULL;
    if(ctx->nominals.count&&!imports)return false;
    if(!source_hir_nominal_imports(ctx,imports,ctx->nominals.count))return false;
    for(uint32_t d=0;d<ctx->nominals.count;++d) {
        SourceName *symbol=ctx->nominal_sources[d];if(!source_request_poll(ctx,symbol->node))return false;
        XrHirNominalDeclarationInput input={.types=&ctx->types,.declaration=d,.module=symbol->module,.source_declaration=symbol->declaration};
        if(symbol->checked_library) {
            if(!imports[d].construction)return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library nominal construction owner is missing");
            input.imported=&imports[d];
        } else if(symbol->node->type==AST_STRUCT_DECL||symbol->node->type==AST_CLASS_DECL) {
            ClassDeclNode *source=symbol->node->type==AST_CLASS_DECL?&symbol->node->as.class_decl:&symbol->node->as.struct_decl;
            uint32_t count=ctx->nominals.declarations[d].field_count;
            uint32_t *initializers=count?source_alloc(ctx,count,sizeof(*initializers)):NULL;if(count&&!initializers)return false;
            for(uint32_t f=0;f<count;++f) {
                if(!source_request_poll(ctx,source->fields[f]))return false;
                if(source->fields[f]->as.field_decl.initializer)initializers[f]=ctx->nominal_members[d][f];
            }
            input.field_initializers=initializers;
            for(int m=0;m<source->method_count;++m) {
                if(!source_request_poll(ctx,source->methods[m]))return false;
                if(source->methods[m]->type==AST_METHOD_DECL&&source->methods[m]->as.method_decl.is_constructor)input.explicit_initializer=true;
            }
        }
        const char *reason=NULL;XrXirStatus status=xr_hir_body_nominal_declare(ctx->typed_body,&input,ctx->request,&reason);
        if(!source_hir_nominal_status(ctx,symbol->node,status,reason))return false;
    }
    return true;
}
static bool source_struct_defaultability(SourceContext *ctx) {
    if(!source_hir_nominal_declarations(ctx))return false;uint32_t added=0,failed=UINT32_MAX;
    XrXirStatus status=xr_hir_body_nominal_schedule(ctx->typed_body,ctx->function_count,ctx->compile.limits.functions,ctx->request,&added,&failed);
    if(status!=XR_XIR_OK)return source_fail(ctx,failed<ctx->nominals.count?ctx->nominal_sources[failed]->node:NULL,status,
        status==XR_XIR_BUDGET?"default constructor function budget exhausted":"default declaration receiving failed");
    ctx->function_count+=added;ctx->first_closure+=added;ctx->next_closure+=added;return true;
}
static bool source_struct_constructors(SourceContext *ctx,uint32_t *next) {
    for(uint32_t d=0;d<ctx->nominals.count;++d) {
        SourceName *symbol=ctx->nominal_sources[d];if(symbol->checked_library)continue;
        if(!source_request_poll(ctx,symbol->node))return false;
        XrHirNominalDefaults defaults=source_hir_nominal_defaults(ctx,d);if(ctx->diagnostic.status!=XR_XIR_OK)return false;
        if(!defaults.available)continue;
        if(*next>=ctx->first_closure)return source_fail(ctx,symbol->node,XR_XIR_BAD_STRUCTURE,"default constructor function inventory mismatch");
        uint32_t index=(*next)++;
        ctx->functions[index]=(XrXirFunction){"$default",8,NULL,0,symbol->type,NULL,0,NULL,0,NULL,0,XR_XIR_UNIT};
        ctx->bodies[index].infer_errors=ctx->bodies[index].infer_initializer=true;
        ctx->bodies[index].node=symbol->node;ctx->bodies[index].module=symbol->module;ctx->bodies[index].declaration=symbol->declaration;
        if(!source_nominal_function_scope(ctx,index,symbol))return false;
        ctx->identities[index]=(XrXirFunctionIdentity){symbol->module,ctx->nominals.declarations[d].exported,d+1,0,0,
            ctx->nominals.declarations[d].kind==XR_XIR_NOMINAL_CLASS?XR_XIR_FUNCTION_NO_SUSPEND:0,XR_XIR_CONSTRUCTOR,0,0,0};
        if(!source_hir_nominal_bind(ctx,symbol->node,d,index,XR_HIR_NOMINAL_IMPLICIT,UINT32_MAX))return false;
    }
    return true;
}
static bool source_default_value(SourceContext *ctx,AstNode *node,XrXirType type,SourceValue *value) {
    if(ctx->diagnostic.status!=XR_XIR_OK)return false;XrHirNominalShape shape={0};XrHirBodyScope scope={0};
    const XrXirTypeNode *found=xr_xir_type_node(&ctx->types,type);
    if(found&&found->kind==XR_XIR_TYPE_NOMINAL&&!source_hir_nominal_shape(ctx,node,type,&shape,NULL))return false;
    if(!source_hir_current_scope(ctx,&scope))return false;
    XrHirNominalPool pool=source_hir_nominal_pool(ctx);XrHirNominalAction action;const char *reason=NULL;
    XrHirNominalUseInput input={.pool=&pool,.shape=shape,.scope=scope,.origin=source_hir_origin(ctx,node,0),.type=type};
    XrXirStatus status=xr_hir_body_nominal_default(ctx->typed_body,&input,ctx->request,&action,&reason);
    return source_hir_nominal_status(ctx,node,status,reason)&&source_hir_nominal_emit(ctx,node,&action,value);
}
static bool source_struct_constructor_body(SourceContext *ctx) {
    uint32_t d=ctx->identities[ctx->function].nominal_owner-1;SourceName *symbol=ctx->nominal_sources[d];
    ClassDeclNode *source=symbol->node->type==AST_CLASS_DECL?&symbol->node->as.class_decl:&symbol->node->as.struct_decl;
    XrHirNominalShape shape={0};XrHirNominalInfo info={0};
    if(!source_hir_nominal_shape(ctx,symbol->node,symbol->type,&shape,&info))return false;
    SourceValue *fields=info.count?source_alloc(ctx,info.count,sizeof(*fields)):NULL;if(info.count&&!fields)return false;
    XrHirNominalDefaults defaults=source_hir_nominal_defaults(ctx,d);if(ctx->diagnostic.status!=XR_XIR_OK)return false;
    for(uint32_t f=0;f<info.count;++f) {
        AstNode *field=source->fields[f];if(!source_request_poll(ctx,field))return false;
        if(defaults.fields[f]) {
            if(!source_hir_nominal_helper(ctx,field,symbol->type,f,&fields[f],NULL))return false;
        } else {
            XrHirNominalField received={0};
            if(!source_hir_nominal_field(ctx,field,shape,f,&received)||!source_default_value(ctx,field,received.type,&fields[f]))return false;
        }
    }
    SourceValue value;
    if(!source_hir_nominal_construct(ctx,symbol->node,&(SourceNominalEmission){symbol->type,0,fields,info.count},&value)||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{value.id,0},{0},0,{0}},NULL))return false;
    ctx->returned=true;return true;
}
