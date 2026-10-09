/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_update.inc.c - Prepared member updates and compound expressions
 *
 * KEY CONCEPT:
 *   Read the old field before RHS effects and commit to the current root value.
 */
typedef struct SourceMemberPlace {
    SourceValue place;
    XrXirType type;
    uint32_t field;
    bool constructor, identity;
} SourceMemberPlace;
static bool source_member_place(SourceContext *ctx, AstNode *node, AstNode *object,
    const char *name, unsigned access, SourceMemberPlace *member) {
    if (source_constructor_receiver(ctx, object)) {
        member->constructor = true;
        return source_struct_field(ctx, node, visible_name(ctx, "this")->type, name, access, &member->field, &member->type);
    }
    XrXirType receiver_type;
    if (!source_path_type(ctx,object,&receiver_type)) return false;
    if (xr_xir_type_is_class(&ctx->types,receiver_type) || receiver_type == XR_XIR_UNIT) {
        if (!expression(ctx,object,&member->place)) return false;
        if (!xr_xir_type_is_class(&ctx->types,member->place.type))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"temporary member mutation requires a class identity");
        member->identity=true;
        return source_struct_field(ctx,node,member->place.type,name,access,&member->field,&member->type);
    }
    SourceName *root = object->type == AST_VARIABLE ? visible_name(ctx, object->as.variable.name) : NULL;
    if (root && root->type == XR_XIR_PANIC_INFO && (root->kind == SOURCE_LOCAL || root->kind == SOURCE_SLOT))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "PanicInfo field writes require class support");
    if (!source_value_place(ctx,object,&member->place) ||
        !source_struct_field(ctx,node,member->place.type,name,access,&member->field,&member->type)) return false;
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_FIELD_PLACE,member->type,{member->place.id},{0},member->field,{0}},&member->place);
}
static bool source_member_read(SourceContext *ctx, AstNode *node, const SourceMemberPlace *member, SourceValue *value) {
    if (member->constructor) return source_constructor_read(ctx, node, member->field, value);
    if (member->identity) return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CLASS_GET,member->type,{member->place.id},{0},member->field,{0}},value);
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_PLACE_READ,member->type,{member->place.id},{0},0,{0}},value);
}
static bool source_member_store(SourceContext *ctx, AstNode *node, const SourceMemberPlace *member, SourceValue value) {
    if (member->constructor) return source_constructor_store(ctx, node, member->field, value);
    if (member->identity) return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CLASS_SET,XR_XIR_UNIT,{member->place.id,value.id},{0},member->field,{0}},NULL);
    return source_recipe_record(ctx, (XrXirInstruction){XR_XIR_PLACE_WRITE, XR_XIR_UNIT, {member->place.id, value.id}, {0}, 0, {0}}, NULL);
}
static bool source_struct_set(SourceContext *ctx, AstNode *node, SourceValue *value) {
    MemberSetNode *set = &node->as.member_set;
    if (source_constructor_receiver(ctx, set->object)) return source_constructor_field(ctx, node, set->member, value, set->value);
    SourceMemberPlace member = {0};
    if (!source_member_place(ctx, node, set->object, set->member, 1, &member) ||
        !source_plan_expression(ctx, set->value, (SourceExpectedType){member.type != XR_XIR_UNIT,member.type, false, false}, value)) return false;
    if (value->type != member.type) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "field assignment type mismatch");
    return source_member_store(ctx, node, &member, *value);
}
static bool source_member_compound(SourceContext *ctx, AstNode *node, AstNodeType operation, SourceValue *value) {
    CompoundAssignmentNode *assignment = &node->as.compound_assignment;
    SourceMemberPlace member = {0}; SourceValue left, right;
    bool shift = operation == AST_BINARY_LSHIFT || operation == AST_BINARY_RSHIFT;
    if (!source_member_place(ctx, node, assignment->object, assignment->name, 3, &member) ||
        !source_member_read(ctx, node, &member, &left) ||
        !source_plan_expression(ctx, assignment->value, (SourceExpectedType){shift ? XR_XIR_UNIT : member.type != XR_XIR_UNIT,shift ? XR_XIR_UNIT : member.type, false, false}, &right) ||
        !source_binary_apply(ctx, node, operation, left, right, value)) return false;
    if (value->type != member.type) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "compound assignment cannot narrow its result");
    return source_member_store(ctx, node, &member, *value);
}
static bool source_compound(SourceContext *ctx, AstNode *node, SourceValue *value) {
    CompoundAssignmentNode *assignment = &node->as.compound_assignment;
    if (!assignment->name) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "compound assignment requires a target");
    AstNodeType operation;
    switch (assignment->op) {
    case TK_PLUS_ASSIGN: operation = AST_BINARY_ADD; break;
    case TK_MINUS_ASSIGN: operation = AST_BINARY_SUB; break;
    case TK_MUL_ASSIGN: operation = AST_BINARY_MUL; break;
    case TK_DIV_ASSIGN: operation = AST_BINARY_DIV; break;
    case TK_MOD_ASSIGN: operation = AST_BINARY_MOD; break;
    case TK_AND_ASSIGN: operation = AST_BINARY_BAND; break;
    case TK_OR_ASSIGN: operation = AST_BINARY_BOR; break;
    case TK_XOR_ASSIGN: operation = AST_BINARY_BXOR; break;
    case TK_LSHIFT_ASSIGN: operation = AST_BINARY_LSHIFT; break;
    case TK_RSHIFT_ASSIGN: operation = AST_BINARY_RSHIFT; break;
    default: return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "unknown compound assignment");
    }
    if (assignment->object) return source_member_compound(ctx, node, operation, value);
    SourceName *symbol = visible_name(ctx, assignment->name);
    if (!symbol || !symbol->mutable || (symbol->kind != SOURCE_LOCAL && symbol->kind != SOURCE_SLOT))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "compound assignment requires a mutable binding");
    if (!source_query_reference(ctx, node, symbol, symbol, XR_XIR_SOURCE_READ_WRITE)) return false;
    SourceValue left, right;

    bool shift = operation == AST_BINARY_LSHIFT || operation == AST_BINARY_RSHIFT;
    if (!source_binding_read(ctx, symbol, &left) || !source_plan_expression(ctx, assignment->value, (SourceExpectedType){shift ? XR_XIR_UNIT : symbol->type != XR_XIR_UNIT,shift ? XR_XIR_UNIT : symbol->type, false, false}, &right) ||
        !source_binary_apply(ctx, node, operation, left, right, value)) return false;
    if (value->type != symbol->type) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "compound assignment cannot narrow its result");
    return source_binding_write(ctx, symbol, *value);
}
