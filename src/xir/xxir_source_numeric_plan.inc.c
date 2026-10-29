/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_numeric_plan.inc.c - Numeric syntax scheduling and typed HIR consumption
 */
typedef struct SourceNumericRequest {
    const SourceInteger *integer;
    const SourceDecimal *decimal;
    SourceExpectedType expected;
    bool permit_default;
} SourceNumericRequest;
static bool source_hir_literal(SourceContext *ctx,AstNode *site,
    const SourceNumericRequest *request,XrHirLiteral *output) {
    if((!request->integer)==(!request->decimal))
        return source_fail(ctx,site,XR_XIR_BAD_STRUCTURE,"numeric plan requires one exact literal domain");
    /* Context propagation precedes checking. No typed HIR is published here. */
    if(!request->expected.present && !request->permit_default)return true;
    XrXirSourceRange range=source_query_range(ctx,site,NULL);
    XrHirLiteralInput input={.origin={site?site->node_id:0,range.module,range.line,range.column,range.end_line,range.end_column},
        .expected=request->expected.present,.expected_type=request->expected.type,.decimal=request->decimal!=NULL};
    if(request->integer) {input.negative=request->integer->negative;input.magnitude=request->integer->magnitude;}
    else {
        AstNode *node=request->decimal->node;
        input.negative=request->decimal->negative;
        if(node) {input.decimal_text=node->as.literal.decimal_text;input.decimal_length=node->as.literal.decimal_length;}
    }
    const char *reason=NULL;XrXirStatus status=xr_hir_literal_check(&ctx->types,&input,ctx->request,output,&reason);
    AstNode *failure=request->decimal?request->decimal->node:site;
    return status==XR_XIR_OK || source_fail(ctx,failure,status,reason);
}
static bool source_hir_literal_emit(SourceContext *ctx,const XrHirLiteral *literal,SourceValue *value) {
    return source_recipe_record(ctx,xr_hir_literal_instruction(literal),value);
}
static bool source_decimal(SourceContext *ctx,const SourceDecimal *literal,SourceExpectedType expected,SourceValue *value) {
    SourceNumericRequest request={NULL,literal,expected,true};XrHirLiteral checked={0};
    return source_hir_literal(ctx,literal->node,&request,&checked) && source_hir_literal_emit(ctx,&checked,value);
}
static bool source_integer(SourceContext *ctx,AstNode *node,const SourceInteger *literal,
    SourceExpectedType expected,SourceValue *value) {
    SourceNumericRequest request={literal,NULL,expected,true};XrHirLiteral checked={0};
    return source_hir_literal(ctx,node,&request,&checked) && source_hir_literal_emit(ctx,&checked,value);
}
