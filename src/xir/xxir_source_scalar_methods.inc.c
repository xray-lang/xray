/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_scalar_methods.inc.c - Governed floating, boolean and rune calls
 *
 * KEY CONCEPT: Exact declaration identities select ordinary verified operations.
 */
static uint32_t source_scalar_identity(XrXirType type) {
    return xr_xir_float_bits(type) ? XR_NATIVE_DECLARATION_FLOAT :
        type==XR_XIR_BOOL ? XR_NATIVE_DECLARATION_BOOLEAN :
        type==XR_XIR_RUNE ? XR_NATIVE_DECLARATION_RUNE : 0;
}
static bool source_float_abs(SourceContext *ctx,SourceValue receiver,SourceValue *value) {
    SourceValue canonical,zero,negative,negated,selected,is_zero;
    /* A same-width numeric conversion canonicalizes NaNs. Explicitly choosing
     * positive zero avoids treating -0 as a negative comparison operand. */
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONVERT_NUMBER,XR_XIR_F64,{receiver.id,0},{0},0,{0}},&canonical) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_FLOAT,XR_XIR_F64,{0},{0},0,{0}},&zero) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_LT_FLOAT,XR_XIR_BOOL,{canonical.id,zero.id},{0},0,{0}},&negative) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_NEG_FLOAT,XR_XIR_F64,{canonical.id,0},{0},0,{0}},&negated) &&
        source_integer_pick(ctx,negative,negated,canonical,&selected) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_EQ_FLOAT,XR_XIR_BOOL,{canonical.id,zero.id},{0},0,{0}},&is_zero) &&
        source_integer_pick(ctx,is_zero,zero,selected,value);
}
static bool source_float_rounding(SourceContext *ctx,SourceValue receiver,
    XrNativeOperation operation,SourceValue *value) {
    SourceValue integral,back,fraction,one,zero,high,low,condition;
    /* Compare the exact fractional difference instead of adding 0.5 to x:
     * that shortcut rounds adjacent half values and odd large integers wrong.
     * A nonzero fraction implies magnitude below 2^52, so selected +/-1 fits.
     * The sole conversion retains the normal numeric-range panic boundary. */
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONVERT_NUMBER,XR_XIR_I64,{receiver.id,0},{0},0,{0}},&integral) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONVERT_NUMBER,XR_XIR_F64,{integral.id,0},{0},0,{0}},&back) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_SUB_FLOAT,XR_XIR_F64,{receiver.id,back.id},{0},0,{0}},&fraction) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},1,{0}},&one) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_FLOAT,XR_XIR_F64,{0},{0},0,{0}},&zero) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ADD_INT,XR_XIR_I64,{integral.id,one.id},{0},0,{0}},&high) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_SUB_INT,XR_XIR_I64,{integral.id,one.id},{0},0,{0}},&low)) return false;
    if (operation==XR_NATIVE_OPERATION_FLOAT_FLOOR || operation==XR_NATIVE_OPERATION_FLOAT_CEIL) {
        bool floor=operation==XR_NATIVE_OPERATION_FLOAT_FLOOR;
        return source_recipe_record(ctx,(XrXirInstruction){floor ? XR_XIR_LT_FLOAT : XR_XIR_GT_FLOAT,
                XR_XIR_BOOL,{fraction.id,zero.id},{0},0,{0}},&condition) &&
            source_integer_pick(ctx,condition,floor ? low : high,integral,value);
    }
    SourceValue half,negative_half,selected;
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_FLOAT,XR_XIR_F64,{0},{0},INT64_C(0x3fe0000000000000),{0}},&half) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_NEG_FLOAT,XR_XIR_F64,{half.id,0},{0},0,{0}},&negative_half) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_GE_FLOAT,XR_XIR_BOOL,{fraction.id,half.id},{0},0,{0}},&condition) &&
        source_integer_pick(ctx,condition,high,integral,&selected) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_LE_FLOAT,XR_XIR_BOOL,{fraction.id,negative_half.id},{0},0,{0}},&condition) &&
        source_integer_pick(ctx,condition,low,selected,value);
}
static bool source_scalar_call(SourceContext *ctx,AstNode *node,SourceValue receiver,SourceValue *value) {
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(source_scalar_identity(receiver.type));
    if (!native || !source_native_admit(ctx,native)) return false;
    CallExprNode *call=&node->as.call_expr;
    const XrNativeMemberDeclaration *member=source_native_member(ctx,native,call->callee->as.member_access.name);
    if (!member || !member->is_public || !member->is_method || member->is_static ||
        member->receiver!=XR_NATIVE_RECEIVER_READ || member->operation==XR_NATIVE_OPERATION_NONE ||
        member->parameter_count || call->type_arg_count || call->default_arg_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"scalar method requires its exact admitted ordinary declaration");
    bool text=member->operation==XR_NATIVE_OPERATION_FLOAT_TO_STRING ||
        member->operation==XR_NATIVE_OPERATION_BOOL_TO_STRING || member->operation==XR_NATIVE_OPERATION_RUNE_TO_STRING;
    if (!text && xr_xir_float_bits(receiver.type) && receiver.type!=XR_XIR_F64)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"floating operation requires the declared f64 receiver");
    if (!source_primitive_member_reference(ctx,call->callee,native,member)) return false;
    uint32_t supplied;
    if (!source_native_read_arguments(ctx,node,NULL,0,0,NULL,&supplied,
        "scalar method requires its exact admitted ordinary declaration")) return false;
    if (text) return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_TO_STRING,XR_XIR_STRING,{receiver.id,0},{0},0,{0}},value);
    switch (member->operation) {
    case XR_NATIVE_OPERATION_RUNE_TO_U32:
        return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_RUNE_TO_INTEGER,XR_XIR_U32,{receiver.id,0},{0},0,{0}},value);
    case XR_NATIVE_OPERATION_FLOAT_ABS: return source_float_abs(ctx,receiver,value);
    case XR_NATIVE_OPERATION_FLOAT_TO_I64:
        return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONVERT_NUMBER,XR_XIR_I64,{receiver.id,0},{0},0,{0}},value);
    case XR_NATIVE_OPERATION_FLOAT_IS_NAN:
        return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_NE_FLOAT,XR_XIR_BOOL,{receiver.id,receiver.id},{0},0,{0}},value);
    case XR_NATIVE_OPERATION_FLOAT_FLOOR: case XR_NATIVE_OPERATION_FLOAT_CEIL: case XR_NATIVE_OPERATION_FLOAT_ROUND:
        return source_float_rounding(ctx,receiver,member->operation,value);
    default: return source_fail(ctx,node,XR_XIR_BAD_TYPE,"scalar operation has no admitted recipe");
    }
}
static bool source_scalar_ground_result(SourceContext *ctx,AstNode *node,
    SourceExpectedType receiver,SourceExpectedType *output) {
    if (!receiver.present) {
        AstNode *object=node->as.call_expr.callee->as.member_access.object;
        while (object && object->type==AST_GROUPING) object=object->as.grouping;
        if (!object) return true;
        if (object->type==AST_LITERAL_TRUE || object->type==AST_LITERAL_FALSE)
            receiver=(SourceExpectedType){true,XR_XIR_BOOL,false,false};
        else if (object->type==AST_LITERAL_RUNE)
            receiver=(SourceExpectedType){true,XR_XIR_RUNE,false,false};
        else {
            SourceDecimal decimal;
            if (!source_direct_decimal(ctx,object,&decimal)) return false;
            if (!decimal.node) return true;
            XrHirLiteral recipe={0};
            SourceNumericRequest request={NULL,&decimal,{false,XR_XIR_UNIT,false,false},true};
            if (!source_hir_literal(ctx,object,&request,&recipe)) return false;
            receiver=(SourceExpectedType){true,recipe.type,false,false};
        }
    }
    uint32_t identity=source_scalar_identity(receiver.type);
    if (!identity) return true;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(identity);
    if (!source_native_admit(ctx,native)) return false;
    const XrNativeMemberDeclaration *member=source_native_member(ctx,native,node->as.call_expr.callee->as.member_access.name);
    if (!member || member->operation==XR_NATIVE_OPERATION_NONE || member->is_static || !member->is_method) return true;
    XrXirType type;
    if (!source_primitive_result_type(ctx,member->result,&type)) return false;
    *output=(SourceExpectedType){true,type,false,false};return true;
}
