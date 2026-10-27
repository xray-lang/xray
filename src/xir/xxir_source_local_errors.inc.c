/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_local_errors.inc.c - Declaration-based function-local error checks
 */
static bool source_error_type_ref(SourceContext *ctx,XrTypeRef *type,XrXirType *output) {
    if (!type) return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"error type is missing");
    if (type->kind==XR_TREF_NAMED && type->name && !type->nchildren &&
        source_text_same(ctx,NULL,type->name,"Never") && !source_nominal_name(ctx,type->name)) {
        *output=XR_XIR_NEVER;return true;
    }
    if (!source_type(ctx,type,output)) return false;
    if (!ctx->type_scope.active) {
        XrXirDeclarations declarations;XrXirModule module=source_module_view(ctx,&declarations);
        XrXirProofContext proof={&module,{XR_XIR_CONTEXT_FUNCTION,ctx->function,0}};
        XrXirStatus status=xr_xir_compile_type_markers_prove(&ctx->compile,&proof,*output,XR_XIR_CONSTRAINT_ERROR);
        if (status!=XR_XIR_OK) return source_fail(ctx,NULL,status,"error type must satisfy Error");
    }
    return true;
}
static bool source_error_clause_type(SourceContext *ctx, AstNode *node,
    const XrThrowsClause *clause, XrXirType *output) {
    XrXirType error=XR_XIR_NEVER;
    if (clause->syntax==XR_THROWS_ERASED) error=XR_XIR_ERROR;
    else if (clause->syntax==XR_THROWS_TYPED) {
        XrTypeRef *type=clause->type;
        if (!type) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"throws requires an error type");
        if (!source_error_type_ref(ctx,type,&error)) return false;
    } else if (clause->syntax!=XR_THROWS_OMITTED)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"throws clause is malformed");
    XrXirDeclarations declarations;
    XrXirModule module=source_module_view(ctx,&declarations);
    XrXirProofContext proof={&module,{XR_XIR_CONTEXT_FUNCTION,ctx->function,0}};
    XrXirStatus status=xr_xir_compile_type_markers_prove(&ctx->compile,&proof,error,XR_XIR_CONSTRAINT_ERROR);
    if (status!=XR_XIR_OK) return source_fail(ctx,node,status,"throws type must satisfy Error");
    *output=error;return true;
}
static bool source_local_marked(SourceContext *ctx) {
    if (ctx->bodies[ctx->function].local_handler) return true;
    for (SourceExpressionPlan *plan=ctx->active_expression;plan && plan->owner==ctx->function;plan=plan->parent)
        if (plan->syntax->type==AST_TRY_EXPR && plan->syntax->as.try_expr.mode==XR_TRY_PROPAGATE) return true;
    return false;
}
static bool source_local_accept(SourceContext *ctx, AstNode *node, XrXirType error,
    bool marked, SourceLocalHandler *handlers) {
    if (error==XR_XIR_NEVER || (ctx->linkage_kind==XR_XIR_PROGRAM && ctx->function==ctx->entry_function)) return true;
    if (error==XR_XIR_UNIT) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"call has no declared error type");
    SourceFunction *body=&ctx->bodies[ctx->function];
    if (!marked)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"E0330: throwing call requires try or a try body");
    for (SourceLocalHandler *handler=handlers;handler;handler=handler->previous)
        for (uint32_t i=0;i<handler->count;++i)
            if (handler->complete[i] &&
                (handler->types[i]==XR_XIR_ERROR || handler->types[i]==error)) return true;
    const XrXirTypeNode *iterator=xr_xir_type_node(&ctx->types,ctx->functions[ctx->function].result);
    XrXirType declared=ctx->functions[ctx->function].error_type;
    bool generator=(ctx->identities[ctx->function].promises & XR_XIR_FUNCTION_GENERATOR)!=0;
    if (generator) {
        if (!iterator || iterator->kind!=XR_XIR_TYPE_ITERATOR || iterator->result==XR_XIR_UNIT)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"generator has no declared Iterator error type");
        declared=iterator->result;
    }
    XrXirType *bound=&ctx->functions[ctx->function].error_type;
    if (body->infer_errors && !generator) {
        if (*bound==XR_XIR_NEVER || error==XR_XIR_ERROR) *bound=error;
        else if (error!=*bound && *bound!=XR_XIR_ERROR)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"inferred error union is not yet supported; declare throws(Error)");
        return true;
    }
    if (!xr_xir_error_bound_accepts(error,declared))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,ctx->identities[ctx->function].cleanup_owner ?
            "E0387: defer must handle every business error inside its own body" :
            "E0331: escaping error exceeds the declared throws type; catch it or declare throws");
    return true;
}
static bool source_local_error(SourceContext *ctx, AstNode *node, XrXirType error, bool call) {
    return source_local_accept(ctx,node,error,!call || source_local_marked(ctx),
        ctx->bodies[ctx->function].local_handler);
}
static bool source_local_declared_error(SourceContext *ctx,uint32_t target,
    uint32_t first,uint32_t count,XrXirType *output) {
    if (target>=ctx->function_count) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"call declaration is missing");
    const XrXirGeneric *caller=&ctx->generics[ctx->function];
    if (count!=ctx->generics[target].parameter_count || first>caller->argument_count ||
        count>caller->argument_count-first || (count && !caller->arguments) || (!count && first))
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"call error substitution is invalid");
    SourceSubstitution substitution={count?caller->arguments+first:NULL,count};
    return source_substitute(ctx,&substitution,ctx->functions[target].error_type,0,output);
}
static XrXirType source_local_operand_type(SourceContext *ctx,uint32_t value) {
    const XrXirFunction *function=&ctx->functions[ctx->function];
    const SourceFunction *body=&ctx->bodies[ctx->function];
    if (value<function->parameter_count) return function->parameters ? function->parameters[value] : XR_XIR_UNIT;
    uint32_t ordinal=value-function->parameter_count;
    if (ordinal>=body->count || !body->recipes) return XR_XIR_UNIT;
    const SourceInstructionRecipe *recipe=&body->recipes[ordinal];
    return recipe->owner==ctx->function && recipe->value==value ? recipe->instruction.type : XR_XIR_UNIT;
}
static bool source_local_call(SourceContext *ctx,const XrXirInstruction *op) {
    XrXirType error=XR_XIR_NEVER;uint32_t target=UINT32_MAX;
    if (op->op==XR_XIR_CALL) {
        if (op->immediate<0 || (uint64_t)op->immediate>=ctx->function_count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"call declaration is missing");
        target=(uint32_t)op->immediate;
    } else if (op->op==XR_XIR_CALL_DEFAULT) {
        const XrXirDefaultBinding *binding=NULL;
        XrXirDeclarations declarations;XrXirModule module=source_module_view(ctx,&declarations);
        XrXirStatus status=xr_xir_compile_default_lookup(&ctx->compile,&module,op->targets[0],op->targets[1],&binding);
        if (status!=XR_XIR_OK || !binding || binding->function>=ctx->function_count)
            return source_fail(ctx,NULL,status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:status,"call default declaration is missing");
        target=binding->function;
    } else if (op->op==XR_XIR_CALL_INDIRECT) {
        if (op->immediate<0 || (uint64_t)op->immediate>UINT32_MAX)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"indirect call operand is invalid");
        error=xr_xir_callable_error(&ctx->types,source_local_operand_type(ctx,(uint32_t)op->immediate));
    } else if (op->op==XR_XIR_CALL_REQUIREMENT) {
        if (op->targets[0]>=ctx->interfaces.count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"call requirement declaration is missing");
        const XrXirInterfaceDeclaration *owner=&ctx->interfaces.declarations[op->targets[0]];
        if (op->targets[1]>=owner->method_count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"call requirement member is missing");
        const XrXirInterfaceMethod *method=&owner->methods[op->targets[1]];
        const XrXirTypeNode *signature=xr_xir_callable_signature(&ctx->types,method->signature);
        uint32_t first=op->type_arguments[0],count=op->type_arguments[1];
        const XrXirGeneric *caller=&ctx->generics[ctx->function];
        if (!signature || owner->parameter_count>UINT32_MAX-method->own_parameter_count ||
            count!=owner->parameter_count+method->own_parameter_count || first>caller->argument_count ||
            count>caller->argument_count-first || (count && !caller->arguments) || (!count && first))
            return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"call requirement error substitution is invalid");
        XrXirType declared=signature->element;
        SourceSubstitution substitution={count?caller->arguments+first:NULL,count};
        if (!source_substitute(ctx,&substitution,declared,0,&error)) return false;
        if (error!=XR_XIR_NEVER && ctx->bodies[ctx->function].error_context)
            return source_fail(ctx,ctx->active_expression ? ctx->active_expression->syntax : NULL,
                XR_XIR_BAD_TYPE,"throwing interface calls in catch regions are not yet supported");
    } else return true;
    AstNode *node=ctx->active_expression && ctx->active_expression->owner==ctx->function ?
        ctx->active_expression->syntax : ctx->bodies[ctx->function].node;
    for (SourceExpressionPlan *plan=ctx->active_expression;plan && plan->owner==ctx->function;plan=plan->parent)
        if (plan->syntax->type==AST_TRY_EXPR) {node=plan->syntax;break;}
    if (target!=UINT32_MAX) {
        SourceFunction *body=&ctx->bodies[ctx->function];
        if (ctx->bodies[target].infer_initializer) {
            if (body->infer_errors && !body->infer_initializer)
                return source_fail(ctx,node,XR_XIR_BAD_TYPE,
                    "inferred closure depends on an initialization bound; declare throws explicitly");
            SourcePendingError *site=source_alloc(ctx,1,sizeof(*site));
            if (!site) return false;
            bool generated=(body->infer_initializer && body->node &&
                (body->node->type==AST_STRUCT_DECL || body->node->type==AST_CLASS_DECL)) ||
                (ctx->identities[ctx->function].method_kind==XR_XIR_CONSTRUCTOR &&
                 ctx->bodies[target].node && ctx->bodies[target].node->type==AST_FIELD_DECL);
            *site=(SourcePendingError){body->pending_errors,body->local_handler,node,target,
                op->type_arguments[0],op->type_arguments[1],generated || source_local_marked(ctx)};
            body->pending_errors=site;return true;
        }
        if (!source_local_declared_error(ctx,target,op->type_arguments[0],op->type_arguments[1],&error)) return false;
    }
    return source_local_error(ctx,node,error,true);
}
typedef struct SourceInitializationFrame {
    uint32_t function;
    SourcePendingError *next;
} SourceInitializationFrame;
/* Only generated initializer/default bodies have inferred dependencies here.
 * Ordinary, imported and explicit init declarations are authoritative leaves.
 * Cyclic generated inference is not implemented; no bottom guess is published. */
static bool source_local_errors_finish(SourceContext *ctx) {
    uint32_t saved_function=ctx->function,saved_module=ctx->module;
    unsigned char *state=source_alloc(ctx,ctx->function_count,sizeof(*state));
    SourceInitializationFrame *stack=source_alloc(ctx,ctx->function_count,sizeof(*stack));
    if (!state || !stack) return false;
    for (uint32_t f=0;f<ctx->function_count;++f) {
        if (!ctx->bodies[f].infer_initializer || state[f]) continue;
        uint32_t depth=1;state[f]=1;
        stack[0]=(SourceInitializationFrame){f,ctx->bodies[f].pending_errors};
        while (depth) {
            SourceInitializationFrame *frame=&stack[depth-1];
            uint32_t current=frame->function;
            SourceFunction *body=&ctx->bodies[current];
            ctx->function=current;ctx->module=body->module;
            if (frame->next) {
                SourcePendingError *site=frame->next;frame->next=site->next;
                if (site->target>=ctx->function_count)
                    return source_fail(ctx,site->node,XR_XIR_BAD_STRUCTURE,"initialization dependency is invalid");
                if (!ctx->bodies[site->target].infer_initializer || state[site->target]==2) continue;
                if (state[site->target]==1)
                    return source_fail(ctx,site->node,XR_XIR_BAD_TYPE,"generated initialization error cycles are not yet supported");
                if (depth>=ctx->function_count)
                    return source_fail(ctx,site->node,XR_XIR_BAD_STRUCTURE,"initialization dependency depth is invalid");
                state[site->target]=1;
                stack[depth++]=(SourceInitializationFrame){site->target,ctx->bodies[site->target].pending_errors};
                continue;
            }
            for (SourcePendingError *site=body->pending_errors;site;site=site->next) {
                XrXirType error;
                if (!source_local_declared_error(ctx,site->target,site->first_argument,site->argument_count,&error) ||
                    !source_local_accept(ctx,site->node,error,true,site->handler)) return false;
            }
            state[current]=2;--depth;
        }
    }
    for (uint32_t f=0;f<ctx->function_count;++f) {
        SourceFunction *body=&ctx->bodies[f];ctx->function=f;ctx->module=body->module;
        for (SourcePendingError *site=body->pending_errors;site;site=site->next) {
            XrXirType error;
            if (!source_local_declared_error(ctx,site->target,site->first_argument,site->argument_count,&error) ||
                !source_local_accept(ctx,site->node,error,site->marked,site->handler)) return false;
        }
    }
    ctx->function=saved_function;ctx->module=saved_module;return true;
}
