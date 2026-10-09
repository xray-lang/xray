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
#include "xxir_source_effect_bindings.inc.c"
static bool source_conversion_plan(SourceContext *ctx, AstNode *node, XrXirType actual,
    SourceExpectedType expected, SourceConversionRecipe *output) {
    SourceConversionRecipe recipe = {false,expected.inferred,(XrXirOp)0,actual,actual,false,actual};
    if (!expected.present) { *output = recipe; return true; }
    if (ctx->effect_argument && ctx->effect_argument->owner == ctx->function &&
        ctx->effect_argument->syntax == node && xr_xir_callable_signature(&ctx->types,actual) &&
        xr_xir_callable_signature(&ctx->types,expected.type)) {
        if (!source_effect_argument_recipe(ctx,node,actual,expected.type,&recipe)) return false;
        *output = recipe; return true;
    }
    if (actual == expected.type) {
        if (!expected.inferred && xr_xir_callable_signature(&ctx->types,actual)) {
            XrXirStatus status = xr_xir_compile_callable_weakening(&ctx->compile,
                &ctx->types,actual,expected.type);
            if (status != XR_XIR_OK) return source_fail(ctx,node,status,
                "fixed callable conversion requires its complete advertised shape");
            recipe.needed = true; recipe.operation = XR_XIR_FUNCTION_WEAKEN;
        } else if (source_root_type(ctx,actual,0)) {
            recipe.needed = true; recipe.operation = XR_XIR_COPY;
        }
        if (ctx->diagnostic.status != XR_XIR_OK) return false;
        *output = recipe; return true;
    }
    XrXirType target = expected.type;
    if (xr_xir_type_is_nullable(&ctx->types,target) && xr_xir_nullable_element(&ctx->types,target) == actual) {
        recipe.operation = XR_XIR_NULLABLE_SOME;
    } else if (xr_xir_type_is_callable(&ctx->types,target) && xr_xir_type_is_callable(&ctx->types,actual)) {
        XrXirStatus status = xr_xir_compile_callable_weakening(&ctx->compile, &ctx->types, actual, target);
        if (status != XR_XIR_OK) return source_fail(ctx,node,status,"callable conversion may only weaken its outer execution contract");
        recipe.operation = XR_XIR_FUNCTION_WEAKEN;
    } else if (target == XR_XIR_ERROR) {
        XrXirDeclarations declarations;
        XrXirModule module = source_module_view(ctx,&declarations);
        XrXirProofContext context = {&module,{XR_XIR_CONTEXT_FUNCTION,ctx->function,0}};
        XrXirStatus status = xr_xir_compile_type_markers_prove(&ctx->compile, &context, actual, XR_XIR_CONSTRAINT_ERROR);
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
    if (!recipe->needed) return true;
    if (recipe->operation == XR_XIR_NULLABLE_SOME && !recipe->inferred) {
        bool contains_callable = source_root_type(ctx,recipe->source,0);
        if (ctx->diagnostic.status != XR_XIR_OK) return false;
        if (contains_callable) {
            /* The wrapper preserves its fixed element identity while the
             * ordinary reference feeding this boundary may retain precision. */
            if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_COPY,recipe->source,
                {value->id,0},{0},0,{0}},value)) return false;
            ctx->bodies[ctx->function].recipes[value->id - ctx->functions[ctx->function].parameter_count].root.kind = 1;
        }
    }
    if (!source_recipe_record(ctx,(XrXirInstruction){recipe->operation,recipe->target,
        {value->id,0},{0},0,{0}},value)) return false;
    if (recipe->operation == XR_XIR_COPY || recipe->operation == XR_XIR_FUNCTION_WEAKEN)
        ctx->bodies[ctx->function].recipes[value->id - ctx->functions[ctx->function].parameter_count].root.kind = recipe->inferred ? 2 : 1;
    if (recipe->effect_binding) {
        if (!source_work(ctx,NULL)) return false;
        SourceRootValue *root = &ctx->bodies[ctx->function].recipes[
            value->id - ctx->functions[ctx->function].parameter_count].root;
        root->kind = 5; root->declared = recipe->declared_bound;
    }
    return true;
}
