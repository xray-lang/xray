/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_closure_error_bound.inc.c - Empty business-error bounds for anonymous bodies
 *
 * KEY CONCEPT:
 *   Only this producer's unfinished anonymous bodies may refine their bound.
 *   Direct calls use the actual substituted declaration, never a callee body.
 */
static bool source_recipe_call_never(SourceContext *ctx,const XrXirInstruction *op,bool *output) {
    *output=false;
    if (op->immediate<0 || (uint64_t)op->immediate>=ctx->function_count) return true;
    uint32_t callee=(uint32_t)op->immediate;
    const XrXirGeneric *caller=&ctx->generics[ctx->function];
    uint32_t first=op->type_arguments[0],count=op->type_arguments[1];
    if (count!=ctx->generics[callee].parameter_count || first>caller->argument_count ||
        count>caller->argument_count-first || (count && !caller->arguments) || (!count && first))
        return true;
    SourceSubstitution substitution={count ? caller->arguments+first : NULL,count};
    XrXirType error=XR_XIR_UNIT;
    if (!source_substitute(ctx,&substitution,ctx->functions[callee].error_type,0,&error)) return false;
    *output=error==XR_XIR_NEVER;
    return true;
}

static XrXirType source_recipe_operand_type(SourceContext *ctx,uint32_t ordinal,uint32_t value) {
    /* Instructions are not published yet. Parameters and earlier owned recipes
     * are the only type authority while the anonymous body is unfinished. */
    const XrXirFunction *function=&ctx->functions[ctx->function];
    const SourceFunction *body=&ctx->bodies[ctx->function];
    if (value<function->parameter_count)
        return function->parameters ? function->parameters[value] : XR_XIR_UNIT;
    uint32_t index=value-function->parameter_count;
    if (!body->recipes || index>=ordinal || index>=body->count) return XR_XIR_UNIT;
    const SourceInstructionRecipe *recipe=&body->recipes[index];
    return recipe->owner==ctx->function && recipe->value==value ?
        recipe->instruction.type : XR_XIR_UNIT;
}

static bool source_recipe_operand_i64(SourceContext *ctx,uint32_t ordinal,uint32_t value) {
    return source_recipe_operand_type(ctx,ordinal,value)==XR_XIR_I64;
}

static bool source_recipe_never(SourceContext *ctx,const XrXirInstruction *op,
    uint32_t ordinal,bool *output) {
    *output=true;
    switch (op->op) {
    /* Materialization, value reads and integer bit arithmetic have no business
     * error result. Their runtime resource and execution failures stay distinct. */
    case XR_XIR_CONST_BOOL: case XR_XIR_CONST_INT: case XR_XIR_CONST_FLOAT:
    case XR_XIR_CONST_RUNE: case XR_XIR_CONST_STRING:
    case XR_XIR_COPY: case XR_XIR_SLOT_LOAD: case XR_XIR_CELL_READ:
    case XR_XIR_ADD_INT: case XR_XIR_SUB_INT: case XR_XIR_MUL_INT:
    case XR_XIR_RETURN:
        return true;
    case XR_XIR_REM_INT:
        *output=op->type==XR_XIR_I64 &&
            source_recipe_operand_i64(ctx,ordinal,op->args[0]) &&
            source_recipe_operand_i64(ctx,ordinal,op->args[1]);
        return true;
    case XR_XIR_EQUAL:
        *output=op->type==XR_XIR_BOOL && op->immediate==0 &&
            source_recipe_operand_i64(ctx,ordinal,op->args[0]) &&
            source_recipe_operand_i64(ctx,ordinal,op->args[1]);
        return true;
    case XR_XIR_CELL_WRITE:
        *output=op->type==XR_XIR_UNIT &&
            xr_xir_cell_element(&ctx->types,source_recipe_operand_type(ctx,ordinal,op->args[0]))==XR_XIR_I64 &&
            source_recipe_operand_i64(ctx,ordinal,op->args[1]);
        return true;
    case XR_XIR_CALL_INDIRECT: {
        *output=false;
        if (op->immediate<0 || (uint64_t)op->immediate>UINT32_MAX) return true;
        XrXirType type=source_recipe_operand_type(ctx,ordinal,(uint32_t)op->immediate);
        const XrXirTypeNode *signature=xr_xir_callable_signature(&ctx->types,type);
        *output=signature && signature->element==XR_XIR_NEVER && signature->result==op->type;
        return true;
    }
    case XR_XIR_CALL:
        return source_recipe_call_never(ctx,op,output);
    default:
        *output=false;
        return true;
    }
}

static bool source_closure_error_bound(SourceContext *ctx) {
    uint32_t function=ctx->function;
    if (ctx->bodies[function].checked_library || ctx->functions[function].error_type!=XR_XIR_ERROR ||
        !ctx->bodies[function].node || ctx->bodies[function].node->type!=AST_FUNCTION_EXPR ||
        !ctx->bodies[function].count ||
        ctx->bodies[function].node->as.function_expr.throws_clause.syntax!=XR_THROWS_OMITTED ||
        (ctx->identities[function].promises & XR_XIR_FUNCTION_GENERATOR))
        return true;
    for (uint32_t i=0;i<ctx->bodies[function].count;++i) {
        if (!source_work(ctx,ctx->bodies[function].node)) return false;
        XrXirInstruction op=ctx->bodies[function].recipes[i].instruction;
        bool empty=false;
        if (!source_recipe_never(ctx,&op,i,&empty)) return false;
        if (!empty) return true;
    }
    ctx->functions[function].error_type=XR_XIR_NEVER;
    return true;
}
