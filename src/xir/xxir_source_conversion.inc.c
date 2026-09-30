/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_conversion.inc.c - One conversion checker and an emission recipe
 *
 * KEY CONCEPT:
 *   Planning proves the original conversion once; emission only consumes the
 *   resulting private recipe and never repeats inference or permissions.
 */
typedef struct SourceExpectedType { bool present; XrXirType type; } SourceExpectedType;
typedef struct SourceConversionRecipe {
    bool needed;
    XrXirOp operation;
    XrXirType source, target;
} SourceConversionRecipe;
static bool source_conversion_plan(SourceContext *ctx, AstNode *node, XrXirType actual,
    SourceExpectedType expected, SourceConversionRecipe *output) {
    SourceConversionRecipe recipe = {false,(XrXirOp)0,actual,actual};
    if (!expected.present || actual == expected.type) { *output = recipe; return true; }
    XrXirType target = expected.type;
    if (xr_xir_type_is_callable(&ctx->types,target) && xr_xir_type_is_callable(&ctx->types,actual)) {
        XrXirStatus status = xr_xir_callable_weakening(&ctx->types,actual,target,&ctx->budget.work);
        if (status != XR_XIR_OK) return source_fail(ctx,node,status,"callable conversion may only discard its top-level promise");
        recipe.operation = XR_XIR_FUNCTION_WEAKEN;
    } else if (target == XR_XIR_ERROR) {
        XrXirDeclarations declarations;
        XrXirModule module = source_module_view(ctx,&declarations);
        XrXirProofContext context = {&module,{XR_XIR_CONTEXT_FUNCTION,ctx->function,0}};
        XrXirStatus status = xr_xir_type_markers_prove(&context,actual,XR_XIR_CONSTRAINT_ERROR,&ctx->budget);
        if (status != XR_XIR_OK) return source_fail(ctx,node,status,"Error conversion requires an enum proof");
        recipe.operation = XR_XIR_ERROR_ERASE;
    } else {
        if (!(actual == XR_XIR_F32 && target == XR_XIR_F64) &&
            (!xr_xir_type_is_integer(actual) || !xr_xir_type_is_integer(target) ||
             xr_xir_integer_signed(actual) != xr_xir_integer_signed(target) ||
             xr_xir_integer_bits(actual) > xr_xir_integer_bits(target)))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"expression cannot satisfy its declared type");
        recipe.operation = XR_XIR_CONVERT_NUMBER;
    }
    recipe.needed = true; recipe.target = target; *output = recipe; return true;
}
static bool source_conversion_emit(SourceContext *ctx, const SourceConversionRecipe *recipe,
    SourceValue *value) {
    /* This checks recipe/value plumbing, not a second conversion relation. */
    if (value->type != recipe->source)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"conversion recipe source changed");
    return !recipe->needed || emit(ctx,(XrXirInstruction){recipe->operation,recipe->target,
        {value->id,0},{0},0,{0}},value);
}
static bool source_expect(SourceContext *ctx, AstNode *node, XrXirType expected, SourceValue *value) {
    SourceConversionRecipe recipe;
    return source_conversion_plan(ctx,node,value->type,(SourceExpectedType){expected != XR_XIR_UNIT,expected},&recipe) &&
        source_conversion_emit(ctx,&recipe,value);
}
