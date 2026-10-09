/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_pattern_bindings.inc.c - Common arm binding identities
 *
 * KEY CONCEPT:
 *   Every alternative checks the same immutable binding set before generation.
 */
typedef struct SourceMatchArm {
    SourceMatchPattern *patterns;
    uint32_t count;
    SourceName *bindings;
} SourceMatchArm;
static bool source_pattern_bindings(SourceContext *ctx, SourceMatchPattern *pattern,
    SourceMatchArm *arm, uint32_t alternative, uint32_t depth) {
    if (!pattern) return true;
    if (!source_work(ctx,pattern->node)) return false;
    if (depth>=128) return source_fail(ctx,pattern->node,XR_XIR_BUDGET,"pattern binding depth exhausted");
    if (pattern->binding) {
        AstNode *node=pattern->binding; const char *name=node->as.variable.name;
        SourceName *symbol=arm->bindings;
        for (;symbol;symbol=symbol->next) {
            if (!source_work(ctx,node)) return false;
            if (source_text_same(ctx, NULL, symbol->name, name)) break;
        }
        if (!alternative) {
            if (symbol) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"duplicate pattern binding");
            symbol=source_alloc(ctx,1,sizeof(*symbol));
            if (!symbol) return false;
            *symbol=(SourceName){arm->bindings,name,NULL,node,SOURCE_LOCAL,1,ctx->module,pattern->type,false,false,0, false, false};
            arm->bindings=symbol;
            if (!source_query_declare(ctx,symbol,XR_XIR_SOURCE_BINDING,ctx->bodies[ctx->function].declaration,
                source_query_range(ctx,node,name))) return false;
            source_query_binding_type(ctx,symbol);
        } else {
            if (!symbol || symbol->type!=pattern->type)
                return source_fail(ctx,node,XR_XIR_BAD_TYPE,"alternatives require the same binding names and exact types");
            if (symbol->index==alternative+1) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"duplicate pattern binding");
            symbol->index=alternative+1;
            if (!source_query_target_reference(ctx,source_query_range(ctx,node,name),symbol->declaration,XR_XIR_SOURCE_WRITE)) return false;
        }
        pattern->symbol=symbol;
    }
    for (uint32_t i=0;i<pattern->count;++i)
        if (!source_pattern_bindings(ctx,pattern->fields[i],arm,alternative,depth+1)) return false;
    return true;
}
static bool source_match_normalize_arms(SourceContext *ctx, AstNode *node, XrXirType type,
    SourceMatchArm **output, bool *exhaustive) {
    MatchExprNode *match=&node->as.match_expr; uint32_t count=(uint32_t)match->arm_count,total=0;
    SourceMatchArm *arms=source_alloc(ctx,count,sizeof(*arms));
    if (!arms) return false;
    for (uint32_t i=0;i<count;++i) {
        AstNode *arm=match->arms[i];
        if (!source_work(ctx,arm)) return false;
        if (!arm || arm->type!=AST_MATCH_ARM || !arm->as.match_arm.pattern)
            return source_fail(ctx,arm,XR_XIR_BAD_STRUCTURE,"invalid match arm");
        AstNode *pattern=arm->as.match_arm.pattern;
        int alternatives=pattern->type==AST_PATTERN_MULTI?pattern->as.pattern_multi.count:1;
        if (alternatives<=0 || (uint32_t)alternatives>UINT32_MAX/2-total)
            return source_fail(ctx,pattern,XR_XIR_BUDGET,"pattern alternative capacity exhausted");
        arms[i].count=(uint32_t)alternatives; total+=arms[i].count;
        arms[i].patterns=source_alloc(ctx,arms[i].count,sizeof(*arms[i].patterns));
        if (!arms[i].patterns) return false;
    }
    SourceMatchPattern **rows=source_alloc(ctx,total,sizeof(*rows));
    if (!rows) return false;
    uint32_t row_count=0; bool complete=false;
    for (uint32_t i=0;i<count;++i) {
        MatchArmNode *arm=&match->arms[i]->as.match_arm; AstNode *root=arm->pattern;
        if (complete) return source_fail(ctx,root,XR_XIR_BAD_STRUCTURE,"unreachable match arm is not admitted");
        for (uint32_t j=0;j<arms[i].count;++j) {
            AstNode *pattern=root->type==AST_PATTERN_MULTI?root->as.pattern_multi.patterns[j]:root;
            if (!source_match_pattern(ctx,pattern,type,&arms[i].patterns[j],0) ||
                !source_pattern_bindings(ctx,&arms[i].patterns[j],&arms[i],j,0)) return false;
            for (SourceName *p=arms[i].bindings;p;p=p->next) {
                if (!source_work(ctx,pattern)) return false;
                if (p->index!=j+1) return source_fail(ctx,pattern,XR_XIR_BAD_TYPE,"alternative is missing a common binding");
            }
            if (!arm->guard) rows[row_count++]=&arms[i].patterns[j];
        }
        if (!arm->guard && !source_pattern_coverage(ctx,(SourcePatternMatrix){&type,rows,1,row_count},0,&complete)) return false;
    }
    if (!complete && xr_xir_type_is_enum(&ctx->types,type))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"enum match is not exhaustive");
    *output=arms; *exhaustive=complete; return true;
}
