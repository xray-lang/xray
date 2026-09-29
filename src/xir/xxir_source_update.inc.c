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
    SourceName *root;
    SourceValue place;
    XrXirType type;
    uint32_t field;
    bool constructor;
} SourceMemberPlace;
static bool source_member_place(SourceContext *ctx, AstNode *node, AstNode *object,
    const char *name, unsigned access, SourceMemberPlace *member) {
    if (source_constructor_receiver(ctx, object)) {
        member->constructor = true;
        return source_struct_field(ctx, node, visible_name(ctx, "this")->type, name, access, &member->field, &member->type);
    }
    SourceName *root = object->type == AST_VARIABLE ? visible_name(ctx, object->as.variable.name) : NULL;
    if (root && root->type == XR_XIR_PANIC_INFO && (root->kind == SOURCE_LOCAL || root->kind == SOURCE_SLOT))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "PanicInfo field writes require class support");
    if (!root || !root->mutable || (root->kind != SOURCE_LOCAL && root->kind != SOURCE_SLOT))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "field mutation requires a mutable named root");
    member->root = root;
    if (!source_struct_field(ctx, node, root->type, name, access, &member->field, &member->type) ||
        !source_query_reference(ctx, object, root, root, XR_XIR_SOURCE_READ_WRITE)) return false;
    return emit(ctx, (XrXirInstruction){root->kind == SOURCE_SLOT ? XR_XIR_SLOT_PLACE : XR_XIR_CELL_PLACE,
        root->type, {root->kind == SOURCE_SLOT ? 0 : root->index, 0}, {0}, root->kind == SOURCE_SLOT ? root->index : 0, {0}}, &member->place);
}
static bool source_member_read(SourceContext *ctx, AstNode *node, const SourceMemberPlace *member, SourceValue *value) {
    if (member->constructor) return source_constructor_read(ctx, node, member->field, value);
    SourceName *root = member->root; SourceValue receiver;
    XrXirInstruction read = root->kind == SOURCE_LOCAL ?
        (XrXirInstruction){XR_XIR_CELL_READ, root->type, {root->index}, {0}, 0, {0}} :
        (XrXirInstruction){XR_XIR_SLOT_LOAD, root->type, {0}, {0}, root->index, {0}};
    return emit(ctx, read, &receiver) && emit(ctx,
        (XrXirInstruction){XR_XIR_STRUCT_GET, member->type, {receiver.id}, {0}, member->field, {0}}, value);
}
static bool source_member_store(SourceContext *ctx, AstNode *node, const SourceMemberPlace *member, SourceValue value) {
    if (member->constructor) return source_constructor_store(ctx, node, member->field, value);
    return emit(ctx, (XrXirInstruction){XR_XIR_STRUCT_SET, XR_XIR_UNIT, {member->place.id, value.id}, {0}, member->field, {0}}, NULL);
}
static bool source_struct_set(SourceContext *ctx, AstNode *node, SourceValue *value) {
    MemberSetNode *set = &node->as.member_set;
    if (source_constructor_receiver(ctx, set->object)) return source_constructor_field(ctx, node, set->member, value, set->value);
    SourceMemberPlace member = {0};
    if (!source_member_place(ctx, node, set->object, set->member, 1, &member) ||
        !expression_in(ctx, set->value, member.type, value)) return false;
    if (value->type != member.type) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "field assignment type mismatch");
    return source_member_store(ctx, node, &member, *value);
}
static bool source_member_compound(SourceContext *ctx, AstNode *node, AstNodeType operation, SourceValue *value) {
    CompoundAssignmentNode *assignment = &node->as.compound_assignment;
    SourceMemberPlace member = {0}; SourceValue left, right;
    bool shift = operation == AST_BINARY_LSHIFT || operation == AST_BINARY_RSHIFT;
    if (!source_member_place(ctx, node, assignment->object, assignment->name, 3, &member) ||
        !source_member_read(ctx, node, &member, &left) ||
        !expression_in(ctx, assignment->value, shift ? XR_XIR_UNIT : member.type, &right) ||
        !source_binary(ctx, node, operation, left, right, value)) return false;
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
    XrXirInstruction read = symbol->kind == SOURCE_LOCAL ?
        (XrXirInstruction) {XR_XIR_CELL_READ, symbol->type, {symbol->index, 0}, {0}, 0, {0}} :
        (XrXirInstruction) {XR_XIR_SLOT_LOAD, symbol->type, {0}, {0}, symbol->index, {0}};
    bool shift = operation == AST_BINARY_LSHIFT || operation == AST_BINARY_RSHIFT;
    if (!emit(ctx, read, &left) || !expression_in(ctx, assignment->value, shift ? XR_XIR_UNIT : symbol->type, &right) ||
        !source_binary(ctx, node, operation, left, right, value)) return false;
    if (value->type != symbol->type) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "compound assignment cannot narrow its result");
    XrXirInstruction write = symbol->kind == SOURCE_LOCAL ?
        (XrXirInstruction) {XR_XIR_CELL_WRITE, XR_XIR_UNIT, {symbol->index, value->id}, {0}, 0, {0}} :
        (XrXirInstruction) {XR_XIR_SLOT_STORE, XR_XIR_UNIT, {value->id, 0}, {0}, symbol->index, {0}};
    return emit(ctx, write, NULL);
}
