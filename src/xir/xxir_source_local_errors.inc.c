/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_local_errors.inc.c - Declaration-based function-local error checks
 */
static bool source_parameter_constraints(SourceContext *ctx, AstNode *node,
    const XrGenericParam *parameter, XrXirConstraint *output);
static bool source_error_prove(SourceContext *ctx,XrXirType type) {
    if (ctx->type_scope.active) {
        uint32_t id=(uint32_t)type;
        if (id>=XR_XIR_TYPE_PARAMETER_BASE && id<XR_XIR_TYPE_PARAMETER_LIMIT) {
            uint32_t p=id-XR_XIR_TYPE_PARAMETER_BASE;XrXirConstraint constraint={0};
            if (p>=ctx->type_scope.count || !ctx->type_scope.parameters)
                return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"error type parameter has no declaration scope");
            if (!source_parameter_constraints(ctx,ctx->type_scope.node,ctx->type_scope.parameters[p],&constraint)) return false;
            if (!(constraint.markers & XR_XIR_CONSTRAINT_ERROR))
                return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"error type parameter must satisfy Error");
            return true;
        }
        if (type==XR_XIR_NEVER || type==XR_XIR_ERROR || xr_xir_type_is_enum(&ctx->types,type)) return true;
        const XrXirTypeNode *node=xr_xir_type_node(&ctx->types,type);
        if (node && node->kind==XR_XIR_TYPE_UNION) {
            uint32_t count=node->parameter_count;
            const XrXirCallableParameter *members=node->parameters;
            for (uint32_t p=0;p<count;++p)
                if (!source_error_prove(ctx,members[p].type)) return false;
            return true;
        }
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"error type must satisfy Error");
    }
    XrXirDeclarations declarations;XrXirModule module=source_module_view(ctx,&declarations);
    XrXirProofContext proof={&module,{XR_XIR_CONTEXT_FUNCTION,ctx->function,0}};
    XrXirStatus status=xr_xir_compile_type_markers_prove(&ctx->compile,&proof,type,XR_XIR_CONSTRAINT_ERROR);
    if (status==XR_XIR_OK) status=xr_xir_compile_type_access(&ctx->compile,&module,ctx->function,type);
    return status==XR_XIR_OK || source_fail(ctx,NULL,status,"error type must satisfy Error");
}
static bool source_error_type_ref(SourceContext *ctx,XrTypeRef *type,XrXirType *output) {
    if (!type) return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"error type is missing");
    if (type->kind==XR_TREF_NAMED && type->name && !type->nchildren &&
        source_text_same(ctx,NULL,type->name,"Never") && !source_nominal_name(ctx,type->name)) {
        *output=XR_XIR_NEVER;return true;
    }
    if (type->kind==XR_TREF_UNION) {
        if (type->nchildren<2 || !type->children) return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"error union is malformed");
        XrXirCallableParameter *members=source_alloc(ctx,(uint32_t)type->nchildren,sizeof(*members));
        if (!members) return false;
        /* Prove every original member before interning may select Error. */
        for (int p=0;p<type->nchildren;++p)
            if (!source_error_type_ref(ctx,type->children[p],&members[p].type)) return false;
        return source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_UNION,
            .parameters=members,.parameter_count=(uint32_t)type->nchildren},output);
    }
    if (!source_type(ctx,type,output) || !source_error_prove(ctx,*output)) return false;
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
    if (!source_error_prove(ctx,error)) return false;
    *output=error;return true;
}
static bool source_local_marked(SourceContext *ctx) {
    if (ctx->bodies[ctx->function].local_handler) return true;
    for (SourceExpressionPlan *plan=ctx->active_expression;plan && plan->owner==ctx->function;plan=plan->parent)
        if (plan->syntax->type==AST_TRY_EXPR && (plan->syntax->as.try_expr.mode==XR_TRY_PROPAGATE ||
            plan->syntax->as.try_expr.mode==XR_TRY_ASSERT ||
            plan->syntax->as.try_expr.mode==XR_TRY_OPTIONAL)) return true;
    return false;
}
static bool source_local_escape(SourceContext *ctx,AstNode *node,XrXirType error,
    SourceLocalHandler *handlers,bool asserted,XrXirType *output) {
    if (error==XR_XIR_UNIT) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"call has no declared error type");
    if (error==XR_XIR_NEVER || asserted) { *output=XR_XIR_NEVER;return true; }
    const XrXirTypeNode *error_node=xr_xir_type_node(&ctx->types,error);
    bool union_error=error_node && error_node->kind==XR_XIR_TYPE_UNION;
    uint32_t member_count=union_error ? error_node->parameter_count : 1,escaping=0;
    XrXirCallableParameter *remaining=source_alloc(ctx,member_count,sizeof(*remaining));
    if (!remaining) return false;
    for (uint32_t p=0;p<member_count;++p) {
        XrXirType member=union_error ? error_node->parameters[p].type : error;
        bool handled=false;
        for (SourceLocalHandler *handler=handlers;handler;handler=handler->previous)
            for (uint32_t i=0;i<handler->count;++i)
                handled|=handler->complete[i] && (handler->types[i]==XR_XIR_ERROR || handler->types[i]==member);
        if (!handled) remaining[escaping++]=(XrXirCallableParameter){member,0};
    }
    if (!escaping) { *output=XR_XIR_NEVER;return true; }
    return source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_UNION,
        .parameters=remaining,.parameter_count=escaping},output);
}
static bool source_local_accept(SourceContext *ctx, AstNode *node, XrXirType error,
    bool marked, SourceLocalHandler *handlers, bool asserted) {
    if (error!=XR_XIR_NEVER && error!=XR_XIR_UNIT && !marked)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"E0330: throwing call requires try or a try body");
    if (!source_local_escape(ctx,node,error,handlers,asserted,&error)) return false;
    if (error==XR_XIR_NEVER) return true;
    SourceFunction *body=&ctx->bodies[ctx->function];
    const XrXirTypeNode *iterator=xr_xir_type_node(&ctx->types,ctx->functions[ctx->function].result);
    XrXirType declared=ctx->functions[ctx->function].error_type;
    bool generator=(ctx->identities[ctx->function].promises & XR_XIR_FUNCTION_GENERATOR)!=0;
    if (generator) {
        if (!iterator || iterator->kind!=XR_XIR_TYPE_ITERATOR || iterator->result==XR_XIR_UNIT)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"generator has no declared Iterator error type");
        declared=iterator->result;
    }
    XrXirType *bound=body->infer_initializer ? &body->initializer_seed :
        &ctx->functions[ctx->function].error_type;
    if (body->infer_errors && !generator) {
        XrXirCallableParameter members[2]={{*bound,0},{error,0}};
        if (!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_UNION,
            .parameters=members,.parameter_count=2},bound)) return false;
        return true;
    }
    if (!xr_xir_error_bound_accepts(&ctx->types,error,declared))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,ctx->identities[ctx->function].cleanup_owner ?
            "E0387: defer must handle every business error inside its own body" :
            "E0331: escaping error exceeds the declared throws type; catch it or declare throws");
    return true;
}
static bool source_local_error(SourceContext *ctx, AstNode *node, XrXirType error, bool call) {
    return source_local_accept(ctx,node,error,!call || source_local_marked(ctx),
        ctx->bodies[ctx->function].local_handler,
        ctx->bodies[ctx->function].error_context && ctx->bodies[ctx->function].error_context->asserted);
}
static bool source_local_substitute_error(SourceContext *ctx,uint32_t target,
    uint32_t first,uint32_t count,XrXirType bound,XrXirType *output) {
    if (target>=ctx->function_count) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"call declaration is missing");
    const XrXirGeneric *caller=&ctx->generics[ctx->function];
    if (count!=ctx->generics[target].parameter_count || first>caller->argument_count ||
        count>caller->argument_count-first || (count && !caller->arguments) || (!count && first))
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"call error substitution is invalid");
    SourceSubstitution substitution={count?caller->arguments+first:NULL,count};
    if (bound==XR_XIR_UNIT) return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"call has no declared error type");
    return source_substitute(ctx,&substitution,bound,0,output);
}
static bool source_local_declared_error(SourceContext *ctx,uint32_t target,
    uint32_t first,uint32_t count,XrXirType *output) {
    if (target>=ctx->function_count) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"call declaration is missing");
    return source_local_substitute_error(ctx,target,first,count,ctx->functions[target].error_type,output);
}
static bool source_initialization_prepare(SourceContext *ctx,uint32_t function);
static bool source_initialization_require(SourceContext *ctx,uint32_t function);
static XrXirType source_local_operand_type(SourceContext *ctx,uint32_t value) {
    const XrXirFunction *function=&ctx->functions[ctx->function];
    const SourceFunction *body=&ctx->bodies[ctx->function];
    if (value<function->parameter_count) return function->parameters ? function->parameters[value] : XR_XIR_UNIT;
    uint32_t ordinal=value-function->parameter_count;
    if (ordinal>=body->count || !body->recipes) return XR_XIR_UNIT;
    const SourceInstructionRecipe *recipe=&body->recipes[ordinal];
    return recipe->owner==ctx->function && recipe->value==value ? recipe->instruction.type : XR_XIR_UNIT;
}
static XrXirType source_local_thrown_type(SourceContext *ctx,SourceValue value) {
    if (value.type!=XR_XIR_ERROR) return value.type;
    const XrXirFunction *function=&ctx->functions[ctx->function];
    const SourceFunction *body=&ctx->bodies[ctx->function];
    for (uint32_t walked=0;walked<body->count;++walked) {
        if (value.id<function->parameter_count) break;
        uint32_t ordinal=value.id-function->parameter_count;
        if (ordinal>=body->count) break;
        const XrXirInstruction *op=&body->recipes[ordinal].instruction;
        if (op->op==XR_XIR_COPY || op->op==XR_XIR_OWNED_RETAIN) {
            if (op->args[0]>=value.id) break;
            value.id=op->args[0];continue;
        }
        if (op->op==XR_XIR_ENUM_GET && op->immediate==XR_XIR_TASK_RESULT_ERROR && !op->args[1]) {
            XrXirTaskResultType outcome={0};
            if (xr_xir_task_result_type_info(&ctx->types,source_local_operand_type(ctx,op->args[0]),&outcome))
                return outcome.error;
        }
        break;
    }
    return XR_XIR_ERROR;
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
            if (body->infer_errors && !body->infer_initializer) {
                if (!source_initialization_require(ctx,target) ||
                    !source_local_declared_error(ctx,target,op->type_arguments[0],op->type_arguments[1],&error)) return false;
                return source_local_error(ctx,node,error,true);
            }
            SourcePendingError *site=source_alloc(ctx,1,sizeof(*site));
            if (!site) return false;
            bool generated=(body->infer_initializer && body->node &&
                (body->node->type==AST_STRUCT_DECL || body->node->type==AST_CLASS_DECL)) ||
                (ctx->identities[ctx->function].method_kind==XR_XIR_CONSTRUCTOR &&
                 ctx->bodies[target].node && ctx->bodies[target].node->type==AST_FIELD_DECL);
            *site=(SourcePendingError){body->pending_errors,body->local_handler,node,target,
                op->type_arguments[0],op->type_arguments[1],generated || source_local_marked(ctx),
                body->error_context && body->error_context->asserted};
            body->pending_errors=site;return true;
        }
        if (!source_local_declared_error(ctx,target,op->type_arguments[0],op->type_arguments[1],&error)) return false;
    }
    return source_local_error(ctx,node,error,true);
}
typedef struct SourceInitializationFrame {
    struct SourceInitializationFrame *next;
    uint32_t function;
} SourceInitializationFrame;
/* Only generated initializer/default bodies contribute inferred edges. Every
 * ordinary, imported or explicit init declaration remains an authoritative leaf.
 * Working bounds are private until the complete reachable graph is stable. */
static bool source_initialization_require(SourceContext *ctx,uint32_t function) {
    if (function>=ctx->function_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"initialization declaration is missing");
    if (!ctx->bodies[function].infer_initializer) return true;
    if (ctx->bodies[function].initializer_state==SOURCE_INITIALIZER_READY) return true;
    uint32_t saved_function=ctx->function,saved_module=ctx->module;bool ok=false;
    SourceInitializationFrame *head=source_alloc(ctx,1,sizeof(*head));
    if (!head) return false;
    *head=(SourceInitializationFrame){NULL,function};SourceInitializationFrame *tail=head;
    for (SourceInitializationFrame *frame=head;frame;frame=frame->next) {
        uint32_t current=frame->function;
        if (ctx->bodies[current].initializer_state==SOURCE_INITIALIZER_READY) continue;
        ctx->function=current;ctx->module=ctx->bodies[current].module;
        if (!source_request_poll(ctx,ctx->bodies[current].node) ||
            !source_initialization_prepare(ctx,current)) goto done;
        for (SourcePendingError *site=ctx->bodies[current].pending_errors;site;site=site->next) {
            if (!source_request_poll(ctx,site->node)) goto done;
            uint32_t target=site->target;
            if (target>=ctx->function_count) {
                source_fail(ctx,site->node,XR_XIR_BAD_STRUCTURE,"initialization dependency is invalid");goto done;
            }
            if (!ctx->bodies[target].infer_initializer || ctx->bodies[target].initializer_state==SOURCE_INITIALIZER_READY) continue;
            bool present=false;
            for (SourceInitializationFrame *seen=head;seen;seen=seen->next) {
                if (!source_request_poll(ctx,site->node)) goto done;
                if (seen->function==target) {present=true;break;}
            }
            if (present) continue;
            SourceInitializationFrame *next=source_alloc(ctx,1,sizeof(*next));
            if (!next) goto done;
            *next=(SourceInitializationFrame){NULL,target};tail->next=next;tail=next;
        }
    }
    for (SourceInitializationFrame *frame=head;frame;frame=frame->next)
        if (ctx->bodies[frame->function].initializer_state!=SOURCE_INITIALIZER_READY)
            ctx->bodies[frame->function].initializer_bound=ctx->bodies[frame->function].initializer_seed;
    bool changed=true;
    while (changed) {
        changed=false;
        for (SourceInitializationFrame *frame=head;frame;frame=frame->next) {
            uint32_t current=frame->function;
            if (ctx->bodies[current].initializer_state==SOURCE_INITIALIZER_READY) continue;
            ctx->function=current;ctx->module=ctx->bodies[current].module;
            if (!source_request_poll(ctx,ctx->bodies[current].node)) goto done;
            XrXirType bound=ctx->bodies[current].initializer_seed;
            for (SourcePendingError *site=ctx->bodies[current].pending_errors;site;site=site->next) {
                if (!source_request_poll(ctx,site->node)) goto done;
                uint32_t target=site->target;
                XrXirType dependency=ctx->bodies[target].infer_initializer &&
                    ctx->bodies[target].initializer_state!=SOURCE_INITIALIZER_READY ? ctx->bodies[target].initializer_bound :
                    ctx->functions[target].error_type,error=XR_XIR_UNIT;
                if (!source_local_substitute_error(ctx,target,site->first_argument,site->argument_count,dependency,&error) ||
                    !source_local_escape(ctx,site->node,error,site->handler,site->asserted,&error)) goto done;
                XrXirCallableParameter members[2]={{bound,0},{error,0}};
                if (!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_UNION,
                    .parameters=members,.parameter_count=2},&bound)) goto done;
            }
            if (bound!=ctx->bodies[current].initializer_bound) {
                ctx->bodies[current].initializer_bound=bound;changed=true;
            }
        }
    }
    for (SourceInitializationFrame *frame=head;frame;frame=frame->next) {
        if (ctx->bodies[frame->function].initializer_state==SOURCE_INITIALIZER_READY) continue;
        ctx->function=frame->function;ctx->module=ctx->bodies[ctx->function].module;
        for (SourcePendingError *site=ctx->bodies[ctx->function].pending_errors;site;site=site->next) {
            uint32_t target=site->target;XrXirType error=XR_XIR_UNIT;
            XrXirType dependency=ctx->bodies[target].initializer_state==SOURCE_INITIALIZER_READY ? ctx->functions[target].error_type :
                ctx->bodies[target].initializer_bound;
            if (!source_request_poll(ctx,site->node) ||
                !source_local_substitute_error(ctx,target,site->first_argument,site->argument_count,dependency,&error)) goto done;
            if (error!=XR_XIR_NEVER && !site->marked) {
                source_fail(ctx,site->node,XR_XIR_BAD_TYPE,"E0330: throwing call requires try or a try body");goto done;
            }
        }
    }
    ctx->function=function;ctx->module=ctx->bodies[function].module;
    if (!source_request_poll(ctx,ctx->bodies[function].node)) goto done;
    for (SourceInitializationFrame *frame=head;frame;frame=frame->next) {
        uint32_t current=frame->function;
        if (ctx->bodies[current].initializer_state==SOURCE_INITIALIZER_READY) continue;
        ctx->functions[current].error_type=ctx->bodies[current].initializer_bound;
        ctx->bodies[current].initializer_state=SOURCE_INITIALIZER_READY;
    }
    ok=true;
done:
    ctx->function=saved_function;ctx->module=saved_module;return ok;
}
static bool source_local_errors_finish(SourceContext *ctx) {
    uint32_t saved_function=ctx->function,saved_module=ctx->module;bool ok=false;
    for (uint32_t f=0;f<ctx->function_count;++f) {
        ctx->function=f;ctx->module=ctx->bodies[f].module;
        if (!source_request_poll(ctx,ctx->bodies[f].node) || !source_initialization_require(ctx,f)) goto done;
    }
    for (uint32_t f=0;f<ctx->function_count;++f) {
        if (ctx->bodies[f].infer_initializer) continue;
        ctx->function=f;ctx->module=ctx->bodies[f].module;
        for (SourcePendingError *site=ctx->bodies[f].pending_errors;site;site=site->next) {
            XrXirType error;
            if (!source_request_poll(ctx,site->node) ||
                !source_local_declared_error(ctx,site->target,site->first_argument,site->argument_count,&error) ||
                !source_local_accept(ctx,site->node,error,site->marked,site->handler,site->asserted)) goto done;
        }
    }
    XrXirDeclarations declarations;XrXirModule module=source_module_view(ctx,&declarations);
    XrXirStatus status=xr_xir_compile_declaration_constraints_verify(&ctx->compile,&module);
    if (status!=XR_XIR_OK) {source_fail(ctx,NULL,status,"ready declaration type obligations failed");goto done;}
    ok=true;
done:
    ctx->function=saved_function;ctx->module=saved_module;return ok;
}
