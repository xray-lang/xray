/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_native_arguments.inc.c - Typed native operands from authentic calls
 *
 * KEY CONCEPT:
 *   Native recipes retain their receiver and operation authority. Their READ
 *   operands use the same ordered projections and conversions as ordinary calls.
 */
typedef struct SourceNativeArity { uint32_t fewest,most; } SourceNativeArity;
static bool source_native_read_arguments(SourceContext *ctx,AstNode *node,
    SourceNativeArity arity,const XrXirType *types,SourceValue *values,uint32_t *count) {
    SourceCallSyntax syntax;
    if (!count || arity.fewest>arity.most || arity.most>65536 || (arity.most && (!types || !values)))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"native operand contract is malformed");
    if (!source_call_syntax(ctx,node,&syntax)) return false;
    if (syntax.type_arg_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Array method requires its declared arguments");
    SourceCallPlan plan={SOURCE_CALL_NATIVE,{0},{0},types,NULL,XR_XIR_UNIT,{0},values,0,0,NULL};
    if (!source_call_expand(ctx,node,&syntax,&plan,arity.most)) return false;
    if ((uint32_t)syntax.arg_count<arity.fewest || (uint32_t)syntax.arg_count>arity.most)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Array method requires its declared arguments");
    for (uint32_t a=0;a<(uint32_t)syntax.arg_count;++a) {
        if (!source_work(ctx,node)) return false;
        if (source_call_argument_access(&syntax,a)!=XR_CALL_ARG_PLAIN)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Array method arguments use ordinary READ values");
    }
    if (!source_call_plan_arguments(ctx,node,&plan,&syntax)) return false;
    *count=(uint32_t)syntax.arg_count;return true;
}
static bool source_native_uniform_arguments(SourceContext *ctx,AstNode *node,XrXirType type,
    SourceValue **values,uint32_t *count) {
    SourceCallSyntax syntax;
    if (!source_call_syntax(ctx,node,&syntax)) return false;
    uint32_t arity=0;
    for (int a=0;a<syntax.arg_count;++a) {
        AstNode *argument=syntax.arguments[a];uint32_t width=1;
        if (!argument || !source_work(ctx,argument)) return false;
        if (argument->type==AST_SPREAD_EXPR &&
            !source_call_spread_shape(ctx,argument->as.spread_expr.expr,0,&width)) return false;
        if (width>65536-arity) return source_fail(ctx,node,XR_XIR_BUDGET,"native expanded arity exhausted");
        arity+=width;
    }
    XrXirType *types=arity ? source_alloc(ctx,arity,sizeof(*types)) : NULL;
    SourceValue *arguments=arity ? source_recipe_storage(ctx,arity,sizeof(*arguments)) : NULL;
    if (arity && (!types || !arguments)) return false;
    for (uint32_t a=0;a<arity;++a) {
        if (!source_work(ctx,node)) return false;
        types[a]=type;
    }
    if (!source_native_read_arguments(ctx,node,(SourceNativeArity){arity,arity},types,arguments,count)) return false;
    *values=arguments;return true;
}
