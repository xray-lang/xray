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
 *   scalar or Unit result can replace it through the ordinary Source recipe writer.
 */
struct SourceComptimeSite {
    SourceComptimeSite *next;
    AstNode *syntax;
    uint32_t module,owner,declaration,helper;
    SourceExpectedType expected;
    XrXirValue value;
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
static bool source_comptime_result_type(XrXirType type) {
    return type==XR_XIR_UNIT || type==XR_XIR_BOOL || xr_xir_type_is_integer(type);
}
static bool source_comptime_result_value(const XrXirValue *value) {
    return value && (value->type==XR_XIR_UNIT ? (!value->reserved && !value->payload) :
        source_comptime_result_type((XrXirType)value->type) &&
        xr_xir_value_argument(value,NULL,(XrXirType)value->type));
}
static bool source_comptime_body(SourceContext *ctx,AstNode *child,SourceExpectedType expected) {
    SourceValue value={0};
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
    if (expected.present && !source_comptime_result_type(expected.type)) expected=(SourceExpectedType){0};
    if (!unit_block && !source_plan_expression(ctx,child,expected,&value)) return false;
    if (!source_comptime_result_type(value.type))
        return source_fail(ctx,child,XR_XIR_BAD_TYPE,"comptime result must be a bool, integer or Unit in this execution domain");
    if (!source_work_units(ctx,child,sizeof(value.type)+sizeof(ctx->returned))) return false;
    ctx->functions[ctx->function].result=value.type;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{value.id,0},{0},0,{0}},NULL)) return false;
    ctx->returned=true;return finish_body(ctx);
}
static bool source_comptime_thunk(SourceContext *ctx,AstNode *node,SourceExpectedType expected,uint32_t *output) {
    uint32_t outer=ctx->function;
    if (ctx->generics[outer].parameter_count || ctx->generics[outer].argument_count || ctx->bodies[outer].type_parameter_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"comptime in an open generic scope is not supported");
    if (ctx->function_count>=ctx->function_capacity || ctx->comptime_count>=ctx->comptime_capacity)
        return source_fail(ctx,node,XR_XIR_BUDGET,"comptime function inventory exhausted");
    if (!source_work_units(ctx,node,3*sizeof(SourceComptimeScope)+sizeof(XrXirFunction)+sizeof(XrXirFunctionIdentity)+sizeof(SourceName)+4*sizeof(uint32_t)+sizeof(AstNode *)+sizeof(XrXirSourceType))) return false;
    SourceComptimeScope saved=source_comptime_scope(ctx);
    uint32_t f=ctx->function_count++;
    char *name=source_alloc(ctx,32,1);
    if (!name || source_format(ctx,name,32,"$comptime%u",f)!=XR_DIAG_OK)
        return source_fail(ctx,node,ctx->diagnostic.status==XR_XIR_OK?XR_XIR_BUDGET:ctx->diagnostic.status,"comptime name allocation failed");
    ctx->functions[f]=(XrXirFunction){name,(uint32_t)source_text_size(ctx,name),NULL,0,XR_XIR_UNIT,NULL,0,NULL,0,NULL,0};
    ctx->identities[f]=(XrXirFunctionIdentity){.module=ctx->module};
    ctx->bodies[f].node=node;ctx->bodies[f].module=ctx->module;
    ctx->function=f;ctx->locals=ctx->scope=NULL;ctx->facts=NULL;ctx->epochs=NULL;
    ctx->loop=NULL;ctx->type_scope=(SourceTypeScope){0};ctx->active_expression=NULL;ctx->effect_argument=NULL;
    ctx->returned=false;ctx->flow_alternatives=0;
    SourceName declaration={.name=name,.node=node};
    bool ok=source_query_declare(ctx,&declaration,XR_XIR_SOURCE_FUNCTION,ctx->bodies[outer].declaration,source_query_range(ctx,node,NULL));
    ctx->bodies[f].declaration=declaration.declaration;
    if (ok) ok=source_query_syntax_role(ctx,declaration.declaration,XR_XIR_SOURCE_SYNTAX_CLOSURE,0);
    if (ok) ok=source_comptime_body(ctx,node->as.comptime_expr.expr,expected);
    if (ok) ((XrXirSourceDeclaration *)ctx->query.declarations)[declaration.declaration-1].type=source_query_type(ctx,ctx->functions[f].result);
    source_comptime_restore(ctx,&saved);
    if (ok) *output=f;
    return ok;
}
static bool source_comptime_expected_same(SourceExpectedType left,SourceExpectedType right) {
    return left.present==right.present && left.type==right.type && left.infer_result==right.infer_result && left.inferred==right.inferred;
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
    if (!site || !source_work_units(ctx,node,sizeof(*site)+sizeof(ctx->comptime_sites)+sizeof(ctx->comptime_count))) return false;
    *site=(SourceComptimeSite){ctx->comptime_sites,node,module,owner,declaration,helper,expected,{0}};
    ctx->comptime_sites=site;++ctx->comptime_count;
    if (!ctx->comptime_input)
        return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CALL,ctx->functions[helper].result,{0},{0},helper,{0}},value);
    const SourceComptimeSite *matched=NULL;
    for (const SourceComptimeSite *original=ctx->comptime_input;original;original=original->next) {
        if (!source_work_units(ctx,node,8)) return false;
        if (original->syntax==node && original->module==module && original->owner==owner && original->declaration==declaration && original->helper==helper &&
            source_comptime_expected_same(original->expected,expected)) {
            if (matched) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"comptime occurrence is ambiguous");
            matched=original;
        }
    }
    if (!matched || matched->value.type!=(uint32_t)ctx->functions[helper].result ||
        !source_comptime_result_type((XrXirType)matched->value.type) ||
        !source_comptime_result_value(&matched->value))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"comptime occurrence or canonical result changed");
    if (!source_work_units(ctx,node,sizeof(site->value)+sizeof(ctx->comptime_matched))) return false;
    site->value=matched->value;++ctx->comptime_matched;
    if (site->value.type==XR_XIR_UNIT) { *value=(SourceValue){0,XR_XIR_UNIT};return true; }
    XrXirOp op=site->value.type==XR_XIR_BOOL?XR_XIR_CONST_BOOL:XR_XIR_CONST_INT;
    return source_recipe_record(ctx,(XrXirInstruction){op,(XrXirType)site->value.type,{0},{0},site->value.payload,{0}},value);
}
