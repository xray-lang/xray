/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_comptime_expression.inc.c - Real private Source evaluation thunks
 *
 * KEY CONCEPT:
 *   A provisional call is real Built code. Only a later independently checked
 *   typed literal result can replace it through the ordinary Source recipe writer.
 */
struct SourceComptimeSite {
    SourceComptimeSite *next;
    AstNode *syntax;
    uint32_t module,owner,declaration,helper;
    SourceExpectedType expected;
    XrXirValue value;
    uint32_t result_arity;
    uint64_t result_work;
};
typedef struct SourceComptimeScope {
    uint32_t function,module,depth,alternatives;
    SourceName *locals,*scope;
    SourceFact *facts;
    SourceEpoch *epochs;
    SourceExpressionPlan *active,*argument;
    SourceLoop *loop;
    SourceTypeScope types;
    bool returned;
} SourceComptimeScope;
static SourceComptimeScope source_comptime_scope(const SourceContext *ctx) {
    return (SourceComptimeScope){ctx->function,ctx->module,ctx->depth,ctx->flow_alternatives,
        ctx->locals,ctx->scope,ctx->facts,ctx->epochs,ctx->active_expression,ctx->effect_argument,
        ctx->loop,ctx->type_scope,ctx->returned};
}
static void source_comptime_restore(SourceContext *ctx,const SourceComptimeScope *scope) {
    ctx->function=scope->function;ctx->module=scope->module;ctx->depth=scope->depth;
    ctx->flow_alternatives=scope->alternatives;ctx->locals=scope->locals;ctx->scope=scope->scope;
    ctx->facts=scope->facts;ctx->epochs=scope->epochs;ctx->active_expression=scope->active;
    ctx->effect_argument=scope->argument;ctx->loop=scope->loop;ctx->type_scope=scope->types;
    ctx->returned=scope->returned;
}
#include "xxir_source_comptime_tuple.inc.c"

static bool source_comptime_body(SourceContext *ctx,AstNode *site,SourceExpectedType expected) {
    AstNode *child=site?site->as.comptime_expr.expr:NULL;SourceValue value={0};
    if (!child) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime has no expression");
    bool unit_block=false;
    if (child->type==AST_BLOCK) {
        if (!source_block_functions(ctx,child)) return false;
        ++ctx->bodies[ctx->function].lexical_depth;
        int count=child->as.block.count;
        bool tail=count && child->as.block.statements[count-1]->type==AST_EXPR_STMT;
        int statements=tail?count-1:count;
        for (int s=0;s<statements;++s) if (!statement(ctx,child->as.block.statements[s],false)) return false;
        if (ctx->returned)
            return source_fail(ctx,child,XR_XIR_BAD_TYPE,"comptime block cannot return from its enclosing function");
        if (tail) child=child->as.block.statements[count-1]->as.expr_stmt;
        else { value=(SourceValue){0,XR_XIR_UNIT};unit_block=true; }
    }
    if (expected.present && !source_comptime_result_type(ctx,child,expected.type)) expected=(SourceExpectedType){0};
    if (ctx->diagnostic.status!=XR_XIR_OK) return false;
    if (!unit_block && !source_plan_expression(ctx,child,expected,&value)) return false;
    if (!source_comptime_result_type(ctx,child,value.type))
        return source_fail(ctx,site,XR_XIR_BAD_TYPE,"comptime result must belong to an admitted closed value family");
    if (!source_work_units(ctx,child,sizeof(value.type)+sizeof(ctx->returned))) return false;
    ctx->functions[ctx->function].result=value.type;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{value.type==XR_XIR_UNIT?0:value.id,0},{0},0,{0}},NULL)) return false;
    ctx->returned=true;return finish_body(ctx);
}
static bool source_comptime_open_body(SourceContext *ctx,AstNode *node,uint32_t function,bool *open) {
    const XrXirFunction *body=&ctx->functions[function];
    const XrXirGeneric *generic=&ctx->generics[function];
    if (!source_work_units(ctx,node,2)) return false;
    *open=xr_xir_type_span(&ctx->types,body->result)!=0;
    for (uint32_t i=0;i<body->instruction_count;++i) {
        if (!source_work_units(ctx,node,2)) return false;
        if (xr_xir_type_span(&ctx->types,body->instructions[i].type)) *open=true;
    }
    for (uint32_t t=0;t<generic->argument_count;++t) {
        if (!source_work_units(ctx,node,2)) return false;
        if (xr_xir_type_span(&ctx->types,generic->arguments[t])) *open=true;
    }
    return true;
}
static bool source_comptime_thunk(SourceContext *ctx,AstNode *node,SourceExpectedType expected,uint32_t *output) {
    uint32_t outer=ctx->function;
    if (ctx->function_count>=ctx->function_capacity || ctx->comptime_count>=ctx->comptime_capacity)
        return source_fail(ctx,node,XR_XIR_BUDGET,"comptime function inventory exhausted");
    uint64_t lexical_bytes=sizeof(ctx->bodies[outer].type_owner)+sizeof(ctx->bodies[outer].generic_owner)+
        sizeof(ctx->bodies[outer].type_parameters)+sizeof(ctx->bodies[outer].type_parameter_count);
    if (!source_work_units(ctx,node,3*sizeof(SourceComptimeScope)+sizeof(XrXirFunction)+sizeof(XrXirFunctionIdentity)+sizeof(SourceName)+4*sizeof(uint32_t)+sizeof(AstNode *)+sizeof(XrXirSourceType)+lexical_bytes)) return false;
    SourceComptimeScope saved=source_comptime_scope(ctx);
    uint32_t f=ctx->function_count++;
    char *name=source_alloc(ctx,32,1);
    if (!name || source_format(ctx,name,32,"$comptime%u",f)!=XR_DIAG_OK)
        return source_fail(ctx,node,ctx->diagnostic.status==XR_XIR_OK?XR_XIR_BUDGET:ctx->diagnostic.status,"comptime name allocation failed");
    ctx->functions[f]=(XrXirFunction){name,(uint32_t)source_text_size(ctx,name),NULL,0,XR_XIR_UNIT,NULL,0,NULL,0,NULL,0, XR_XIR_ERROR};
    /* A synthetic lexical closure retains its declaration owner's access,
     * exactly as an ordinary nested function does. No runtime capture enters. */
    uint32_t nominal=ctx->identities[outer].nominal_owner;
    ctx->identities[f]=(XrXirFunctionIdentity){.module=ctx->module,.nominal_owner=nominal,
        .method_kind=nominal?XR_XIR_MEMBER_HELPER:XR_XIR_NON_MEMBER};
    ctx->bodies[f].node=node;ctx->bodies[f].module=ctx->module;
    /* A genuine lexical helper inherits the declaration's symbolic binders.
     * Ordinary specialization closes them before any evaluator admission. */
    ctx->bodies[f].type_owner=ctx->bodies[outer].type_owner;
    ctx->bodies[f].generic_owner=ctx->bodies[outer].generic_owner;
    ctx->bodies[f].type_parameters=ctx->bodies[outer].type_parameters;
    ctx->bodies[f].type_parameter_count=ctx->bodies[outer].type_parameter_count;
    if (!source_work_units(ctx,node,2*sizeof(XrXirGeneric))) return false;
    ctx->generics[f]=(XrXirGeneric){ctx->generics[outer].constraints,ctx->generics[outer].parameter_count,
        NULL,0,ctx->generics[outer].parameter_kinds};
    ctx->function=f;ctx->locals=ctx->scope=NULL;ctx->facts=NULL;ctx->epochs=NULL;
    ctx->loop=NULL;ctx->type_scope=saved.types;ctx->active_expression=NULL;ctx->effect_argument=NULL;
    ctx->returned=false;ctx->flow_alternatives=0;
    SourceName declaration={.name=name,.node=node};
    bool ok=source_query_declare(ctx,&declaration,XR_XIR_SOURCE_FUNCTION,ctx->bodies[outer].declaration,source_query_range(ctx,node,NULL));
    ctx->bodies[f].declaration=declaration.declaration;
    if (ok) ok=source_query_syntax_role(ctx,declaration.declaration,XR_XIR_SOURCE_SYNTAX_CLOSURE,0);
    if (ok) ok=source_comptime_body(ctx,node,expected);
    bool open=false;
    if (ok) ok=source_comptime_open_body(ctx,node,f,&open);
    if (ok && !open) {
        ctx->generics[f].constraints=NULL;ctx->generics[f].parameter_count=0;ctx->generics[f].parameter_kinds=NULL;
    }
    if (ok) ok=source_query_parameters(ctx,declaration.declaration);
    source_comptime_restore(ctx,&saved);
    if (ok) *output=f;
    return ok;
}
static bool source_comptime_expected_same(SourceContext *ctx,AstNode *node,SourceExpectedType left,SourceExpectedType right) {
    if (left.present!=right.present || left.infer_result!=right.infer_result || left.inferred!=right.inferred) return false;
    if (!ctx->comptime_input_types)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"comptime expected context has no live Source type owner");
    if (xr_xir_type_span(ctx->comptime_input_types,left.type) || xr_xir_type_span(&ctx->types,right.type)) {
        uint32_t count=ctx->generics[ctx->function].parameter_count;
        XrXirType *arguments=count?source_alloc(ctx,count,sizeof(*arguments)):NULL;
        if (count && !arguments) return false;
        for (uint32_t i=0;i<count;++i) {
            if (!source_work_units(ctx,node,sizeof(*arguments))) return false;
            arguments[i]=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+i);
        }
        XrXirStatus status=xr_xir_compile_type_substitution_matches_between(&ctx->compile,
            ctx->comptime_input_types,&ctx->types,arguments,count,left.type,right.type);
        return status==XR_XIR_OK || source_fail(ctx,node,status,"comptime symbolic expected context changed");
    }
    return source_comptime_type_matches(ctx,node,ctx->comptime_input_types,left.type,right.type);
}
static bool source_comptime_call(SourceContext *ctx,AstNode *node,uint32_t helper,SourceValue *value) {
    XrXirInstruction call={XR_XIR_CALL,ctx->functions[helper].result,{0},{0},helper,{0}};
    uint32_t count=ctx->generics[helper].parameter_count;
    XrXirType *arguments=count?source_alloc(ctx,count,sizeof(*arguments)):NULL;
    if (count && !arguments) return false;
    for (uint32_t a=0;a<count;++a) {
        if (!source_work_units(ctx,node,sizeof(*arguments))) return false;
        arguments[a]=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+a);
    }
    if (count && !source_type_arguments(ctx,node,arguments,count,&call)) return false;
    return source_recipe_record(ctx,call,value);
}
static bool source_comptime_expression(SourceContext *ctx,AstNode *node,SourceExpectedType expected,SourceValue *value) {
    uint32_t owner=ctx->function,module=ctx->module,helper=UINT32_MAX;
    if (!source_comptime_thunk(ctx,node,expected,&helper)) return false;
    uint32_t declaration=ctx->bodies[owner].declaration;
    for (const SourceComptimeSite *previous=ctx->comptime_sites;previous;previous=previous->next) {
        if (!source_work_units(ctx,node,4)) return false;
        if (previous->syntax==node && previous->module==module && previous->owner==owner)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"comptime occurrence was lowered twice");
    }
    SourceComptimeSite *site=source_alloc(ctx,1,sizeof(*site));
    if (!site || !source_work_units(ctx,node,sizeof(*site)+sizeof(ctx->comptime_sites)+sizeof(ctx->comptime_count)+2)) return false;
    *site=(SourceComptimeSite){ctx->comptime_sites,node,module,owner,declaration,helper,expected,{0},0,0};
    ctx->comptime_sites=site;++ctx->comptime_count;
    if (!ctx->comptime_input) return source_comptime_call(ctx,node,helper,value);
    const SourceComptimeSite *matched=NULL;
    for (const SourceComptimeSite *original=ctx->comptime_input;original;original=original->next) {
        if (!source_work_units(ctx,node,8)) return false;
        if (original->syntax==node && original->module==module && original->owner==owner && original->declaration==declaration && original->helper==helper &&
            source_comptime_expected_same(ctx,node,original->expected,expected)) {
            if (matched) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"comptime occurrence is ambiguous");
            matched=original;
        }
    }
    if (matched && ctx->generics[helper].parameter_count) {
        if (!source_work_units(ctx,node,sizeof(ctx->comptime_matched))) return false;
        ++ctx->comptime_matched;return source_comptime_call(ctx,node,helper,value);
    }
    if (!matched || !source_comptime_result_value(ctx,node,&matched->value,matched->result_arity,matched->result_work))
        return source_fail(ctx,node,ctx->diagnostic.status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:ctx->diagnostic.status,"comptime occurrence or canonical result changed");
    const XrXirTypeArena *arena=NULL;
    if (matched->result_arity) {
        if (!source_work_units(ctx,node,(uint64_t)matched->result_arity*16)) return false;
        arena=xr_xir_value_arena(&matched->value);
    }
    if (!source_comptime_type_matches(ctx,node,xr_xir_compile_type_arena_types(arena),
        (XrXirType)matched->value.type,ctx->functions[helper].result))
        return source_fail(ctx,node,ctx->diagnostic.status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:ctx->diagnostic.status,"comptime occurrence changed its result type");
    if (!source_work_units(ctx,node,sizeof(site->value)+sizeof(site->result_arity)+sizeof(site->result_work)+sizeof(ctx->comptime_matched))) return false;
    site->result_arity=matched->result_arity;
    site->result_work=matched->result_work;
    if (matched->value.type==XR_XIR_STRING || matched->result_arity) {
        if (!source_work_units(ctx,node,(uint64_t)matched->result_arity*16)) return false;
        XrXirValueStatus copied=xr_xir_value_copy(&matched->value,&site->value);
        if (copied!=XR_XIR_VALUE_OK) return source_fail(ctx,node,
            copied==XR_XIR_VALUE_LIMIT || copied==XR_XIR_VALUE_REFCOUNT_LIMIT?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE,
            "comptime result receiving failed");
    } else site->value=matched->value;
    ++ctx->comptime_matched;
    return source_comptime_constant(ctx,node,&site->value,ctx->functions[helper].result,value);
}
