/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_task_call.inc.c - Direct Task creation and owned await successors
 *
 * KEY CONCEPT:
 *   Creation prepares ordinary arguments once; awaiting never executes a synchronous call.
 */
static bool source_go_target(SourceContext *ctx, AstNode *node, SourceName **binding, SourceName **target) {
    GoExprNode *go = &node->as.go_expr;
    AstNode *call = go->expr;
    if (go->name || go->link_mode || go->spawn_kind != XR_SPAWN_COROUTINE || !call || call->type != AST_CALL_EXPR)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "go requires an ordinary direct function call");
    AstNode *callee = call->as.call_expr.callee;
    for (uint32_t depth = 0; callee && callee->type == AST_GROUPING; ++depth) {
        if (depth >= 128) return source_fail(ctx, node, XR_XIR_BUDGET, "go declaration path depth exhausted");
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        callee = callee->as.grouping;
    }
    *binding = NULL; *target = NULL;
    if (callee && callee->type == AST_VARIABLE) {
        *binding = visible_name(ctx, callee->as.variable.name); *target = *binding;
        if (*target && (*target)->kind == SOURCE_IMPORT)
            *target = imported_declaration(ctx, *target, (*target)->imported);
    } else if (callee && callee->type == AST_MEMBER_ACCESS && callee->as.member_access.object &&
        callee->as.member_access.object->type == AST_VARIABLE) {
        *binding = visible_name(ctx, callee->as.member_access.object->as.variable.name);
        if (*binding && (*binding)->kind == SOURCE_MODULE)
            *target = imported_declaration(ctx, *binding, callee->as.member_access.name);
    }
    if (*target && (*target)->kind != SOURCE_FUNCTION) *target = NULL;
    return true;
}
/* Resolve the evaluated value, never a callable type or a declaration name.
 * Copy its physical environment before argument evaluation grows recipe pools. */
static bool source_go_value_arguments(SourceContext *ctx, AstNode *call, uint32_t *target,
    SourceDirectArguments *output) {
    SourceValue callee;
    if (call->as.call_expr.type_arg_count || !expression(ctx,call->as.call_expr.callee,&callee))
        return source_fail(ctx,call,XR_XIR_BAD_TYPE,"go value requires an instantiated function reference");
    uint32_t id=callee.id;
    const SourceFunction *body=&ctx->bodies[ctx->function];
    uint32_t parameters=ctx->functions[ctx->function].parameter_count;
    XrXirInstruction producer={0};
    for (uint32_t depth=0;;++depth) {
        if (!(depth%64u) && !source_request_poll(ctx,call)) return false;
        if (id<parameters || id-parameters>=body->count)
            return source_fail(ctx,call,XR_XIR_BAD_TYPE,"go value has no statically owned function reference");
        producer=body->recipes[id-parameters].instruction;
        if (producer.op!=XR_XIR_COPY && producer.op!=XR_XIR_OWNED_RETAIN && producer.op!=XR_XIR_FUNCTION_WEAKEN) break;
        if (producer.args[0]>=id)
            return source_fail(ctx,call,XR_XIR_BAD_STRUCTURE,"go reference alias is not an earlier value");
        id=producer.args[0];
    }
    if (producer.op!=XR_XIR_FUNCTION_REF || producer.immediate<0 || (uint64_t)producer.immediate>=ctx->function_count)
        return source_fail(ctx,call,XR_XIR_BAD_TYPE,"go value requires its actual function reference producer");
    *target=(uint32_t)producer.immediate;
    XrXirFunction function=ctx->functions[*target];
    const XrXirTypeNode *found=xr_xir_callable_signature(&ctx->types,callee.type);
    if (!found) return source_fail(ctx,call,XR_XIR_BAD_TYPE,"go reference requires a callable signature");
    XrXirTypeNode signature=*found;
    uint32_t captures=producer.args[1],count=function.parameter_count;
    XrXirGeneric generic=ctx->generics[ctx->function];
    if (captures>count || signature.parameter_count!=count-captures ||
        producer.args[0]>body->operand_count || captures>body->operand_count-producer.args[0] ||
        producer.type_arguments[0]>generic.argument_count ||
        producer.type_arguments[1]>generic.argument_count-producer.type_arguments[0] ||
        producer.type_arguments[1]!=ctx->generics[*target].parameter_count)
        return source_fail(ctx,call,XR_XIR_BAD_STRUCTURE,"go reference environment disagrees with its declaration");
    if (signature.flags&XR_XIR_CALLABLE_REST)
        return source_fail(ctx,call,XR_XIR_UNSUPPORTED,"go rest function values are not yet admitted");
    SourceCallStorage storage={0};
    if (!source_call_storage(ctx,producer.type_arguments[1],count,&storage)) return false;
    SourceSubstitution substitution={storage.types,producer.type_arguments[1]};
    if (substitution.count && !source_copy_bytes(ctx,call,storage.types,generic.arguments+producer.type_arguments[0],
        substitution.count*sizeof(*storage.types))) return false;
    for (uint32_t p=0;p<count;++p) {
        if (!(p%64u) && !source_request_poll(ctx,call)) return false;
        XrXirType type;
        if (!source_substitute(ctx,&substitution,function.parameters[p],0,&type)) return false;
        if (p<captures) {
            uint32_t capture=body->operands[producer.args[0]+p];
            if (capture>=parameters && capture-parameters>=body->count)
                return source_fail(ctx,call,XR_XIR_BAD_STRUCTURE,"go capture has no actual producer");
            XrXirType actual=capture<parameters ? ctx->functions[ctx->function].parameters[capture] :
                body->recipes[capture-parameters].instruction.type;
            if (actual!=type) return source_fail(ctx,call,XR_XIR_BAD_TYPE,"go capture type disagrees with its declaration");
            storage.values[p]=(SourceValue){.id=capture,.type=actual};
        }
        else if (signature.parameters[p-captures].type!=type || signature.parameters[p-captures].mode!=XR_PARAM_READ)
            return source_fail(ctx,call,XR_XIR_BAD_TYPE,"go callable tail disagrees with its physical declaration");
    }
    XrXirType result,error;
    if (!source_substitute(ctx,&substitution,function.result,0,&result) ||
        !source_substitute(ctx,&substitution,function.error_type,0,&error)) return false;
    if (result!=signature.result || error!=signature.element)
        return source_fail(ctx,call,XR_XIR_BAD_TYPE,"go callable result disagrees with its declaration");
    SourceCallSyntax syntax;
    SourceCallPlan plan={.family=SOURCE_CALL_FUNCTION,.prefix=substitution,.substitution=substitution,
        .function_parameters=function.parameters,.result=function.result,.values=storage.values,
        .parameter_offset=captures};
    plan.declaration.function=*target;plan.declaration.callable=callee.type;plan.declaration.callee_value=callee.id;
    if (!source_hir_call_prepare(ctx,call,&plan,count)) return false;
    plan.value_offset=captures;
    if (!source_call_syntax(ctx,call,&syntax) || !source_call_expand(ctx,call,&syntax,&plan,count-captures)) return false;
    if ((uint32_t)syntax.arg_count!=count-captures)
        return source_fail(ctx,call,XR_XIR_BAD_TYPE,"go value requires its exact exposed arguments");
    if (!source_call_plan_arguments(ctx,call,&plan,&syntax,NULL) ||
        !source_instantiation_prove(ctx,call,(XrXirDeclarationContext){XR_XIR_CONTEXT_FUNCTION,*target,0},substitution)) return false;
    SourceCallConversions conversions={SOURCE_CALL_FUNCTION,substitution,function.parameters,NULL,storage.values,count,0};
    if (!source_call_conversions(ctx,call,&conversions) || !source_hir_call_commit(ctx,call,&plan,storage.values,count,count)) return false;
    *output=(SourceDirectArguments){substitution,storage.values,count,result,0,plan.signature};return true;
}
static bool source_go_call(SourceContext *ctx, AstNode *node, SourceExpectedType context, SourceValue *value) {
    SourceName *binding, *target;
    if (!source_go_target(ctx, node, &binding, &target)) return false;
    AstNode *call = node->as.go_expr.expr;
    SourceCallSyntax syntax;
    if (!source_call_syntax(ctx, call, &syntax)) return false;
    for (int a = 0; a < syntax.arg_count; ++a) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (syntax.arg_accesses && syntax.arg_accesses[a] != XR_CALL_ARG_PLAIN)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "go argument access mode is not admitted");
    }
    SourceExpectedType child = ctx->active_expression->task_element_context;
    XrXirType expected = context.type;
    for (uint32_t depth = 0; context.present && xr_xir_type_is_nullable(&ctx->types, expected); ++depth) {
        if (depth >= 128) return source_fail(ctx, node, XR_XIR_BUDGET, "go result context depth exhausted");
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        expected = xr_xir_nullable_element(&ctx->types, expected);
    }
    const XrXirTypeNode *task_context = context.present ? xr_xir_type_node(&ctx->types, expected) : NULL;
    if (task_context && task_context->kind == XR_XIR_TYPE_TASK)
        child = (SourceExpectedType) {true, task_context->element, context.infer_result, false};
    SourceDirectArguments prepared = {0};
    uint32_t function=target ? target->index : UINT32_MAX;
    if (target) {
        SourceDirectRequest direct = {function, {0}, NULL, child};
        if (!source_direct_arguments(ctx, call, &direct, &prepared)) return false;
    } else if (!source_go_value_arguments(ctx,call,&function,&prepared)) return false;
    XrXirType error;
    if (!source_substitute(ctx,&prepared.substitution,ctx->functions[function].error_type,0,&error)) return false;
    XrXirDeclarations declarations;XrXirModule module=source_module_view(ctx,&declarations);
    XrXirProofContext proof={&module,{XR_XIR_CONTEXT_FUNCTION,function,0}};
    XrXirStatus status=xr_xir_compile_type_markers_prove(&ctx->compile,&proof,
        ctx->functions[function].error_type,XR_XIR_CONSTRAINT_SENDABLE);
    if (status!=XR_XIR_OK)
        return source_fail(ctx,call,status,"Task worker declared error type must satisfy Sendable");
    XrXirInstruction op = {XR_XIR_GO, XR_XIR_UNIT, {0}, {0}, function, {0}};
    return source_task_type(ctx, call, prepared.result, error, &op.type) &&
        source_type_arguments(ctx, call, prepared.substitution.types, prepared.substitution.count, &op) &&
        (!target || source_query_reference(ctx, call->as.call_expr.callee, binding, target, XR_XIR_SOURCE_CALL)) &&
        source_hir_call_group(ctx,prepared.signature,op,prepared.values,value);
}
static bool source_task_result_hint(SourceContext *ctx, SourceExpressionPlan *plan,
    SourceExpectedType context, uint32_t depth) {
    if (!context.present) return true;
    if (!plan) return source_fail(ctx, NULL, XR_XIR_BAD_STRUCTURE, "Task result plan is missing");
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    if (depth >= 128) return source_fail(ctx, plan->syntax, XR_XIR_BUDGET, "Task result inference depth exhausted");
    switch (plan->syntax->type) {
    case AST_GO_EXPR: case AST_CALL_EXPR: case AST_MATCH_EXPR:
        plan->task_element_context = context;
        return true;
    case AST_GROUPING:
        return !plan->left || source_task_result_hint(ctx, plan->left, context, depth + 1);
    case AST_TERNARY:
        return source_task_result_hint(ctx, plan->left, context, depth + 1) &&
            source_task_result_hint(ctx, plan->right, context, depth + 1);
    default: return true;
    }
}
static bool source_task_expression(SourceContext *ctx, AstNode *node, SourceExpectedType expected,
    SourceExpectedType task_context, SourceValue *value) {
    SourceExpressionPlan *plan = source_plan_collect(ctx, node, expected);
    return plan && source_task_result_hint(ctx, plan, task_context, 0) && source_plan_complete(ctx, plan, value);
}
static bool source_task_await(SourceContext *ctx, AstNode *node, SourceExpectedType context, SourceValue *value) {
    AwaitExprNode *await = &node->as.await_expr;
    if (!await->expr || await->timeout || await->into || await->is_any || await->is_all || await->is_any_success)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "await requires one Task without aggregation or timeout");
    SourceValue task;
    /* Context is evidence for an ordinary generic call, never a conversion
     * of an existing invariant Task handle to a different element type. */
    if (!source_task_expression(ctx, await->expr, (SourceExpectedType){0}, context, &task)) return false;
    const XrXirTypeNode *type = xr_xir_type_node(&ctx->types, task.type);
    if (!type || type->kind != XR_XIR_TYPE_TASK)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "await requires an admitted Task value");
    XrXirType element = type->element,declared_error=type->result;
    if (type->result==XR_XIR_UNIT)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Task error declaration is not yet supported");
    XrXirType cancelled=xr_xir_cancelled_error_type(&ctx->types),error_type=XR_XIR_UNIT;
    if (cancelled==XR_XIR_UNIT) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"await requires the governed Cancelled declaration");
    XrXirCallableParameter errors[2]={{declared_error,0},{cancelled,0}};
    if (!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_UNION,.parameters=errors,.parameter_count=2},&error_type) ||
        !source_local_error(ctx,node,error_type,true)) return false;
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!body->block_count && !begin_block(ctx)) return false;
    uint32_t origin = body->count, error = body->block_count;
    if (error == UINT32_MAX) return source_fail(ctx, node, XR_XIR_BUDGET, "await block identity exhausted");
    XrXirInstruction op = {XR_XIR_TASK_AWAIT, XR_XIR_UNIT, {task.id, 0}, {error + 1, error}, 0, {0}};
    if (!source_recipe_append(ctx, op, NULL) || !begin_block(ctx)) return false;
    SourceValue caught;
    if (!source_recipe_append(ctx, (XrXirInstruction) {XR_XIR_INVOKE_ERROR, XR_XIR_ERROR, {0}, {0}, origin, {0}}, &caught))
        return false;
    if (body->error_context) {
        if (!source_error_edge(ctx, caught) || !source_handler_site(ctx,origin,caught.id)) return false;
    } else if (!source_recipe_append(ctx, (XrXirInstruction) {XR_XIR_THROW, XR_XIR_UNIT, {caught.id, 0}, {0}, 0, {0}}, NULL))
        return false;
    if (!begin_block(ctx)) return false;
    if (element == XR_XIR_UNIT) { *value = (SourceValue){.id=UINT32_MAX, .type=XR_XIR_UNIT,.place={0}}; return true; }
    return source_recipe_append(ctx,
        (XrXirInstruction) {XR_XIR_INVOKE_RESULT, element, {0}, {0}, origin, {0}}, value);
}

static bool source_task_result_type(SourceContext *ctx,XrXirType element,XrXirType error,XrXirType *type) {
    SourceName *symbol=source_prelude_name(ctx,"TaskResult");
    if (!symbol || symbol->kind!=SOURCE_NOMINAL ||
        ctx->nominals.declarations[symbol->index].native.native_id!=XR_NATIVE_DECLARATION_TASK_RESULT)
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"Task outcome requires the governed TaskResult declaration");
    XrXirType *arguments=source_alloc(ctx,2,sizeof(*arguments));if (!arguments) return false;
    arguments[0]=element;arguments[1]=error;
    return source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,
        .nominal={symbol->index,arguments,2,NULL,0}},type);
}
static bool source_task_member_reference(SourceContext *ctx,AstNode *node,
    const XrNativeMemberDeclaration *member) {
    if (!member->id || member->id>3)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Task member identity is not admitted");
    uint32_t slot=member->id-1;
    if (!ctx->task_members[slot]) {
        XrXirType result=XR_XIR_UNIT;
        if (member->result!=XR_NATIVE_TERM_UNIT) {
            if (!source_task_result_type(ctx,(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,
                (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1),&result)) return false;
            if (member->result==XR_NATIVE_TERM_NULLABLE_TASK_RESULT &&
                !source_nullable_type(ctx,result,&result)) return false;
        }
        SourceName symbol={0};symbol.name=source_owned_text(ctx,member->name);
        if (!symbol.name || !source_query_declare(ctx,&symbol,XR_XIR_SOURCE_MEMBER,ctx->task_declaration,
            (XrXirSourceRange){ctx->task_module,(int)member->line,(int)member->column,
                (int)member->line,(int)(member->column+source_text_size(ctx,member->name))})) return false;
        ctx->task_members[slot]=symbol.declaration;
        const char *signature=source_owned_text(ctx,member->signature);
        if (!signature) return false;
        XrXirSourceDeclaration *record=(XrXirSourceDeclaration *)&ctx->query.declarations[symbol.declaration-1];
        record->native_identity=member->id;record->signature=signature;
        record->type=(XrXirSourceType){result,member->result==XR_NATIVE_TERM_UNIT?0:ctx->task_declaration,true};
        record->exported=true;record->mutable=true;
        record->generic_parent=ctx->task_declaration;record->generic_parent_count=2;
    }
    return source_query_target_token_reference(ctx,node,ctx->task_members[slot],XR_XIR_SOURCE_CALL);
}
static bool source_task_member_call(SourceContext *ctx,AstNode *node,SourceValue *value) {
    CallExprNode *call=&node->as.call_expr;
    AstNode *callee=call->callee;
    if (!source_native_task_declaration(ctx)) return false;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_TASK);
    const XrNativeMemberDeclaration *member=source_native_member(ctx,native,callee->as.member_access.name);
    if (!member || !member->is_public || !member->is_method || member->is_static || !member->lowered ||
        member->receiver!=XR_NATIVE_RECEIVER_REF ||
        (member->operation!=XR_NATIVE_OPERATION_TASK_CANCEL && member->operation!=XR_NATIVE_OPERATION_TASK_POLL &&
         member->operation!=XR_NATIVE_OPERATION_TASK_AWAIT_RESULT) || member->parameter_count ||
        call->arg_count || call->type_arg_count || call->default_arg_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Task member requires its admitted zero-argument recipe");
    SourceValue cell,task;
    if (!source_reference_place(ctx,callee->as.member_access.object,&cell)) return false;
    XrXirType type=xr_xir_cell_element(&ctx->types,cell.type);
    const XrXirTypeNode *shape=xr_xir_type_node(&ctx->types,type);
    if (!shape || shape->kind!=XR_XIR_TYPE_TASK)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Task member requires a mutable Task binding");
    XrXirType element=shape->element,error=shape->result;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,type,{cell.id,0},{0},0,{0}},&task)) return false;
    if (member->operation==XR_NATIVE_OPERATION_TASK_CANCEL) {
        if (member->result!=XR_NATIVE_TERM_UNIT)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Task cancellation result identity is invalid");
        return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_TASK_CANCEL,XR_XIR_UNIT,{task.id,0},{0},0,{0}},value) &&
            source_task_member_reference(ctx,callee,member);
    }
    XrXirType result=XR_XIR_UNIT;
    if (!source_task_result_type(ctx,element,error,&result)) return false;
    if (member->operation==XR_NATIVE_OPERATION_TASK_POLL) {
        if (member->result!=XR_NATIVE_TERM_NULLABLE_TASK_RESULT)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Task poll result identity is invalid");
        if (!source_nullable_type(ctx,result,&result) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_TASK_POLL,result,{task.id,0},{0},0,{0}},value)) return false;
    } else {
        if (member->result!=XR_NATIVE_TERM_TASK_RESULT)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Task outcome result identity is invalid");
        SourceFunction *body=&ctx->bodies[ctx->function];
        if (!body->block_count && !begin_block(ctx)) return false;
        uint32_t origin=body->count,next=body->block_count;
        if (!source_recipe_append(ctx,(XrXirInstruction){XR_XIR_TASK_AWAIT_RESULT,XR_XIR_UNIT,{task.id,0},{next,0},0,{0}},NULL) ||
            !begin_block(ctx) || !source_recipe_append(ctx,
                (XrXirInstruction){XR_XIR_INVOKE_RESULT,result,{0},{0},origin,{0}},value)) return false;
    }
    return source_task_member_reference(ctx,callee,member);
}
