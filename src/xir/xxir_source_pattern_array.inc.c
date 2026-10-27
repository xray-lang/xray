/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_pattern_array.inc.c - Length-tested Array bindings and owned tails
 */
static bool source_pattern_array_tail(SourceContext *,SourceValue,SourceValue,SourceValue *);
static bool source_pattern_array(SourceContext *ctx,AstNode *node,XrXirType type,
    SourceMatchPattern *out,uint32_t depth) {
    PatternArrayNode *array=&node->as.pattern_array;
    if (!xr_xir_type_is_array(&ctx->types,type))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Array pattern requires an exact Array type");
    if (array->count<0 || (array->count && !array->patterns) ||
        (!array->has_rest && (array->rest_name || array->rest_binding)) ||
        (array->rest_name && (!array->rest_binding || array->rest_binding->type!=AST_VARIABLE)))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Array pattern children or tail binding are missing");
    bool bound_tail=array->rest_name && !source_text_same(ctx,node,array->rest_name,"_");
    out->array=true;out->array_prefix=(uint32_t)array->count;
    out->interval=true;out->low=out->array_prefix;
    out->high=array->has_rest ? INT64_MAX : out->low;
    out->count=out->array_prefix+(bound_tail?1u:0u);
    out->fields=out->count ? source_alloc(ctx,out->count,sizeof(*out->fields)) : NULL;
    if (out->count && !out->fields) return false;
    XrXirType element=xr_xir_array_element(&ctx->types,type);
    for (uint32_t i=0;i<out->array_prefix;++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        AstNode *syntax=array->patterns[i];
        if (!syntax) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Array pattern element is missing");
        if (syntax->type!=AST_PATTERN_WILDCARD && !source_pattern_binding(syntax))
            return source_fail(ctx,syntax,XR_XIR_BAD_TYPE,"Array element patterns require bindings or wildcards");
        SourceMatchPattern *child=source_alloc(ctx,1,sizeof(*child));
        if (!child || !source_match_pattern(ctx,syntax,element,child,depth+1)) return false;
        out->fields[i]=child;
    }
    if (bound_tail) {
        SourceMatchPattern *tail=source_alloc(ctx,1,sizeof(*tail));
        if (!tail) return false;
        *tail=(SourceMatchPattern){.node=array->rest_binding,.binding=array->rest_binding,.type=type,.any=true};
        out->fields[out->array_prefix]=tail;
    }
    return source_native_array_declaration(ctx);
}
static bool source_match_array_payload(SourceContext *ctx,SourceMatchPattern *pattern,
    SourceValue receiver,bool test,SourceMatchFailure **failures) {
    SourceValue prefix;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,
        {0},{0},pattern->array_prefix,{0}},&prefix)) return false;
    if (test) {
        SourceValue length,condition;
        if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_LEN,XR_XIR_I64,
                {receiver.id},{0},0,{0}},&length) ||
            !source_binary_apply(ctx,pattern->node,pattern->low==pattern->high?AST_BINARY_EQ:AST_BINARY_GE,
                length,prefix,&condition) || !source_match_test(ctx,condition,true,failures)) return false;
    }
    /* The length branch dominates every element read and tail construction. */
    for (uint32_t i=0;i<pattern->array_prefix;++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        SourceMatchPattern *child=pattern->fields[i];
        if (!child->binding) continue;
        SourceValue index,value;
        if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},i,{0}},&index) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_GET,child->type,
                {receiver.id,index.id},{0},0,{0}},&value) || !source_match_bind(ctx,child->symbol,value)) return false;
    }
    if (pattern->count>pattern->array_prefix) {
        SourceMatchPattern *tail=pattern->fields[pattern->array_prefix];SourceValue value;
        if (!source_pattern_array_tail(ctx,receiver,prefix,&value) ||
            !source_match_bind(ctx,tail->symbol,value)) return false;
    }
    return true;
}
