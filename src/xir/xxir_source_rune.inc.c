/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_rune.inc.c - Explicit scalar construction and code point queries
 *
 * KEY CONCEPT:
 *   Rune stays independent of numeric coercion. One evaluated i64 input feeds
 *   the existing checked range-panic path, without an Error effect or suspension.
 */
static bool source_rune_call(SourceContext *ctx,AstNode *node,bool construct,SourceValue *value) {
    CallExprNode *call=&node->as.call_expr;SourceValue input;
    if (call->arg_count!=1 || call->type_arg_count || !call->arguments ||
        (call->arg_accesses && call->arg_accesses[0]!=XR_CALL_ARG_PLAIN))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Rune conversion takes one plain value");
    if (!source_plan_expression(ctx,call->arguments[0],(SourceExpectedType){false,XR_XIR_UNIT,false},&input)) return false;
    if (input.type!=(construct?XR_XIR_I64:XR_XIR_RUNE))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,construct?"rune construction requires i64":"code point conversion requires rune");
    return source_recipe_record(ctx,(XrXirInstruction){construct?XR_XIR_INTEGER_TO_RUNE:XR_XIR_RUNE_TO_INTEGER,
        construct?XR_XIR_RUNE:XR_XIR_I64,{input.id,0},{0},0,{0}},value);
}
static bool source_rune_codepoint(SourceContext *ctx,AstNode *node,SourceValue receiver,SourceValue *value) {
    CallExprNode *call=&node->as.call_expr;
    if (call->arg_count || call->type_arg_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"toUInt32 accepts no value or type arguments");
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_RUNE_TO_INTEGER,XR_XIR_U32,{receiver.id,0},{0},0,{0}},value);
}
