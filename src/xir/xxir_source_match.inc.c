/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_match.inc.c - Ordered enum tests and owned arm bindings
 */
#include "xxir_source_pattern.inc.c"
#include "xxir_source_pattern_coverage.inc.c"
static bool source_match_bind(SourceContext *ctx, AstNode *node, SourceValue value) {
    for (SourceName *p=ctx->locals;p!=ctx->scope;p=p->next) {
        if (!source_work(ctx,node)) return false;
        if (!strcmp(p->name,node->as.variable.name)) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"duplicate pattern binding");
    }
    SourceName *binding=source_alloc(ctx,1,sizeof(*binding));
    if (!binding) return false;
    *binding=(SourceName){ctx->locals,node->as.variable.name,NULL,node,SOURCE_LOCAL,value.id,ctx->module,value.type,false,false,0};
    ctx->locals=binding;
    if (!source_query_declare(ctx,binding,XR_XIR_SOURCE_BINDING,ctx->bodies[ctx->function].declaration,
        source_query_range(ctx,node,binding->name))) return false;
    source_query_binding_type(ctx,binding); return true;
}
typedef struct SourceMatchFailure {
    struct SourceMatchFailure *next;
    uint32_t instruction, target;
} SourceMatchFailure;
static bool source_match_test(SourceContext *ctx, SourceValue condition, bool truth, SourceMatchFailure **failures) {
    SourceMatchFailure *failure=source_alloc(ctx,1,sizeof(*failure));
    if (!failure) return false;
    SourceFunction *body=&ctx->bodies[ctx->function];
    *failure=(SourceMatchFailure){*failures,body->count,truth?1u:0u}; *failures=failure;
    XrXirInstruction branch={XR_XIR_BRANCH,XR_XIR_UNIT,{condition.id,0},{0},0};
    branch.targets[truth?0:1]=body->block_count;
    return emit(ctx,branch,NULL) && begin_block(ctx);
}
static bool source_match_payload(SourceContext *ctx, SourceMatchPattern *pattern,
    SourceValue receiver, bool test, SourceMatchFailure **failures, uint32_t depth) {
    if (!pattern) return true;
    if (!source_work(ctx,pattern->node)) return false;
    if (depth>=128) return source_fail(ctx,pattern->node,XR_XIR_BUDGET,"pattern generation depth exhausted");
    if (pattern->binding) return source_match_bind(ctx,pattern->binding,receiver);
    if (pattern->any) return true;
    if (pattern->boolean) return !test || source_match_test(ctx,receiver,pattern->truth,failures);
    SourceEnumSelection *selected=&pattern->selection;
    if (test) {
        SourceValue tag,ordinal,condition;
        if (!emit(ctx,(XrXirInstruction){XR_XIR_ENUM_TAG,XR_XIR_I64,{receiver.id,0},{0},0},&tag) ||
            !emit(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},selected->variant},&ordinal) ||
            !emit(ctx,(XrXirInstruction){XR_XIR_EQ_INT,XR_XIR_BOOL,{tag.id,ordinal.id},{0},0},&condition) ||
            !source_match_test(ctx,condition,true,failures)) return false;
    }
    for (uint32_t i=0;i<pattern->count;++i) {
        if (!source_work(ctx,pattern->node)) return false;
        SourceMatchPattern *child=pattern->fields[i];
        if (!child || (child->any && !child->binding)) continue;
        SourceValue value;
        if (!emit(ctx,(XrXirInstruction){XR_XIR_ENUM_GET,child->type,{receiver.id,i},{0},selected->variant},&value) ||
            !source_match_payload(ctx,child,value,test,failures,depth+1)) return false;
    }
    return true;
}
static bool source_match_body(SourceContext *ctx, AstNode *node, XrXirType expected, SourceValue *value) {
    if (node->type != AST_BLOCK) return expression_in(ctx,node,expected,value);
    *value=(SourceValue){0,XR_XIR_UNIT};
    for (int i=0;i<node->as.block.count;++i) {
        AstNode *part=node->as.block.statements[i];
        if (i+1==node->as.block.count && part->type==AST_EXPR_STMT)
            return expression_in(ctx,part->as.expr_stmt,expected,value);
        if (!statement(ctx,part,false)) return false;
        if (ctx->returned) {
            if (i+1!=node->as.block.count) return source_fail(ctx,part,XR_XIR_BAD_STRUCTURE,"unreachable match statements are not admitted");
            return true;
        }
    }
    return true;
}
static bool source_match(SourceContext *ctx, AstNode *node, XrXirType expected, bool result_required, SourceValue *value) {
    MatchExprNode *match=&node->as.match_expr; SourceValue receiver;
    if (match->arm_count<=0 || (uint32_t)match->arm_count>UINT32_MAX/2 || !expression(ctx,match->expr,&receiver))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"match requires arms and a scrutinee");
    if (!xr_xir_type_is_enum(&ctx->types,receiver.type)) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"match scrutinee family is not admitted");
    uint32_t count=(uint32_t)match->arm_count;
    SourceMatchPattern *patterns=source_alloc(ctx,count,sizeof(*patterns));
    SourceMatchPattern **rows=source_alloc(ctx,count,sizeof(*rows));
    SourceValue *inputs=source_alloc(ctx,count*2,sizeof(*inputs)); uint32_t *jumps=source_alloc(ctx,count,sizeof(*jumps));
    if (!patterns || !rows || !inputs || !jumps) return false;
    uint32_t row_count=0; bool complete=false;
    for (uint32_t i=0;i<count;++i) {
        AstNode *arm=match->arms[i];
        if (!source_work(ctx,arm) || arm->type!=AST_MATCH_ARM ||
            !source_match_pattern(ctx,arm->as.match_arm.pattern,receiver.type,&patterns[i],0)) return false;
        if (complete) return source_fail(ctx,arm,XR_XIR_BAD_STRUCTURE,"unreachable match arm is not admitted");
        if (!arm->as.match_arm.guard) {
            rows[row_count++]=&patterns[i];
            if (!source_pattern_coverage(ctx,(SourcePatternMatrix){&receiver.type,rows,1,row_count},0,&complete)) return false;
        }
    }
    if (!complete) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"enum match is not exhaustive");
    SourceFunction *body=&ctx->bodies[ctx->function];
    SourceName *saved=ctx->locals,*scope=ctx->scope; XrXirType result=XR_XIR_UNIT;
    uint32_t continuing=0;
    for (uint32_t i=0;i<count;++i) {
        MatchArmNode *arm=&match->arms[i]->as.match_arm; SourceMatchFailure *failures=NULL;
        ctx->locals=saved; ctx->scope=saved;
        if (!source_match_payload(ctx,&patterns[i],receiver,i+1<count,&failures,0)) return false;
        if (arm->guard) {
            SourceValue condition;
            if (!expression(ctx,arm->guard,&condition)) return false;
            if (condition.type!=XR_XIR_BOOL) return source_fail(ctx,arm->guard,XR_XIR_BAD_TYPE,"match guard requires bool");
            if (!source_match_test(ctx,condition,true,&failures)) return false;
        }
        SourceValue output;
        if (!source_match_body(ctx,arm->body,expected,&output)) return false;
        if (!ctx->returned) {
            if (!continuing) result=output.type;
            else if (result!=output.type) return source_fail(ctx,arm->body,XR_XIR_BAD_TYPE,"match arms need the same admitted type");
            inputs[continuing*2]=(SourceValue){body->block_count-1,XR_XIR_UNIT};
            inputs[continuing*2+1]=output; jumps[continuing++]=body->count;
            if (!emit(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{0},0},NULL)) return false;
        }
        uint32_t next=body->block_count;
        for (SourceMatchFailure *f=failures;f;f=f->next) body->ops[f->instruction].targets[f->target]=next;
        if (i+1<count && !begin_block(ctx)) return false;
    }
    ctx->locals=saved; ctx->scope=scope;
    if (!continuing) {
        if (result_required) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"never-valued expression context is not admitted");
        ctx->returned=true; *value=(SourceValue){0,XR_XIR_UNIT}; return true;
    }
    uint32_t join=body->block_count;
    if (!begin_block(ctx)) return false;
    for (uint32_t i=0;i<continuing;++i) body->ops[jumps[i]].targets[0]=join;
    if (result==XR_XIR_UNIT) { *value=(SourceValue){0,XR_XIR_UNIT}; return true; }
    return emit_group(ctx,(XrXirInstruction){XR_XIR_PHI,result,{0},{0},0},inputs,continuing*2,value);
}
