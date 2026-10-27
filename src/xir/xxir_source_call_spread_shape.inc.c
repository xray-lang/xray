/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_call_spread_shape.inc.c - Static shape hints from authentic expressions
 */
static SourceName *source_call_spread_function(SourceContext *ctx,AstNode *callee) {
    SourceName *binding=NULL;
    if (callee && callee->type==AST_VARIABLE) binding=visible_name(ctx,callee->as.variable.name);
    else if (callee && callee->type==AST_MEMBER_ACCESS && callee->as.member_access.object &&
        callee->as.member_access.object->type==AST_VARIABLE) {
        SourceName *space=visible_name(ctx,callee->as.member_access.object->as.variable.name);
        if (space && space->kind==SOURCE_MODULE) binding=imported_declaration(ctx,space,callee->as.member_access.name);
    }
    if (binding && binding->kind==SOURCE_IMPORT) binding=imported_declaration(ctx,binding,binding->imported);
    return binding && binding->kind==SOURCE_FUNCTION ? binding : NULL;
}
/* Shape facts reserve operands only. Completion checks the real tuple type
 * and every declaration, conversion and access through the ordinary plan. */
static bool source_call_spread_shape(SourceContext *ctx,AstNode *node,uint32_t depth,uint32_t *arity) {
    if (!node || !((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    if (depth>=128) return source_fail(ctx,node,XR_XIR_BUDGET,"call spread shape depth exhausted");
    if (node->type==AST_GROUPING) return source_call_spread_shape(ctx,node->as.grouping,depth+1,arity);
    if (node->type==AST_TUPLE_LITERAL) {
        const TupleLiteralNode *tuple=&node->as.tuple_literal;
        if (tuple->count<0 || (tuple->count && !tuple->elements))
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call spread Tuple entries are missing");
        *arity=0;
        for (int f=0;f<tuple->count;++f) {
            AstNode *field=tuple->elements[f];uint32_t count=1;
            if (!field) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call spread Tuple entry is missing");
            if (field->type==AST_SPREAD_EXPR && !source_call_spread_shape(ctx,field->as.spread_expr.expr,depth+1,&count)) return false;
            if (!((ctx)->diagnostic.status == XR_XIR_OK) || count>65536-*arity)
                return source_fail(ctx,node,XR_XIR_BUDGET,"call expanded arity exhausted");
            *arity+=count;
        }
        return true;
    }
    SourceExpectedType ground;
    if (!source_ground_type(ctx,node,depth,&ground)) return false;
    if (ground.present && node->type==AST_VARIABLE) {
        SourceName *binding=visible_name(ctx,node->as.variable.name);
        uint32_t claims=source_fact_depth(ctx,binding);
        if (ctx->diagnostic.status!=XR_XIR_OK) return false;
        for (uint32_t c=0;c<claims && xr_xir_type_is_nullable(&ctx->types,ground.type);++c) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            ground.type=xr_xir_nullable_element(&ctx->types,ground.type);
        }
    }
    if (!ground.present && node->type==AST_CALL_EXPR) {
        const CallExprNode *call=&node->as.call_expr;
        SourceName *binding=source_call_spread_function(ctx,call->callee);
        if (ctx->diagnostic.status!=XR_XIR_OK) return false;
        if (binding) {
            XrXirType result=ctx->functions[binding->index].result;
            uint32_t parameters=ctx->generics[binding->index].parameter_count;
            if (call->type_arg_count && (uint32_t)call->type_arg_count==parameters && call->type_args) {
                XrXirType *types=source_alloc(ctx,parameters,sizeof(*types));
                if (!types) return false;
                for (uint32_t p=0;p<parameters;++p)
                    if (!((ctx)->diagnostic.status == XR_XIR_OK) || !source_type(ctx,call->type_args[p],&types[p])) return false;
                if (!source_substitute(ctx,&(SourceSubstitution){types,parameters},result,0,&result)) return false;
            } else if ((uint32_t)result>=XR_XIR_TYPE_PARAMETER_BASE &&
                (uint32_t)result-XR_XIR_TYPE_PARAMETER_BASE<parameters) {
                const XrXirFunction *function=&ctx->functions[binding->index];
                uint32_t count=call->arg_count>0 ? (uint32_t)call->arg_count : 0;
                for (uint32_t a=0;a<count && a<function->parameter_count;++a) {
                    if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
                    if (function->parameters[a]==result && call->arguments[a]->type!=AST_SPREAD_EXPR)
                        return source_call_spread_shape(ctx,call->arguments[a],depth+1,arity);
                }
            }
            ground=(SourceExpectedType){true,result,false,false};
        }
    }
    if (ground.present && ground.type==XR_XIR_UNIT) { *arity=0;return true; }
    const XrXirTypeNode *tuple=ground.present ? xr_xir_tuple_signature(&ctx->types,ground.type) : NULL;
    if (!tuple) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"call spread requires a statically known Tuple");
    *arity=tuple->parameter_count;return true;
}
