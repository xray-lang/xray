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
    XrXirTypeDescription description={0};
    XrXirFieldDescription member={0};
    XrXirStatus status=xr_xir_compile_type_description(&ctx->compile,&ctx->types,receiver,&description);
    if (status==XR_XIR_OK) status=xr_xir_compile_description_field(&ctx->compile,&description,field,&member);
    if (status==XR_XIR_OK && !member.resolved) status=XR_XIR_BAD_STRUCTURE;
    if (status!=XR_XIR_OK) return source_fail(ctx,NULL,status,status==XR_XIR_BUDGET ?
        "constructor field budget exhausted" : "constructor field type is invalid");
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
    const XrXirTypeNode *nominal = xr_xir_type_node(&ctx->types, type);
    uint32_t count = nominal->nominal.field_count;
    SourceValue *fields = count ? source_alloc(ctx, count, sizeof(*fields)) : NULL;
    if (count && !fields) return false;
    for (uint32_t f = 0; f < count; ++f)
        if (!source_constructor_read(ctx, f, &fields[f])) return false;
    return source_recipe_group(ctx, (XrXirInstruction) {xr_xir_type_is_class(&ctx->types,type) ? XR_XIR_CLASS_NEW : XR_XIR_STRUCT_NEW, type, {0}, {0}, 0, {0}}, fields, count, value);
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
    if (!source_constructor_value(ctx, &value) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {value.id, 0}, {0}, 0, {0}}, NULL)) return false;
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
static bool source_constructor_declare(SourceContext *ctx, SourceName *owner, AstNode *node, uint32_t *next) {
    MethodDeclNode *method = &node->as.method_decl;
    if (ctx->nominal_constructors[owner->index] || *next >= ctx->first_closure || method->is_static ||
        method->is_override || method->is_static_constructor || method->is_getter || method->is_setter ||
        method->is_variadic || method->is_operator || method->attr_count || method->type_param_count || method->condition_count ||
        method->borrow_origin_count || method->borrow_origin_syntax || !method->body ||
        method->param_count < 0 || method->param_count > 65536 || (method->is_private && method->is_protected))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constructor declaration contract is not admitted");
    uint32_t index = (*next)++; ctx->function = index; ctx->nominal_constructors[owner->index] = index;
    XrXirNominalDeclaration *declaration = (XrXirNominalDeclaration *)&ctx->nominals.declarations[owner->index];
    if (declaration->kind == XR_XIR_NOMINAL_STRUCT) declaration->flags |= XR_XIR_NOMINAL_MODULE_LITERAL;
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
    ctx->functions[index] = (XrXirFunction) {"init", 4, body->parameters, count, owner->type, NULL, 0, NULL, 0, NULL, 0, XR_XIR_NEVER};
    if (!source_error_clause_type(ctx,node,&method->throws_clause,&ctx->functions[index].error_type)) return false;
    const XrXirNominalDeclaration *decl = &ctx->nominals.declarations[owner->index];
    for (uint32_t p = 0; p < count; ++p) {
        XrParamNode *param = method->params[p];
        if (param->passing_mode != XR_PARAM_READ || param->pattern || param->is_rest || source_text_same(ctx, NULL, param->name, "this"))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constructor parameter contract is not admitted");
        if (param->type) { if (!source_type(ctx, param->type, &body->parameters[p])) return false; }
        else {
            for (uint32_t f = 0; f < decl->field_count; ++f) {
                if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
                if (source_text_size(ctx, param->name) == decl->fields[f].name.length &&
                    source_span_same(ctx, NULL, param->name, decl->fields[f].name.bytes, decl->fields[f].name.length)) body->parameters[p] = decl->fields[f].type;
            }
        }
        if (body->parameters[p] == XR_XIR_UNIT) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constructor parameter needs a declaration type");
    }
    uint32_t access = method->is_private ? XR_XIR_MEMBER_PRIVATE : method->is_protected ? XR_XIR_MEMBER_PROTECTED : XR_XIR_MEMBER_PUBLIC;
    ctx->identities[index] = (XrXirFunctionIdentity) {owner->module, access ? 0 : decl->exported, owner->index + 1, access, 0,
        decl->kind == XR_XIR_NOMINAL_CLASS ? XR_XIR_FUNCTION_NO_SUSPEND : 0, XR_XIR_CONSTRUCTOR, 0, 0, 0};
    return source_query_parameters(ctx, symbol.declaration);
}
typedef struct SourceConstructorCall {
    AstNode *reference;
    AstNode **arguments;
    XrCallArgAccess *accesses;
    uint32_t count;
    SourceName *binding, *target;
    XrXirType type;
} SourceConstructorCall;
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
    uint32_t index = ctx->nominal_constructors[nominal.nominal.declaration];
    if (!index) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "type has no admitted constructor");
    const XrXirFunction *function = &ctx->functions[index];
    SourceCallSyntax syntax;
    if (!source_call_syntax(ctx,node,&syntax)) return false;
    /* The nominal application already proved its type arguments. Constructors
     * have no own binders; the value argument worker sees only that scope. */
    syntax.type_args=NULL;syntax.type_arg_count=0;
    if (ctx->identities[index].member_access && ctx->identities[ctx->function].nominal_owner != ctx->identities[index].nominal_owner)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constructor arity or authority mismatch");
    SourceName called = *target;
    if (ctx->bodies[index].checked_library ||
        (ctx->bodies[index].node && ctx->bodies[index].node->type == AST_METHOD_DECL))
        called.declaration = ctx->bodies[index].declaration;
    if (!source_query_reference(ctx, call->reference, binding, &called, XR_XIR_SOURCE_CALL)) return false;
    SourceSubstitution substitution = {nominal.nominal.arguments, nominal.nominal.argument_count};
    SourceValue *arguments = function->parameter_count ? source_recipe_storage(ctx, function->parameter_count, sizeof(*arguments)) : NULL;
    if (function->parameter_count && !arguments) return false;
    SourceCallPlan plan={.family=SOURCE_CALL_CONSTRUCTOR,.prefix=substitution,.substitution=substitution,.function_parameters=function->parameters,.requirement_parameters=NULL,
        .result=type,.expected_result={0},.values=arguments,.parameter_offset=0,.value_offset=0,.parameter_kinds=NULL,.argument_contexts=NULL};
    plan.declaration.function=index;plan.declaration.constructor_accesses=call->accesses;
    plan.declaration.constructor_argument_count=call->count;
    if (!source_call_expand(ctx,node,&syntax,&plan,function->parameter_count) ||
        !source_argument_arity(ctx,node,index,(uint32_t)syntax.arg_count) || !source_call_plan_arguments(ctx,node,&plan,&syntax,NULL)) return false;
    for (uint32_t p = (uint32_t)syntax.arg_count; p < function->parameter_count; ++p)
        if (!source_argument_default(ctx, node, index, p, &substitution, &arguments[p])) return false;
    return source_constructor_emit(ctx,node,&(SourceConstructorEmission){type,index,(uint32_t)syntax.arg_count,arguments,plan.signature},value);
}
static bool source_constructor_call(SourceContext *ctx, AstNode *node, XrXirType type,
    SourceName *binding, SourceName *target, SourceValue *value) {
    const CallExprNode *syntax = &node->as.call_expr;
    SourceConstructorCall call = {syntax->callee, syntax->arguments, syntax->arg_accesses,
        (uint32_t)syntax->arg_count, binding, target, type};
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
        target = imported_declaration(ctx, target, syntax->class_name);
    } else if (target && target->kind == SOURCE_IMPORT)
        target = imported_declaration(ctx, target, target->imported);
    if (ctx->diagnostic.status != XR_XIR_OK) return false;
    if (target && target->kind==SOURCE_FUNCTION)
        return source_function_call(ctx,node,binding,target,context,value);
    XrXirType type;
    if (!source_nominal_apply(ctx, target, syntax->type_args, (uint32_t)syntax->type_arg_count, &type)) return false;
    SourceConstructorCall call = {node, syntax->arguments, syntax->arg_accesses,
        (uint32_t)syntax->arg_count, binding, target, type};
    return source_constructor_invoke(ctx, node, &call, value);
}
static bool source_constructor_body(SourceContext *ctx) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    MethodDeclNode *method = &body->node->as.method_decl;
    SourceConstructorScan scan = {ctx, 0, false, false};
    if (!source_constructor_scan(method->body, &scan)) return false;
    if (scan.shared && xr_xir_type_is_class(&ctx->types,ctx->functions[ctx->function].result))
        return source_fail(ctx,body->node,XR_XIR_BAD_TYPE,"class constructor cleanup cannot capture unpublished this");
    body->constructor_shared = scan.shared;
    uint32_t owner = ctx->identities[ctx->function].nominal_owner - 1;
    const XrXirNominalDeclaration *decl = &ctx->nominals.declarations[owner];
    body->constructor_places = decl->field_count ? source_alloc(ctx, decl->field_count, sizeof(*body->constructor_places)) : NULL;
    if (decl->field_count && !body->constructor_places) return false;
    for (uint32_t p = 0; p < (uint32_t)method->param_count; ++p) {
        SourceName *symbol = source_parameter_name(ctx,body->node,method->params[p],body->parameter_scope);
        if (!symbol) return false;
        symbol->kind = SOURCE_LOCAL; symbol->index = p; symbol->type = body->parameters[p];
        XrParamNode *parameter = method->params[p];
        XrXirSourceRange range = {ctx->module, parameter->line, parameter->column, parameter->line, 0};
        if (parameter->column > 0 && source_text_size(ctx, symbol->name) <= (size_t)(INT_MAX - parameter->column))
            range.end_column = parameter->column + (int)source_text_size(ctx, symbol->name);
        if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_PARAMETER, body->declaration, range)) return false;
        symbol->declared_mode=parameter->passing_mode;
        if(!source_hir_parameter_declared(ctx,symbol))return false;
        source_query_binding_type(ctx, symbol);
    }
    SourceName *self = add_name(ctx, &ctx->locals, "this", body->node);
    if (!self) return false;
    self->kind = SOURCE_LOCAL; self->type = ctx->functions[ctx->function].result; self->construction = true;
    if (!source_query_declare(ctx, self, XR_XIR_SOURCE_BINDING, body->declaration, source_query_range(ctx, body->node, NULL)) ||
        !source_query_syntax_role(ctx, self->declaration, XR_XIR_SOURCE_SYNTAX_RECEIVER, 0)) return false;
    XrHirBodyBinding self_binding={0};if(!source_hir_binding_register(ctx,self,&self_binding))return false;
    source_query_binding_type(ctx, self);
    for (uint32_t f = 0; f < decl->field_count; ++f) {
        SourceValue place; XrXirType storage;
        if (!source_constructor_storage_type(ctx, self->type, f, &storage)) return false;
        if (!((ctx)->diagnostic.status == XR_XIR_OK) || !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_LOCAL_UNINIT,
            storage, {0}, {0}, (decl->fields[f].flags & XR_XIR_FIELD_MUTABLE) ? 0 : 1, {0}}, &place)) return false;
        body->constructor_places[f] = place.id;
        uint32_t initializer = ctx->nominal_defaults[owner][f];
        if (initializer) {
            const XrXirTypeNode *nominal = xr_xir_type_node(&ctx->types, self->type);
            SourceValue value; XrXirInstruction op = {XR_XIR_CALL, decl->fields[f].type, {0}, {0}, initializer, {0}};
            if (!source_type_arguments(ctx, body->node, nominal->nominal.arguments, nominal->nominal.argument_count, &op) ||
                !source_recipe_record(ctx, op, &value) || !source_recipe_record(ctx, (XrXirInstruction) {xr_xir_type_is_cell(&ctx->types, storage) ? XR_XIR_CELL_LOCAL_WRITE : XR_XIR_LOCAL_WRITE, XR_XIR_UNIT, {place.id, value.id}, {0}, 0, {0}}, NULL)) return false;
        }
    }
    return statement(ctx, method->body, false) && (ctx->returned || source_constructor_return(ctx, NULL));
}
