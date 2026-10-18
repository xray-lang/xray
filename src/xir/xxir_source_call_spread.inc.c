/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_call_spread.inc.c - Owned direct-call Tuple projections
 *
 * One authentic expression plan supplies every projection of a spread.
 * The private entry vector never replaces or mutates the borrowed call AST.
 */
typedef struct SourceCallSpread {
    SourceExpressionPlan *source;
    AstNode **fields;
    XrXirType *types;
    bool *known;
    uint32_t first,count;
} SourceCallSpread;
typedef struct SourceCallEntry {
    AstNode *syntax;
    XrCallArgAccess access;
    SourceCallArgumentPlan plan;
    SourceCallSpread *spread;
    uint32_t field;
    bool reference;
} SourceCallEntry;
typedef struct SourceCallExpansion {
    SourceCallEntry *entries;
    SourceCallSpread *spreads;
    uint32_t spread_count,next_spread;
} SourceCallExpansion;
static bool source_ground_type(SourceContext *ctx,AstNode *node,uint32_t depth,SourceExpectedType *output);
static AstNode *source_call_argument_syntax(const SourceCallSyntax *call,uint32_t index) {
    return call->expanded?call->expanded->entries[index].syntax:call->arguments[index];
}
static XrCallArgAccess source_call_argument_access(const SourceCallSyntax *call,uint32_t index) {
    return call->expanded?call->expanded->entries[index].access:
        call->arg_accesses?call->arg_accesses[index]:XR_CALL_ARG_PLAIN;
}
#include "xxir_source_call_spread_shape.inc.c"
static bool source_call_spread_fields(SourceContext *ctx,AstNode *node,SourceCallSpread *spread,
    uint32_t *at,uint32_t depth) {
    if(!node || !source_work(ctx,node)) return false;
    if(depth>=128) return source_fail(ctx,node,XR_XIR_BUDGET,"call spread field depth exhausted");
    if(node->type==AST_GROUPING) return source_call_spread_fields(ctx,node->as.grouping,spread,at,depth+1);
    if(node->type==AST_TUPLE_LITERAL) {
        const TupleLiteralNode *tuple=&node->as.tuple_literal;
        for(int f=0;f<tuple->count;++f) {
            AstNode *child=tuple->elements[f];
            if(!child || !source_work(ctx,child)) return false;
            if(child->type==AST_SPREAD_EXPR) {
                if(!source_call_spread_fields(ctx,child->as.spread_expr.expr,spread,at,depth+1)) return false;
            } else {
                if(*at>=spread->count) return source_fail(ctx,child,XR_XIR_BAD_STRUCTURE,"call spread shape changed");
                spread->fields[*at]=child;
                SourceExpectedType type={0};
                if(child->type==AST_LITERAL_STRING || child->type==AST_LITERAL_RUNE ||
                    child->type==AST_LITERAL_TRUE || child->type==AST_LITERAL_FALSE)
                    type=(SourceExpectedType){true,child->type==AST_LITERAL_STRING?XR_XIR_STRING:
                        child->type==AST_LITERAL_RUNE?XR_XIR_RUNE:XR_XIR_BOOL,false,false};
                else if(!source_ground_type(ctx,child,depth+1,&type)) return false;
                spread->types[*at]=type.type;spread->known[(*at)++]=type.present;
            }
        }
        return true;
    }
    uint32_t width=0;
    if(!source_call_spread_shape(ctx,node,depth+1,&width)) return false;
    if(*at>spread->count || width>spread->count-*at)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call spread shape changed");
    SourceExpectedType type={0};if(!source_ground_type(ctx,node,depth+1,&type)) return false;
    for(uint32_t f=0;f<width;++f) {
        if(!source_work(ctx,node)) return false;
        const XrXirTypeNode *tuple=type.present?xr_xir_tuple_signature(&ctx->types,type.type):NULL;
        if(tuple && tuple->parameter_count==width) {
            spread->known[*at+f]=true;spread->types[*at+f]=tuple->parameters[f].type;
        }
    }
    *at+=width;return true;
}
static bool source_call_expand(SourceContext *ctx,AstNode *node,SourceCallSyntax *call,
    const SourceCallPlan *formal,uint32_t parameters,bool enabled) {
    bool present=false;
    for(int a=0;a<call->arg_count;++a) {
        if(!source_work(ctx,node)) return false;
        if(call->arguments[a] && call->arguments[a]->type==AST_SPREAD_EXPR) present=true;
    }
    if(!present || !enabled) return true;
    SourceCallExpansion *expanded=source_recipe_storage(ctx,1,sizeof(*expanded));
    if(!expanded) return false;
    expanded->entries=parameters?source_recipe_storage(ctx,parameters,sizeof(*expanded->entries)):NULL;
    expanded->spreads=source_recipe_storage(ctx,(uint32_t)call->arg_count,sizeof(*expanded->spreads));
    if((parameters && !expanded->entries) || !expanded->spreads) return false;
    SourceFact *facts=ctx->facts;SourceEpoch *epochs=ctx->epochs;uint32_t count=0;bool ok=false;
    for(int a=0;a<call->arg_count;++a) {
        AstNode *input=call->arguments[a];
        if(!input || !source_work(ctx,input)) goto done;
        XrCallArgAccess access=call->arg_accesses?call->arg_accesses[a]:XR_CALL_ARG_PLAIN;
        SourceCallSpread *spread=NULL;uint32_t width=1;
        if(input->type==AST_SPREAD_EXPR) {
            if(access!=XR_CALL_ARG_PLAIN) {source_fail(ctx,input,XR_XIR_BAD_TYPE,"call spread supplies ordinary READ values");goto done;}
            spread=&expanded->spreads[expanded->spread_count++];spread->first=count;
            if(!source_call_spread_shape(ctx,input->as.spread_expr.expr,0,&width)) goto done;
            spread->count=width;
            spread->source=source_plan_collect(ctx,input->as.spread_expr.expr,(SourceExpectedType){0});
            spread->fields=width?source_recipe_storage(ctx,width,sizeof(*spread->fields)):NULL;
            spread->types=width?source_recipe_storage(ctx,width,sizeof(*spread->types)):NULL;
            spread->known=width?source_recipe_storage(ctx,width,sizeof(*spread->known)):NULL;
            if(!spread->source || (width && (!spread->fields || !spread->types || !spread->known))) goto done;
            uint32_t at=0;
            if(!source_call_spread_fields(ctx,input->as.spread_expr.expr,spread,&at,0) || at!=width ||
                !source_planned_effects(ctx,spread->source,0)) goto done;
        }
        if(formal->parameter_offset>parameters || count>parameters-formal->parameter_offset ||
            width>parameters-formal->parameter_offset-count) {source_fail(ctx,input,XR_XIR_BAD_TYPE,"too many call arguments");goto done;}
        for(uint32_t f=0;f<width;++f) {
            SourceCallEntry *entry=&expanded->entries[count];entry->syntax=input;entry->access=access;
            uint32_t parameter=count+formal->parameter_offset;
            entry->reference=xr_xir_type_is_cell(&ctx->types,formal->function_parameters[parameter]);
            entry->spread=spread;entry->field=f;
            if(spread) {
                if(entry->reference) {source_fail(ctx,input,XR_XIR_BAD_TYPE,"call spread cannot satisfy a ref parameter");goto done;}
                SourceExpressionPlan *value=source_plan_collect(ctx,input,(SourceExpectedType){0});
                if(!value) goto done;
                if(spread->known[f]) {value->type_ready=true;value->ground_type=spread->types[f];}
                if(spread->fields[f] && (!source_direct_integer(ctx,spread->fields[f],&value->integer) ||
                    !source_direct_decimal(ctx,spread->fields[f],&value->decimal))) goto done;
                entry->plan.value=value;
            } else if(entry->reference) {
                if(!source_reference_effects(ctx,input,&entry->plan.reference)) goto done;
            } else {
                entry->plan.value=source_plan_collect(ctx,input,(SourceExpectedType){0});
                if(!entry->plan.value || !source_planned_effects(ctx,entry->plan.value,0)) goto done;
            }
            ++count;
        }
    }
    call->arg_count=(int)count;call->expanded=expanded;ok=true;
done:
    ctx->facts=facts;ctx->epochs=epochs;return ok;
}
