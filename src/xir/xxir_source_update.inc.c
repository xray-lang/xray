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
    XrHirBodyPlace typed;
    XrXirType type;
    uint32_t field;
    bool constructor;
} SourceMemberPlace;
static bool source_member_place(SourceContext *ctx, AstNode *node, AstNode *object,
    const char *name, unsigned access, SourceMemberPlace *member) {
    if (source_constructor_receiver(ctx, object)) {
        member->constructor = true;
        XrXirType receiver=visible_name(ctx,"this")->type;
        XrHirPlaceAction field;
        if(!source_struct_field(ctx,node,receiver,name,access,&member->field,&member->type)||
            !source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_CONSTRUCTOR,.value=receiver,
                .type=member->type,.field=member->field,.access=access},&field)||
            !source_hir_place_emitted(ctx,&field,NULL,ctx->bodies[ctx->function].count))return false;
        member->typed=field.place;member->type=field.type;return true;
    }
    XrXirType receiver_type;
    if (!source_path_type(ctx,object,&receiver_type)) return false;
    XrHirPlaceAction route;
    if(!source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_ROUTE,.type=receiver_type},&route)||
        !source_hir_place_emitted(ctx,&route,NULL,ctx->bodies[ctx->function].count))return false;
    if(route.object_root) {
        if (!expression(ctx,object,&member->place)) return false;
        XrHirPlaceAction identity,field;
        if(!source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_TEMPORARY,
            .type=member->place.type,.base=member->place.id},&identity)||
            !source_hir_place_emitted(ctx,&identity,&member->place,ctx->bodies[ctx->function].count)||
            !source_struct_field(ctx,node,member->place.type,name,access,&member->field,&member->type)||
            !source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_MEMBER,.parent=identity.place,
                .type=member->type,.field=member->field,.access=access},&field)||
            !source_hir_place_emitted(ctx,&field,NULL,ctx->bodies[ctx->function].count))return false;
        member->typed=field.place;member->type=field.type;return true;
    }
    SourceName *root = object->type == AST_VARIABLE ? visible_name(ctx, object->as.variable.name) : NULL;
    if (root && root->type == XR_XIR_PANIC_INFO && (root->kind == SOURCE_LOCAL || root->kind == SOURCE_SLOT))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "PanicInfo field writes require class support");
    if (!source_value_place(ctx,object,&member->place) ||
        !source_struct_field(ctx,node,member->place.type,name,access,&member->field,&member->type)) return false;
    XrHirPlaceAction field;
    if(!source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_FIELD,.parent=member->place.place,
        .type=member->type,.field=member->field,.access=access},&field))return false;
    uint32_t before=ctx->bodies[ctx->function].count;
    if(!source_recipe_record(ctx,(XrXirInstruction){field.operation,field.type,{field.location},{0},field.immediate,{0}},&member->place)||
        !source_hir_place_emitted(ctx,&field,&member->place,before))return false;
    member->typed=field.place;member->type=field.type;return true;
}
static bool source_member_read(SourceContext *ctx,const SourceMemberPlace *member,SourceValue *value) {
    AstNode *node=ctx->active_expression?ctx->active_expression->syntax:NULL;
    XrHirPlaceAction action;
    if(!source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_READ,.parent=member->typed},&action))return false;
    uint32_t before=ctx->bodies[ctx->function].count;
    bool ok=member->constructor?source_constructor_read(ctx,action.field,value):
        source_recipe_record(ctx,(XrXirInstruction){action.operation,action.type,{action.location},{0},action.immediate,{0}},value);
    return ok&&source_hir_place_emitted(ctx,&action,value,before);
}
static bool source_member_store(SourceContext *ctx,const SourceMemberPlace *member,SourceValue value,const char *failure) {
    AstNode *node=ctx->active_expression?ctx->active_expression->syntax:NULL;
    XrHirPlaceAction action;
    if(!source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_WRITE,.parent=member->typed,
        .value=value.type,.failure=failure},&action))return false;
    uint32_t before=ctx->bodies[ctx->function].count;
    bool ok=member->constructor?source_constructor_store(ctx,action.field,value):
        source_recipe_record(ctx,(XrXirInstruction){action.operation,action.type,{action.location,value.id},{0},action.immediate,{0}},NULL);
    return ok&&source_hir_place_emitted(ctx,&action,NULL,before);
}
static bool source_struct_set(SourceContext *ctx, AstNode *node, SourceValue *value) {
    MemberSetNode *set = &node->as.member_set;
    if (source_constructor_receiver(ctx, set->object)) return source_constructor_field(ctx, node, set->member, value, set->value);
    SourceMemberPlace member = {0};
    if (!source_member_place(ctx, node, set->object, set->member, 1, &member) ||
        !source_plan_expression(ctx, set->value, (SourceExpectedType){member.type != XR_XIR_UNIT,member.type, false, false}, value)) return false;
    return source_member_store(ctx,&member,*value,"field assignment type mismatch");
}
static bool source_member_compound(SourceContext *ctx, AstNode *node, AstNodeType operation, SourceValue *value) {
    CompoundAssignmentNode *assignment = &node->as.compound_assignment;
    SourceMemberPlace member = {0}; SourceValue left, right;
    bool shift = operation == AST_BINARY_LSHIFT || operation == AST_BINARY_RSHIFT;
    if (!source_member_place(ctx, node, assignment->object, assignment->name, 3, &member) ||
        !source_member_read(ctx, &member, &left) ||
        !source_plan_expression(ctx, assignment->value, (SourceExpectedType){shift ? XR_XIR_UNIT : member.type != XR_XIR_UNIT,shift ? XR_XIR_UNIT : member.type, false, false}, &right) ||
        !source_binary_apply(ctx, node, operation, left, right, value)) return false;
    return source_member_store(ctx,&member,*value,"compound assignment cannot narrow its result");
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
    if(!symbol)return source_fail(ctx,node,XR_XIR_BAD_TYPE,"compound assignment requires a mutable binding");
    XrHirBindingAction qualification;
    if(!source_hir_binding_use(ctx,node,symbol,XR_HIR_BINDING_COMPOUND,(SourceValue){0},&qualification)||
        !source_hir_binding_emitted(ctx,&qualification,NULL,ctx->bodies[ctx->function].count))return false;
    if (!source_query_reference(ctx, node, symbol, symbol, XR_XIR_SOURCE_READ_WRITE)) return false;
    SourceValue left, right;

    bool shift = operation == AST_BINARY_LSHIFT || operation == AST_BINARY_RSHIFT;
    if (!source_binding_read(ctx, symbol, &left) || !source_plan_expression(ctx, assignment->value, (SourceExpectedType){shift ? XR_XIR_UNIT : symbol->type != XR_XIR_UNIT,shift ? XR_XIR_UNIT : symbol->type, false, false}, &right) ||
        !source_binary_apply(ctx, node, operation, left, right, value)) return false;
    return source_binding_write_reason(ctx,node,symbol,*value,"compound assignment cannot narrow its result");
}
/* Only scalar/string reads and literal template parts prove that the RHS
 * cannot rebind the receiver. Calls and member getters keep ordinary ordering. */
static bool source_string_append_rhs(SourceContext *ctx, AstNode *node, unsigned depth) {
    if (!node || depth >= 64) return false;
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    if (node->type == AST_LITERAL_STRING) return true;
    if (node->type == AST_LITERAL_INT || node->type == AST_LITERAL_FLOAT ||
        node->type == AST_LITERAL_TRUE || node->type == AST_LITERAL_FALSE || node->type == AST_LITERAL_RUNE)
        return depth != 0;
    if (node->type == AST_VARIABLE) {
        SourceName *symbol = visible_name(ctx, node->as.variable.name);
        return symbol && (symbol->type == XR_XIR_STRING || (depth && (symbol->type == XR_XIR_BOOL ||
            symbol->type == XR_XIR_RUNE || xr_xir_type_is_number(symbol->type))));
    }
    if (node->type != AST_TEMPLATE_STRING || node->as.template_str.part_count < 0) return false;
    for (int part = 0; part < node->as.template_str.part_count; ++part)
        if (!source_string_append_rhs(ctx, node->as.template_str.parts[part], depth + 1)) return false;
    return true;
}
/* One scalar interpolation needs no temporary String. Multiple nonempty parts
 * still finish their complete RHS before the receiver is published once. */
static AstNode *source_string_append_scalar(SourceContext *ctx, AstNode *node) {
    if (node->type != AST_TEMPLATE_STRING) return NULL;
    AstNode *scalar = NULL;
    for (int i = 0; i < node->as.template_str.part_count; ++i) {
        AstNode *part = node->as.template_str.parts[i];
        if (part->type == AST_LITERAL_STRING && !part->as.literal.string_length) continue;
        if (scalar) return NULL;
        scalar = part;
    }
    if (!scalar) return NULL;
    if (scalar->type == AST_VARIABLE) {
        SourceName *symbol = visible_name(ctx, scalar->as.variable.name);
        return symbol && (symbol->type == XR_XIR_BOOL || symbol->type == XR_XIR_RUNE ||
            xr_xir_type_is_number(symbol->type)) ? scalar : NULL;
    }
    return scalar->type == AST_LITERAL_INT || scalar->type == AST_LITERAL_FLOAT ||
        scalar->type == AST_LITERAL_TRUE || scalar->type == AST_LITERAL_FALSE ||
        scalar->type == AST_LITERAL_RUNE ? scalar : NULL;
}
static bool source_string_append_scalar_query(SourceContext *ctx, AstNode *node) {
    for (int i = 0; i < node->as.template_str.part_count; ++i) {
        AstNode *part = node->as.template_str.parts[i];
        if (part->type == AST_LITERAL_STRING && !part->as.literal.string_length &&
            !source_query_expression(ctx, part, XR_XIR_STRING)) return false;
    }
    return source_query_expression(ctx, node, XR_XIR_STRING);
}
static bool source_string_append_statement(SourceContext *ctx, AstNode *node, bool *handled) {
    *handled = false;
    if (!node || node->type != AST_COMPOUND_ASSIGNMENT) return true;
    CompoundAssignmentNode *assignment = &node->as.compound_assignment;
    if (assignment->object || assignment->op != TK_PLUS_ASSIGN || !assignment->name) return true;
    SourceName *symbol = visible_name(ctx, assignment->name);
    if (!symbol || !symbol->mutable || symbol->type != XR_XIR_STRING || symbol->construction ||
        (symbol->kind != SOURCE_LOCAL && symbol->kind != SOURCE_SLOT) ||
        (symbol->kind == SOURCE_LOCAL && symbol->index < ctx->functions[ctx->function].parameter_count)) return true;
    if (!source_string_append_rhs(ctx, assignment->value, 0)) return ctx->diagnostic.status == XR_XIR_OK;
    *handled = true;
    SourceValue cell = {0}, suffix = {0};
    if (!source_query_reference(ctx, node, symbol, symbol, XR_XIR_SOURCE_READ_WRITE) ||
        !source_binding_cell(ctx, symbol, &cell)) return false;
    AstNode *scalar = source_string_append_scalar(ctx, assignment->value);
    if (scalar) {
        if (!expression(ctx, scalar, &suffix)) return false;
        if (suffix.type == XR_XIR_BOOL || suffix.type == XR_XIR_RUNE || xr_xir_type_is_number(suffix.type))
            return source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CELL_STRING_APPEND, XR_XIR_UNIT,
                {cell.id, suffix.id}, {0}, 1, {0}}, NULL) &&
                source_string_append_scalar_query(ctx, assignment->value) && source_query_expression(ctx, node, XR_XIR_STRING);
    }
    return source_plan_expression(ctx, assignment->value, (SourceExpectedType){true, XR_XIR_STRING, false, false}, &suffix) &&
        source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CELL_STRING_APPEND, XR_XIR_UNIT,
            {cell.id, suffix.id}, {0}, 0, {0}}, NULL) && source_query_expression(ctx, node, XR_XIR_STRING);
}
