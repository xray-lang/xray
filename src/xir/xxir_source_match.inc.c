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
#include "xxir_source_pattern_bindings.inc.c"
typedef struct SourceMatchPlan {
    SourceMatchArm *arms;
    SourceExpressionPlan **guards, **values;
    uint32_t count;
    bool exhaustive;
} SourceMatchPlan;
/* Pattern declarations own lexical identity before any runtime payload exists. */
static bool source_match_prepare(SourceContext *ctx, SourceExpressionPlan *plan) {
    if (plan->match || !plan->left->type_ready) return true;
    AstNode *node=plan->syntax;MatchExprNode *match=&node->as.match_expr;
    if (match->arm_count<=0 || (uint32_t)match->arm_count>UINT32_MAX/2)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"match requires arms and a scrutinee");
    XrXirType type=plan->left->ground_type;
    if (!xr_xir_type_is_enum(&ctx->types,type) && type!=XR_XIR_BOOL && type!=XR_XIR_STRING && !xr_xir_type_is_number(type))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"match scrutinee family is not admitted");
    SourceMatchPlan *prepared=source_recipe_storage(ctx,1,sizeof(*prepared));
    if (!prepared) return false;
    prepared->count=(uint32_t)match->arm_count;
    if (!source_match_normalize_arms(ctx,node,type,&prepared->arms,&prepared->exhaustive)) return false;
    prepared->guards=source_recipe_storage(ctx,prepared->count,sizeof(*prepared->guards));
    prepared->values=source_recipe_storage(ctx,prepared->count,sizeof(*prepared->values));
    if (!prepared->guards || !prepared->values) return false;
    SourceName *saved=ctx->locals,*scope=ctx->scope;bool ready=true;XrXirType result=XR_XIR_UNIT;
    for (uint32_t i=0;i<prepared->count;++i) {
        if (!source_work(ctx,node)) return false;
        ctx->locals=saved;ctx->scope=saved;
        for (SourceName *symbol=prepared->arms[i].bindings;symbol;symbol=symbol->next) {
            if (!source_work(ctx,symbol->node)) return false;
            SourceName *bound=source_recipe_storage(ctx,1,sizeof(*bound));
            if (!bound) return false;
            *bound=*symbol;bound->next=ctx->locals;ctx->locals=bound;
        }
        MatchArmNode *arm=&match->arms[i]->as.match_arm;
        if (arm->guard && !(prepared->guards[i]=source_plan_collect(ctx,arm->guard,(SourceExpectedType){false,XR_XIR_UNIT}))) return false;
        /* A block introduces declarations in sequence; collect it at that lexical event. */
        if (arm->body->type!=AST_BLOCK) {
            prepared->values[i]=source_plan_collect(ctx,arm->body,(SourceExpectedType){false,XR_XIR_UNIT});
            if (!prepared->values[i]) return false;
            if (!prepared->values[i]->type_ready) ready=false;
            else if (!i) result=prepared->values[i]->ground_type;
            else if (result!=prepared->values[i]->ground_type) ready=false;
        } else ready=false;
    }
    ctx->locals=saved;ctx->scope=scope;plan->match=prepared;
    if (ready) {plan->type_ready=true;plan->ground_type=result;}
    return true;
}
static bool source_match_bind(SourceContext *ctx, SourceName *symbol, SourceValue value) {
    if (!symbol || symbol->type!=value.type) return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"pattern binding type is inconsistent");
    SourceName *binding=source_alloc(ctx,1,sizeof(*binding));
    if (!binding) return false;
    *binding=*symbol; binding->next=ctx->locals; binding->index=value.id; ctx->locals=binding;
    return true;
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
    XrXirInstruction branch={XR_XIR_BRANCH,XR_XIR_UNIT,{condition.id,0},{0},0, {0}};
    branch.targets[truth?0:1]=body->block_count;
    return source_recipe_record(ctx,branch,NULL) && begin_block(ctx);
}
static bool source_pattern_integer_value(SourceContext *ctx, XrXirType type, uint64_t key, SourceValue *value) {
    uint64_t bias=source_pattern_integer_bias(type), bits=key^bias;
    if (bits&bias) bits|=~source_pattern_integer_max(type);
    int64_t payload=bits<=INT64_MAX?(int64_t)bits:-1-(int64_t)(UINT64_MAX-bits);
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,type,{0},{0},payload, {0}},value);
}
static bool source_match_payload(SourceContext *ctx, SourceMatchPattern *pattern,
    SourceValue receiver, bool test, SourceMatchFailure **failures, uint32_t depth) {
    if (!pattern) return true;
    if (!source_work(ctx,pattern->node)) return false;
    if (depth>=128) return source_fail(ctx,pattern->node,XR_XIR_BUDGET,"pattern generation depth exhausted");
    if (pattern->binding) return source_match_bind(ctx,pattern->symbol,receiver);
    if (pattern->any) return true;
    if (pattern->boolean) return !test || source_match_test(ctx,receiver,pattern->truth,failures);
    if (pattern->node->type==AST_PATTERN_RANGE) {
        if (!test) return true;
        SourceValue low,high,condition;
        if (pattern->empty) return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_BOOL,XR_XIR_BOOL,{0},{0},0, {0}},&condition) &&
            source_match_test(ctx,condition,true,failures);
        return source_pattern_integer_value(ctx,pattern->type,pattern->low,&low) &&
            source_binary_apply(ctx,pattern->node,AST_BINARY_GE,receiver,low,&condition) &&
            source_match_test(ctx,condition,true,failures) &&
            source_pattern_integer_value(ctx,pattern->type,pattern->high,&high) &&
            source_binary_apply(ctx,pattern->node,AST_BINARY_LE,receiver,high,&condition) &&
            source_match_test(ctx,condition,true,failures);
    }
    if (pattern->literal) {
        SourceValue literal,condition;
        if (pattern->type==XR_XIR_STRING) {
            if (!source_literal(ctx,pattern->literal,&literal)) return false;
        } else if (!source_recipe_record(ctx,(XrXirInstruction){xr_xir_float_bits(pattern->type)?XR_XIR_CONST_FLOAT:XR_XIR_CONST_INT,
            pattern->type,{0},{0},pattern->literal_payload, {0}},&literal)) return false;
        return !test || (source_binary_apply(ctx,pattern->node,AST_BINARY_EQ,receiver,literal,&condition) &&
            source_match_test(ctx,condition,true,failures));
    }
    SourceEnumSelection *selected=&pattern->selection;
    if (test) {
        SourceValue tag,ordinal,condition;
        if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ENUM_TAG,XR_XIR_I64,{receiver.id,0},{0},0, {0}},&tag) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},selected->variant, {0}},&ordinal) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_EQ_INT,XR_XIR_BOOL,{tag.id,ordinal.id},{0},0, {0}},&condition) ||
            !source_match_test(ctx,condition,true,failures)) return false;
    }
    for (uint32_t i=0;i<pattern->count;++i) {
        if (!source_work(ctx,pattern->node)) return false;
        SourceMatchPattern *child=pattern->fields[i];
        if (!child || (child->any && !child->binding)) continue;
        SourceValue value;
        if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ENUM_GET,child->type,{receiver.id,i},{0},selected->variant, {0}},&value) ||
            !source_match_payload(ctx,child,value,test,failures,depth+1)) return false;
    }
    return true;
}
#include "xxir_source_match_alternatives.inc.c"
static bool source_match_body(SourceContext *ctx, AstNode *node, SourceExpectedType expected, SourceValue *value) {
    if (node->type != AST_BLOCK) return source_plan_expression(ctx, node, expected, value);
    *value=(SourceValue){0,XR_XIR_UNIT};
    for (int i=0;i<node->as.block.count;++i) {
        AstNode *part=node->as.block.statements[i];
        if (i+1==node->as.block.count && part->type==AST_EXPR_STMT)
            return source_plan_expression(ctx, part->as.expr_stmt, expected, value);
        if (!statement(ctx,part,false)) return false;
        if (ctx->returned) {
            if (i+1!=node->as.block.count) return source_fail(ctx,part,XR_XIR_BAD_STRUCTURE,"unreachable match statements are not admitted");
            return true;
        }
    }
    return true;
}
static bool source_match(SourceContext *ctx, AstNode *node, SourceExpectedType expected, bool result_required, SourceValue *value) {
    MatchExprNode *match=&node->as.match_expr; SourceValue receiver;
    SourceExpressionPlan *plan=ctx->active_expression;
    if (!plan || plan->syntax!=node || !plan->left || !source_plan_complete(ctx,plan->left,&receiver)) return false;
    if (!source_match_prepare(ctx,plan)) return false;
    SourceMatchPlan *prepared=plan->match;
    if (!prepared) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"match scrutinee type is not ready");
    uint32_t count=prepared->count;SourceMatchArm *patterns=prepared->arms;bool exhaustive=prepared->exhaustive;
    SourceValue *inputs=source_alloc(ctx,count*2,sizeof(*inputs)); uint32_t *jumps=source_alloc(ctx,count,sizeof(*jumps));
    if (!inputs || !jumps) return false;
    SourceFunction *body=&ctx->bodies[ctx->function];
    SourceName *saved=ctx->locals,*scope=ctx->scope; XrXirType result=XR_XIR_UNIT;
    if (!body->block_count && !begin_block(ctx)) return false;
    uint32_t continuing=0;
    for (uint32_t i=0;i<count;++i) {
        MatchArmNode *arm=&match->arms[i]->as.match_arm; SourceMatchFailure *failures=NULL;
        ctx->locals=saved; ctx->scope=saved;
        if (!source_match_alternatives(ctx,&patterns[i],receiver,i+1<count || !exhaustive,&failures)) return false;
        if (arm->guard) {
            SourceValue condition;
            if (!source_plan_complete(ctx,prepared->guards[i],&condition)) return false;
            if (condition.type!=XR_XIR_BOOL) return source_fail(ctx,arm->guard,XR_XIR_BAD_TYPE,"match guard requires bool");
            if (!source_match_test(ctx,condition,true,&failures)) return false;
        }
        SourceValue output;
        if (prepared->values[i]) {
            prepared->values[i]->expected=expected;
            if (!source_plan_complete(ctx,prepared->values[i],&output)) return false;
        } else if (!source_match_body(ctx,arm->body,expected,&output)) return false;
        if (!ctx->returned) {
            if (!continuing) result=output.type;
            else if (result!=output.type) return source_fail(ctx,arm->body,XR_XIR_BAD_TYPE,"match arms need the same admitted type");
            inputs[continuing*2]=(SourceValue){body->current_block.identity,XR_XIR_UNIT};
            inputs[continuing*2+1]=output; jumps[continuing++]=body->count;
            if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{0},0, {0}},NULL)) return false;
        }
        uint32_t next=body->block_count;
        for (SourceMatchFailure *f=failures;f;f=f->next) body->recipes[f->instruction].instruction.targets[f->target]=next;
        if (i+1<count && !begin_block(ctx)) return false;
    }
    ctx->locals=saved; ctx->scope=scope;
    if (!exhaustive && (!begin_block(ctx) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_MATCH_FAIL,XR_XIR_UNIT,{0},{0},0, {0}},NULL))) return false;
    if (!continuing) {
        if (result_required) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"never-valued expression context is not admitted");
        ctx->returned=true; *value=(SourceValue){0,XR_XIR_UNIT}; return true;
    }
    uint32_t join=body->block_count;
    if (!begin_block(ctx)) return false;
    for (uint32_t i=0;i<continuing;++i) body->recipes[jumps[i]].instruction.targets[0]=join;
    if (result==XR_XIR_UNIT) { *value=(SourceValue){0,XR_XIR_UNIT}; return true; }
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_PHI,result,{0},{0},0, {0}},inputs,continuing*2,value);
}
