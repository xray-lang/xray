/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_numeric_plan.inc.c - Exact numeric payload before constant emission
 *
 * KEY CONCEPT:
 *   Literal syntax retains its exact domain until a context or the driver's final
 *   default phase chooses a type. The existing payload check is the only judge.
 */
typedef struct SourceNumericRequest {
    const SourceInteger *integer;
    const SourceDecimal *decimal;
    SourceExpectedType expected;
    bool permit_default;
} SourceNumericRequest;
static bool source_numeric_plan(SourceContext *ctx, AstNode *site,
    const SourceNumericRequest *request, SourceNumericRecipe *output) {
    if ((!request->integer) == (!request->decimal))
        return source_fail(ctx,site,XR_XIR_BAD_STRUCTURE,"numeric plan requires one exact literal domain");
    if (!request->expected.present && !request->permit_default) {
        *output = (SourceNumericRecipe){0}; return true;
    }
    XrXirType expected = request->expected.present ? request->expected.type : XR_XIR_UNIT;
    if (xr_xir_type_is_nullable(&ctx->types,expected)) expected = xr_xir_nullable_element(&ctx->types,expected);
    SourceNumericRecipe recipe = {true,XR_XIR_CONST_INT,XR_XIR_I64,0};
    uint64_t bits = 0;
    if (request->decimal) {
        recipe.operation = XR_XIR_CONST_FLOAT;
        recipe.type = request->expected.present && xr_xir_float_bits(expected) ? expected : XR_XIR_F64;
        if (!source_decimal_payload(ctx,request->decimal,recipe.type,&bits)) return false;
        memcpy(&recipe.payload,&bits,sizeof(bits));
    } else if (request->expected.present && xr_xir_float_bits(expected)) {
        recipe.operation = XR_XIR_CONST_FLOAT; recipe.type = expected;
        if (!source_integer_float_payload(ctx,site,request->integer,recipe.type,&bits)) return false;
        memcpy(&recipe.payload,&bits,sizeof(bits));
    } else {
        recipe.type = request->expected.present && xr_xir_type_is_integer(expected) ? expected : XR_XIR_I64;
        if (!source_integer_payload(ctx,site,request->integer,recipe.type,&bits)) return false;
        recipe.payload = bits <= INT64_MAX ? (int64_t)bits : -1-(int64_t)(UINT64_MAX-bits);
    }
    *output = recipe; return true;
}
static bool source_numeric_emit(SourceContext *ctx, const SourceNumericRecipe *recipe, SourceValue *value) {
    if (!recipe->ready) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"unresolved numeric recipe cannot be emitted");
    return source_recipe_record(ctx,(XrXirInstruction){recipe->operation,recipe->type,{0},{0},recipe->payload,{0}},value);
}
static bool source_decimal(SourceContext *ctx, const SourceDecimal *literal, SourceExpectedType expected, SourceValue *value) {
    SourceNumericRequest request = {NULL,literal,expected,true};
    SourceNumericRecipe recipe;
    return source_numeric_plan(ctx,literal->node,&request,&recipe) && source_numeric_emit(ctx,&recipe,value);
}
static bool source_integer(SourceContext *ctx, AstNode *node, const SourceInteger *literal,
    SourceExpectedType expected, SourceValue *value) {
    SourceNumericRequest request = {literal,NULL,expected,true};
    SourceNumericRecipe recipe;
    return source_numeric_plan(ctx,node,&request,&recipe) && source_numeric_emit(ctx,&recipe,value);
}
