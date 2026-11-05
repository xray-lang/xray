/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_constructors.inc.c - Declaration-owned construction and field storage
 *
 * KEY CONCEPT:
 *   A complete nominal value is assembled only from initialized field places.
 */
static bool source_class_stage_result(SourceContext *,AstNode *,SourceValue *);
static bool source_class_stage_wrapper(SourceContext *);
static bool source_class_field_defaults(SourceContext *,uint32_t);

typedef struct SourceConstructorScan {
    SourceContext *ctx;
    uint32_t depth;
    bool cleanup, shared;
} SourceConstructorScan;
static bool source_constructor_scan(AstNode *node, void *pointer) {
    SourceConstructorScan *scan = pointer;
    if (!node || scan->shared) return true;
    if (!((scan->ctx)->diagnostic.status == XR_XIR_OK)) return false;
    if (scan->depth == 128) return source_fail(scan->ctx, node, XR_XIR_BUDGET, "constructor capture depth exhausted");
    if (node->type == AST_FUNCTION_EXPR && !scan->cleanup) return true;
    if (scan->cleanup && (node->type == AST_THIS_EXPR ||
        (node->type == AST_VARIABLE && source_text_same(scan->ctx, node, node->as.variable.name, "this")))) {
        scan->shared = true; return true;
    }
    bool cleanup = scan->cleanup;
    scan->cleanup |= node->type == AST_DEFER_STMT;
    ++scan->depth;
    bool ok = xr_ast_for_each_child(node, source_constructor_scan, scan);
    --scan->depth; scan->cleanup = cleanup;
    return ok || source_fail(scan->ctx, node, XR_XIR_BAD_STRUCTURE, "unknown constructor syntax");
}
static bool source_constructor_active(SourceContext *ctx) {
    AstNode *node = ctx->bodies[ctx->function].node;
    return node && node->type == AST_METHOD_DECL && node->as.method_decl.is_constructor;
}
static bool source_constructor_receiver(SourceContext *ctx, AstNode *node) {
    if (!node || (node->type != AST_THIS_EXPR &&
        (node->type != AST_VARIABLE || !source_text_same(ctx, NULL, node->as.variable.name, "this")))) return false;
    SourceName *symbol = visible_name(ctx, "this");
    return symbol && symbol->construction;
}
static bool source_constructor_storage_type(SourceContext *ctx, XrXirType receiver, uint32_t field, XrXirType *type) {
    XrHirNominalShape shape={0};XrHirNominalField member={0};
    if(!source_hir_nominal_shape(ctx,NULL,receiver,&shape,NULL)||!source_hir_nominal_field(ctx,NULL,shape,field,&member))return false;
    *type=member.type;
    if (ctx->bodies[ctx->function].constructor_shared && (member.flags & XR_XIR_FIELD_MUTABLE))
        return source_cell_type(ctx,*type,type);
    return true;
}
static bool source_constructor_storage(SourceContext *ctx, uint32_t field, SourceValue *value) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    XrXirType type;
    if (!((ctx)->diagnostic.status == XR_XIR_OK) || !source_constructor_storage_type(ctx, visible_name(ctx, "this")->type, field, &type)) return false;
    if (body->constructor_captured) { *value = (SourceValue){.id=body->constructor_places[field], .type=type,.place={0}}; return true; }
    return source_recipe_record(ctx, (XrXirInstruction){XR_XIR_LOCAL_READ, type, {body->constructor_places[field]}, {0}, 0, {0}}, value);
}
static bool source_constructor_read(SourceContext *ctx, uint32_t field, SourceValue *value) {
    if (!source_constructor_storage(ctx, field, value)) return false;
    if (!xr_xir_type_is_cell(&ctx->types, value->type)) return true;
    return source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CELL_READ, xr_xir_cell_element(&ctx->types, value->type),
        {value->id}, {0}, 0, {0}}, value);
}
static bool source_constructor_value(SourceContext *ctx, SourceValue *value) {
    XrXirType type = visible_name(ctx, "this")->type;
    XrHirNominalShape shape={0};XrHirNominalInfo info={0};
    if(!source_hir_nominal_shape(ctx,ctx->bodies[ctx->function].node,type,&shape,&info))return false;
    uint32_t count = info.count;
    SourceValue *fields = count ? source_alloc(ctx, count, sizeof(*fields)) : NULL;
    if (count && !fields) return false;
    for (uint32_t f = 0; f < count; ++f)
        if (!source_constructor_read(ctx, f, &fields[f])) return false;
    return source_hir_nominal_construct(ctx,ctx->bodies[ctx->function].node,&(SourceNominalEmission){type,0,fields,count},value);
}
static bool source_constructor_return(SourceContext *ctx, AstNode *node) {
    if (node && node->type == AST_RETURN_STMT && node->as.return_stmt.value_count)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constructor return must be implicit or bare");
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (body->frontier) {
        if (!source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CLEANUP_LEAVE, XR_XIR_UNIT, {0}, {body->block_count}, 0, {0}}, NULL)) return false;
        body->frontier = 0;
        if (!begin_block(ctx)) return false;
    }
    SourceValue value;
    if (!(ctx->identities[ctx->function].method_kind==XR_XIR_CONSTRUCTOR_FIELDS ?
            source_class_stage_result(ctx,node,&value) : source_constructor_value(ctx, &value)) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {value.type==XR_XIR_UNIT?0:value.id, 0}, {0}, 0, {0}}, NULL)) return false;
    ctx->returned = true; return true;
}
static bool source_constructor_store(SourceContext *ctx, uint32_t field, SourceValue value) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    XrXirType storage;
    if (!((ctx)->diagnostic.status == XR_XIR_OK) || !source_constructor_storage_type(ctx, visible_name(ctx, "this")->type, field, &storage)) return false;
    XrXirOp op = xr_xir_type_is_cell(&ctx->types, storage) ?
        (body->constructor_captured ? XR_XIR_CELL_WRITE : XR_XIR_CELL_LOCAL_WRITE) : XR_XIR_LOCAL_WRITE;
    return source_recipe_record(ctx, (XrXirInstruction) {op, XR_XIR_UNIT, {body->constructor_places[field], value.id}, {0}, 0, {0}}, NULL);
}
static bool source_constructor_field(SourceContext *ctx, AstNode *node, const char *name,
    SourceValue *value, AstNode *incoming) {
    uint32_t field; XrXirType type;
    unsigned access = incoming ? (source_constructor_active(ctx) ? 2u : 1u) : 0u;
    if (!source_struct_field(ctx, node, visible_name(ctx, "this")->type, name, access, &field, &type)) return false;
    XrHirPlaceAction selected,action;
    if(!source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_CONSTRUCTOR,
        .value=visible_name(ctx,"this")->type,.type=type,.field=field,.access=access},&selected)||
        !source_hir_place_emitted(ctx,&selected,NULL,ctx->bodies[ctx->function].count))return false;
    if(!incoming) {
        if(!source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_READ,.parent=selected.place},&action))return false;
        uint32_t before=ctx->bodies[ctx->function].count;
        return source_constructor_read(ctx,action.field,value)&&source_hir_place_emitted(ctx,&action,value,before);
    }
    if(!source_plan_expression(ctx,incoming,(SourceExpectedType){selected.type!=XR_XIR_UNIT,selected.type,false,false},value)||
        !source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_WRITE,.parent=selected.place,
            .value=value->type,.failure="constructor field type mismatch"},&action))return false;
    uint32_t before=ctx->bodies[ctx->function].count;
    return source_constructor_store(ctx,action.field,*value)&&source_hir_place_emitted(ctx,&action,NULL,before);
}
static bool source_constructor_place(SourceContext *ctx,AstNode *node,const char *name,SourceValue *place) {
    SourceName *self=visible_name(ctx,"this");uint32_t field;XrXirType type,storage;
    if(!self || !self->construction || !ctx->bodies[ctx->function].constructor_places)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"constructor field place has no active local owner");
    XrXirType receiver=self->type;
    if(!source_struct_field(ctx,node,receiver,name,3,&field,&type) ||
        !source_constructor_storage_type(ctx,receiver,field,&storage))return false;
    *place=(SourceValue){.id=ctx->bodies[ctx->function].constructor_places[field],.type=type};
    if(xr_xir_type_is_cell(&ctx->types,storage)) {
        SourceValue cell;
        if(!source_constructor_storage(ctx,field,&cell) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_PLACE,type,{cell.id},{0},0,{0}},place))return false;
    }
    /* Ordinary mutation roots at the actual staged local, never unpublished
     * this. Explicit ref continues to require its existing real Cell owner. */
    XrHirPlaceAction action;uint32_t before=ctx->bodies[ctx->function].count;
    if(!source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_CONSTRUCTOR,
        .value=receiver,.type=type,.field=field,.access=3,.base=place->id},&action) ||
        !source_hir_place_emitted(ctx,&action,NULL,before))return false;
    place->place=action.place;return source_query_expression(ctx,node,type);
}
static bool source_constructor_declare(SourceContext *ctx, SourceName *owner, AstNode *node, uint32_t *next) {
    MethodDeclNode *method = &node->as.method_decl;
    if (source_hir_nominal_defaults(ctx,owner->index).constructor || *next >= ctx->first_closure || method->is_static ||
        method->is_override || method->is_static_constructor || method->is_getter || method->is_setter ||
        method->is_variadic || method->is_operator || method->attr_count || method->type_param_count || method->condition_count ||
        method->borrow_origin_count || method->borrow_origin_syntax || !method->body ||
        method->param_count < 0 || method->param_count > 65536 || (method->is_private && method->is_protected))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constructor declaration contract is not admitted");
    uint32_t index = (*next)++; ctx->function = index;
    XrXirNominalDeclaration *declaration = (XrXirNominalDeclaration *)&ctx->nominals.declarations[owner->index];
    if (declaration->kind == XR_XIR_NOMINAL_STRUCT) declaration->flags |= XR_XIR_NOMINAL_MODULE_LITERAL;
    uint32_t access = method->is_private ? XR_XIR_MEMBER_PRIVATE : method->is_protected ? XR_XIR_MEMBER_PROTECTED : XR_XIR_MEMBER_PUBLIC;
    ctx->identities[index] = (XrXirFunctionIdentity) {owner->module, access ? 0 : declaration->exported, owner->index + 1, access, 0,
        0, XR_XIR_CONSTRUCTOR, 0, 0, 0};
    SourceFunction *body = &ctx->bodies[index]; body->node = node; body->module = owner->module;
    if(!source_method_parameter_scope(ctx,node,&body->parameter_scope))return false;
    if (!source_nominal_function_scope(ctx,index,owner)) return false;
    SourceName symbol = {0}; symbol.name = "init"; symbol.node = node;
    if (!source_query_declare(ctx, &symbol, XR_XIR_SOURCE_FUNCTION, owner->declaration,
        source_query_range(ctx, node, "init"))) return false;
    body->declaration = symbol.declaration;
    if (!source_query_syntax_role(ctx, symbol.declaration, XR_XIR_SOURCE_SYNTAX_METHOD, 0)) return false;
    uint32_t count = (uint32_t)method->param_count;
    body->parameters = count ? source_alloc(ctx, count, sizeof(*body->parameters)) : NULL;
    if (count && !body->parameters) return false;
    ctx->functions[index] = (XrXirFunction) {"init", 4, body->parameters, count, owner->type, NULL, 0, NULL, 0, NULL, 0, XR_XIR_NEVER,0,NULL};
    if (!source_error_clause_type(ctx,node,&method->throws_clause,&ctx->functions[index].error_type)) return false;
    for (uint32_t p = 0; p < count; ++p) {
        XrParamNode *param = method->params[p];
        if ((param->passing_mode != XR_PARAM_READ && param->passing_mode!=XR_PARAM_MOVE) ||
            param->pattern || param->is_rest || source_text_same(ctx, NULL, param->name, "this"))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constructor parameter contract is not admitted");
        if (param->type) { if (!source_type(ctx, param->type, &body->parameters[p])) return false; }
        else {
            XrHirNominalShape shape={0};if(!source_hir_nominal_shape(ctx,node,owner->type,&shape,NULL))return false;
            size_t length=source_text_size(ctx,param->name);if(length>UINT32_MAX)return source_fail(ctx,node,XR_XIR_BUDGET,"field name size exhausted");
            const char *reason=NULL;uint32_t field=0;
            XrXirStatus status=xr_hir_body_nominal_find(ctx->typed_body,shape,(XrXirLiteral){param->name,(uint32_t)length},ctx->request,&field,&reason);
            if(status!=XR_XIR_OK)return source_fail(ctx,node,status,status==XR_XIR_BAD_TYPE?"constructor parameter needs a declaration type":reason);
            XrHirNominalField received={0};if(!source_hir_nominal_field(ctx,node,shape,field,&received))return false;body->parameters[p]=received.type;
        }
        if (body->parameters[p] == XR_XIR_UNIT) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constructor parameter needs a declaration type");
    }
    return source_move_parameters(ctx,index,method->params,count,0)&&
        source_hir_nominal_bind(ctx,node,owner->index,index,XR_HIR_NOMINAL_EXPLICIT,UINT32_MAX)&&source_query_parameters(ctx, symbol.declaration);
}
typedef struct SourceConstructorCall {
    AstNode *reference;
    AstNode **arguments;
    XrCallArgAccess *accesses;
    uint32_t count;
    SourceName *binding, *target;
    XrXirType type;
    SourceExpectedType expected;
} SourceConstructorCall;
/* An omitted constructor list leaves only the declaration's binders open.
 * Type syntax elsewhere still requires a complete nominal application. */
static bool source_constructor_type(SourceContext *ctx,SourceName *target,
    const SourceCallSyntax *syntax,XrXirType *type) {
    if(target && target->kind==SOURCE_NOMINAL && !syntax->type_arg_count &&
        ctx->nominals.declarations[target->index].parameter_count) {
        *type=target->type;return true;
    }
    return source_nominal_apply(ctx,target,syntax->type_args,(uint32_t)syntax->type_arg_count,type);
}
static bool source_constructor_application(SourceContext *ctx,AstNode *node,
    uint32_t declaration,SourceSubstitution substitution,XrXirType *type) {
    return source_instantiation_prove(ctx,node,
        (XrXirDeclarationContext){XR_XIR_CONTEXT_NOMINAL,declaration,0},substitution) &&
        source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,
            .nominal={declaration,substitution.types,substitution.count,NULL,0}},type);
}
typedef struct SourceConstructorEmission {
    XrXirType type;
    uint32_t function,supplied;
    SourceValue *arguments;
    XrHirBodySignature signature;
} SourceConstructorEmission;
static bool source_constructor_emit(SourceContext *ctx,AstNode *node,const SourceConstructorEmission *request,SourceValue *value) {
    XrXirTypeNode nominal=*xr_xir_type_node(&ctx->types,request->type);
    SourceSubstitution substitution={nominal.nominal.arguments,nominal.nominal.argument_count};
    XrXirFunction function=ctx->functions[request->function];
    SourceCallPlan plan={.family=SOURCE_CALL_CONSTRUCTOR,.function_parameters=function.parameters,
        .signature=request->signature,.declaration={.function=request->function}};
    if(!source_hir_call_prepare(ctx,node,&plan,function.parameter_count))return false;
    for(uint32_t p=0;p<function.parameter_count;++p) {
        if(!((ctx)->diagnostic.status == XR_XIR_OK))return false;
        XrXirType expected;
        if(!source_substitute(ctx,&substitution,function.parameters[p],0,&expected))return false;
        if(!request->arguments)return source_fail(ctx,node,XR_XIR_BAD_TYPE,"constructor argument type differs from its declaration");
        XrHirCallUseInput input={.kind=XR_HIR_CALL_FINAL,.formal=p,.argument=p,.check_type=true,.expected=expected,
            .substitution=substitution.types,.substitution_count=substitution.count,
            .failure="constructor argument type differs from its declaration"};
        if(p>=request->supplied) {
            if(!source_hir_default_input(ctx,node,request->function,p,request->arguments[p],&input))return false;
        }
        if(!source_hir_call_use(ctx,node,&plan,input,&request->arguments[p],NULL))return false;
    }
    XrXirInstruction op={XR_XIR_CALL,request->type,{0},{0},request->function,{0}};
    return source_type_arguments(ctx,node,substitution.types,substitution.count,&op)&&
        source_hir_call_group(ctx,plan.signature,op,request->arguments,value);
}
static bool source_constructor_invoke(SourceContext *ctx, AstNode *node,
    const SourceConstructorCall *call, SourceValue *value) {
    XrXirType type = call->type;
    SourceName *binding = call->binding, *target = call->target;
    XrXirTypeNode nominal = *xr_xir_type_node(&ctx->types, type);
    uint32_t index = source_hir_nominal_defaults(ctx,nominal.nominal.declaration).constructor;
    if (!index) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "type has no admitted constructor");
    const XrXirFunction *function = &ctx->functions[index];
    SourceCallSyntax syntax;
    if (!source_call_syntax(ctx,node,&syntax)) return false;
    bool inferred=!syntax.type_arg_count && nominal.nominal.argument_count;
    /* Explicit nominal arguments fix the prefix. Omission instead lets this
     * same argument worker solve the nominal binders, never a second list. */
    syntax.type_args=NULL;syntax.type_arg_count=0;
    if (ctx->identities[index].member_access && ctx->identities[ctx->function].nominal_owner != ctx->identities[index].nominal_owner)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constructor arity or authority mismatch");
    SourceName called = *target;
    if (ctx->bodies[index].checked_library ||
        (ctx->bodies[index].node && ctx->bodies[index].node->type == AST_METHOD_DECL))
        called.declaration = ctx->bodies[index].declaration;
    if (!source_query_reference(ctx, call->reference, binding, &called, XR_XIR_SOURCE_CALL)) return false;
    SourceSubstitution substitution = {nominal.nominal.arguments, nominal.nominal.argument_count};
    SourceCallStorage storage={0};
    if(!source_call_storage(ctx,inferred?substitution.count:0,function->parameter_count,&storage))return false;
    SourceValue *arguments=storage.values;
    if(inferred)substitution.types=storage.types;
    SourceCallPlan plan={.family=SOURCE_CALL_CONSTRUCTOR,.prefix=inferred?(SourceSubstitution){0}:substitution,.substitution=substitution,.function_parameters=function->parameters,.requirement_parameters=NULL,
        .result=type,.expected_result=call->expected,.values=arguments,.parameter_offset=0,.value_offset=0,.parameter_kinds=NULL,.argument_contexts=NULL};
    plan.declaration.function=index;plan.declaration.constructor_accesses=call->accesses;
    plan.declaration.constructor_argument_count=call->count;
    if (!source_call_expand(ctx,node,&syntax,&plan,function->parameter_count) ||
        !source_argument_arity(ctx,node,index,(uint32_t)syntax.arg_count) || !source_call_plan_arguments(ctx,node,&plan,&syntax,NULL)) return false;
    if(!source_constructor_application(ctx,node,nominal.nominal.declaration,substitution,&type))return false;
    for (uint32_t p = (uint32_t)syntax.arg_count; p < function->parameter_count; ++p)
        if (!source_argument_default(ctx, node, index, p, &substitution, &arguments[p])) return false;
    if(inferred && !source_call_conversions(ctx,node,&(SourceCallConversions){SOURCE_CALL_FUNCTION,
        substitution,function->parameters,NULL,arguments,function->parameter_count,0}))return false;
    return source_constructor_emit(ctx,node,&(SourceConstructorEmission){type,index,(uint32_t)syntax.arg_count,arguments,plan.signature},value);
}
static bool source_constructor_call(SourceContext *ctx, AstNode *node, SourceExpectedType context,
    SourceName *binding, SourceName *target, SourceValue *value) {
    SourceCallSyntax actual;XrXirType type;
    if(!source_call_syntax(ctx,node,&actual)||!source_constructor_type(ctx,target,&actual,&type))return false;
    const CallExprNode *syntax = &node->as.call_expr;
    SourceConstructorCall call = {syntax->callee, syntax->arguments, syntax->arg_accesses,
        (uint32_t)syntax->arg_count, binding, target, type,context};
    return source_constructor_invoke(ctx, node, &call, value);
}
static bool source_constructor_new(SourceContext *ctx, AstNode *node, SourceExpectedType context,SourceValue *value) {
    const NewExprNode *syntax = &node->as.new_expr;
    SourceName *binding = ctx->active_expression->binding, *target = binding;
    if (syntax->is_type_namespace || syntax->arg_count < 0 || syntax->arg_count > 65536 ||
        syntax->type_arg_count < 0 || syntax->type_arg_count > 65536 ||
        (syntax->arg_count && !syntax->arguments) || !syntax->class_name)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "nominal construction shape is not admitted");
    if (syntax->module_name) {
        if (!target || target->kind != SOURCE_MODULE)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constructor namespace is not an imported module");
        if (source_mem_module(ctx,target->module) && source_text_same(ctx,node,syntax->class_name,"withSliceMut"))
            return source_buffer_scoped_call(ctx,node,target,value);
        target = imported_declaration(ctx, target, syntax->class_name);
    } else if (target && target->kind == SOURCE_IMPORT)
        target = imported_declaration(ctx, target, target->imported);
    if (ctx->diagnostic.status != XR_XIR_OK) return false;
    if (target && target->kind==SOURCE_FUNCTION)
        return source_function_call(ctx,node,binding,target,context,value);
    XrXirType type;SourceCallSyntax actual;
    if (!source_call_syntax(ctx,node,&actual)||!source_constructor_type(ctx,target,&actual,&type)) return false;
    SourceConstructorCall call = {node, syntax->arguments, syntax->arg_accesses,
        (uint32_t)syntax->arg_count, binding, target, type,context};
    return source_constructor_invoke(ctx, node, &call, value);
}
static bool source_constructor_parameters(SourceContext *ctx) {
    SourceFunction *body=&ctx->bodies[ctx->function];MethodDeclNode *method=&body->node->as.method_decl;
    bool stage=ctx->identities[ctx->function].method_kind==XR_XIR_CONSTRUCTOR_FIELDS;
    for(uint32_t p=0;p<(uint32_t)method->param_count;++p) {
        const XrParamNode *parameter=method->params[p];SourceName *symbol=NULL;
        if(stage) {
            uint32_t item=0;
            if(!xr_hir_items_find(ctx->items,ctx->module,body->parameter_scope,parameter->name,&item))
                return source_fail(ctx,body->node,XR_XIR_BAD_STRUCTURE,"field helper has no constructor parameter declaration");
            const SourceName *declared=xr_hir_items_binding(ctx->items,item);
            if(!declared || !declared->declaration || declared->node!=body->node ||
                declared->type!=body->parameters[p] || declared->index!=p)
                return source_fail(ctx,body->node,XR_XIR_BAD_STRUCTURE,"field helper parameter changed its constructor owner");
            symbol=add_name(ctx,&ctx->locals,parameter->name,body->node);
            if(symbol)symbol->declaration=declared->declaration;
        } else symbol=source_parameter_name(ctx,body->node,parameter,body->parameter_scope);
        if(!symbol)return false;
        symbol->kind=SOURCE_LOCAL;symbol->index=p;symbol->type=body->parameters[p];
        symbol->module=body->module;symbol->binding_function=ctx->function;symbol->is_parameter=true;
        if(!stage) {
            XrXirSourceRange range={ctx->module,parameter->line,parameter->column,parameter->line,0};
            if(parameter->column>0 && source_text_size(ctx,symbol->name)<=(size_t)(INT_MAX-parameter->column))
                range.end_column=parameter->column+(int)source_text_size(ctx,symbol->name);
            if(!source_query_declare(ctx,symbol,XR_XIR_SOURCE_PARAMETER,body->declaration,range))return false;
        }
        symbol->declared_mode=parameter->passing_mode;
        if(symbol->declared_mode==XR_PARAM_MOVE)symbol->mutable=true;
        if(body->constructor_stage) {
            /* A wrapper forwards the incoming word. Only the real AST body
             * materializes a mutable Cell for a consuming formal. */
            ((XrXirSourceDeclaration *)ctx->query.declarations)[symbol->declaration-1].type=source_query_type(ctx,symbol->type);
        } else {
            if(!source_hir_parameter_declared(ctx,symbol))return false;
            source_query_binding_type(ctx,symbol);
        }
    }
    return true;
}

static bool source_constructor_body(SourceContext *ctx) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    MethodDeclNode *method = &body->node->as.method_decl;
    if(body->constructor_stage)return source_class_stage_wrapper(ctx);
    SourceConstructorScan scan = {ctx, 0, false, false};
    if (!source_constructor_scan(method->body, &scan)) return false;
    if (scan.shared && xr_xir_type_is_class(&ctx->types,ctx->nominal_sources[ctx->identities[ctx->function].nominal_owner-1]->type))
        return source_fail(ctx,body->node,XR_XIR_BAD_TYPE,"class constructor cleanup cannot capture unpublished this");
    body->constructor_shared = scan.shared;
    uint32_t owner = ctx->identities[ctx->function].nominal_owner - 1;
    XrXirNominalDeclaration declaration=ctx->nominals.declarations[owner];
    const XrXirNominalDeclaration *decl=&declaration;
    body->constructor_places = decl->field_count ? source_alloc(ctx, decl->field_count, sizeof(*body->constructor_places)) : NULL;
    if (decl->field_count && !body->constructor_places) return false;
    if(!source_constructor_parameters(ctx))return false;
    SourceName *self = add_name(ctx, &ctx->locals, "this", body->node);
    if (!self) return false;
    self->kind = SOURCE_LOCAL; self->type = ctx->nominal_sources[owner]->type; self->construction = true;
    uint32_t receiver_parent=body->declaration;
    if(ctx->identities[ctx->function].method_kind==XR_XIR_CONSTRUCTOR_FIELDS) {
        uint32_t constructor=source_hir_nominal_defaults(ctx,owner).constructor;
        if(!constructor || constructor>=ctx->function_count ||
            ctx->bodies[constructor].constructor_stage!=ctx->function+1)
            return source_fail(ctx,body->node,XR_XIR_BAD_STRUCTURE,"field receiver lost its original constructor declaration");
        receiver_parent=ctx->bodies[constructor].declaration;
    }
    if (!source_query_declare(ctx, self, XR_XIR_SOURCE_BINDING, receiver_parent, source_query_range(ctx, body->node, NULL)) ||
        !source_query_syntax_role(ctx, self->declaration, XR_XIR_SOURCE_SYNTAX_RECEIVER, 0)) return false;
    XrHirBodyBinding self_binding={0};if(!source_hir_binding_register(ctx,self,&self_binding))return false;
    source_query_binding_type(ctx, self);
    for (uint32_t f = 0; f < decl->field_count; ++f) {
        SourceValue place; XrXirType storage;
        if (!source_constructor_storage_type(ctx, self->type, f, &storage)) return false;
        if (!((ctx)->diagnostic.status == XR_XIR_OK) || !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_LOCAL_UNINIT,
            storage, {0}, {0}, (decl->fields[f].flags & XR_XIR_FIELD_MUTABLE) ? 0 : 1, {0}}, &place)) return false;
        body->constructor_places[f] = place.id;

    }
    if(!decl->parent && !source_class_field_defaults(ctx,0))return false;
    return statement(ctx, method->body, false) && (ctx->returned || source_constructor_return(ctx, NULL));
}

#include "xxir_source_class_stages.inc.c"
