/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_binary_plan.inc.c - One numeric and string operator type recipe
 *
 * KEY CONCEPT:
 *   Operator admission and operand conversions are planned without values;
 *   emission consumes the completed recipe without repeating type decisions.
 */
typedef struct SourceBinaryRecipe {
    SourceConversionRecipe left, right;
    XrXirOp operation;
    XrXirType result;
    int64_t immediate;
} SourceBinaryRecipe;
static bool source_binary_plan(SourceContext *ctx,AstNode *node,AstNodeType operation,
    XrXirType left,XrXirType right,SourceBinaryRecipe *output) {
    SourceBinaryRecipe recipe={0};XrXirType target=left;
    if (operation==AST_BINARY_EQ || operation==AST_BINARY_NE) {
        if (left!=right) {
            if (xr_xir_type_is_nullable(&ctx->types,left) && xr_xir_nullable_element(&ctx->types,left)==right) target=left;
            else if (xr_xir_type_is_nullable(&ctx->types,right) && xr_xir_nullable_element(&ctx->types,right)==left) target=right;
            else return source_fail(ctx,node,XR_XIR_BAD_TYPE,"equality requires the same declared operand type");
        }
        XrXirDeclarations declarations;
        XrXirModule module=source_module_view(ctx,&declarations);
        XrXirProofContext proof={&module,{XR_XIR_CONTEXT_FUNCTION,ctx->function,0}};
        XrXirStatus status=xr_xir_compile_type_markers_prove(&ctx->compile, &proof, target, XR_XIR_CONSTRAINT_EQUAL);
        if (status!=XR_XIR_OK) return source_fail(ctx,node,status,"equality requires its declaration predicate");
        recipe.operation=XR_XIR_EQUAL; recipe.result=XR_XIR_BOOL; recipe.immediate=operation==AST_BINARY_NE;
    } else if (left==XR_XIR_STRING && right==XR_XIR_STRING && operation==AST_BINARY_ADD) {
        recipe.operation=XR_XIR_CONCAT_STRING;recipe.result=XR_XIR_STRING;
    } else if (left==XR_XIR_STRING && right==XR_XIR_STRING) {
        switch(operation) {
        case AST_BINARY_LT:recipe.operation=XR_XIR_LT_STRING;break;
        case AST_BINARY_LE:recipe.operation=XR_XIR_LE_STRING;break;
        case AST_BINARY_GT:recipe.operation=XR_XIR_GT_STRING;break;
        case AST_BINARY_GE:recipe.operation=XR_XIR_GE_STRING;break;
        default:return source_fail(ctx,node,XR_XIR_BAD_TYPE,"operator is not defined for string operands");
        }
        recipe.result=XR_XIR_BOOL;
    } else if (xr_xir_float_bits(left) && xr_xir_float_bits(right)) {
        target=left==XR_XIR_F64 || right==XR_XIR_F64 ? XR_XIR_F64 : XR_XIR_F32;
        switch(operation) {
        case AST_BINARY_ADD:recipe.operation=XR_XIR_ADD_FLOAT;break;
        case AST_BINARY_SUB:recipe.operation=XR_XIR_SUB_FLOAT;break;
        case AST_BINARY_MUL:recipe.operation=XR_XIR_MUL_FLOAT;break;
        case AST_BINARY_DIV:recipe.operation=XR_XIR_DIV_FLOAT;break;
        case AST_BINARY_LT:recipe.operation=XR_XIR_LT_FLOAT;break;
        case AST_BINARY_LE:recipe.operation=XR_XIR_LE_FLOAT;break;
        case AST_BINARY_GT:recipe.operation=XR_XIR_GT_FLOAT;break;
        case AST_BINARY_GE:recipe.operation=XR_XIR_GE_FLOAT;break;
        default:return source_fail(ctx,node,XR_XIR_BAD_TYPE,"operator is not defined for floating operands");
        }
        recipe.result=recipe.operation>=XR_XIR_ADD_FLOAT && recipe.operation<=XR_XIR_DIV_FLOAT ? target : XR_XIR_BOOL;
    } else {
        if (!xr_xir_type_is_integer(left) || !xr_xir_type_is_integer(right))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"operator requires a declared concrete operand contract");
        bool shift=operation==AST_BINARY_LSHIFT || operation==AST_BINARY_RSHIFT;
        if (!shift) {
            if (xr_xir_integer_signed(left)!=xr_xir_integer_signed(right))
                return source_fail(ctx,node,XR_XIR_BAD_TYPE,"integer signedness requires an explicit cast");
            target=xr_xir_integer_bits(left)<xr_xir_integer_bits(right) ? right : left;
        }
        switch(operation) {
        case AST_BINARY_ADD:recipe.operation=XR_XIR_ADD_INT;break;
        case AST_UNARY_NEG:case AST_BINARY_SUB:recipe.operation=XR_XIR_SUB_INT;break;
        case AST_BINARY_MUL:recipe.operation=XR_XIR_MUL_INT;break;
        case AST_BINARY_DIV:recipe.operation=XR_XIR_DIV_INT;break;
        case AST_BINARY_MOD:recipe.operation=XR_XIR_REM_INT;break;
        case AST_BINARY_BAND:recipe.operation=XR_XIR_AND_INT;break;
        case AST_BINARY_BOR:recipe.operation=XR_XIR_OR_INT;break;
        case AST_UNARY_BNOT:case AST_BINARY_BXOR:recipe.operation=XR_XIR_XOR_INT;break;
        case AST_BINARY_LSHIFT:recipe.operation=XR_XIR_SHL_INT;break;
        case AST_BINARY_RSHIFT:recipe.operation=XR_XIR_SHR_INT;break;
        case AST_BINARY_LT:recipe.operation=XR_XIR_LT_INT;break;
        case AST_BINARY_LE:recipe.operation=XR_XIR_LE_INT;break;
        case AST_BINARY_GT:recipe.operation=XR_XIR_GT_INT;break;
        case AST_BINARY_GE:recipe.operation=XR_XIR_GE_INT;break;
        default:return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"unknown arithmetic operator");
        }
        recipe.result=operation>=AST_BINARY_EQ && operation<=AST_BINARY_GE ? XR_XIR_BOOL : target;
    }
    bool shift=operation==AST_BINARY_LSHIFT || operation==AST_BINARY_RSHIFT;
    if (!source_conversion_plan(ctx,node,left,(SourceExpectedType){true,target, false},&recipe.left) ||
        !source_conversion_plan(ctx,node,right,(SourceExpectedType){true,shift ? right : target, false},&recipe.right)) return false;
    *output=recipe;return true;
}
static bool source_binary_emit(SourceContext *ctx,const SourceBinaryRecipe *recipe,
    SourceValue left,SourceValue right,SourceValue *value) {
    return source_conversion_emit(ctx,&recipe->left,&left) && source_conversion_emit(ctx,&recipe->right,&right) &&
        source_recipe_record(ctx,(XrXirInstruction){recipe->operation,recipe->result,{left.id,right.id},{0},recipe->immediate,{0}},value);
}
static bool source_binary_apply(SourceContext *ctx,AstNode *node,AstNodeType operation,
    SourceValue left,SourceValue right,SourceValue *value) {
    SourceBinaryRecipe recipe;
    return source_binary_plan(ctx,node,operation,left.type,right.type,&recipe) && source_binary_emit(ctx,&recipe,left,right,value);
}
