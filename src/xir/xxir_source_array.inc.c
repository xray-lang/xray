/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_array.inc.c - Value Array expressions and logical root places
 *
 * KEY CONCEPT:
 *   Mutation binds the root before operands and reads its current value at commit.
 */
static const XrNativeMemberDeclaration *source_array_member(SourceContext *ctx,
    AstNode *node, const char *name) {
    if (!source_native_array_declaration(ctx)) return NULL;
    const XrNativeTypeDeclaration *array = xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ARRAY);
    const XrNativeMemberDeclaration *member = source_native_member(ctx,array,name);
    if (!source_work(ctx, node)) return NULL;
    if (!member || !member->is_public || member->operation == XR_NATIVE_OPERATION_NONE) {
        source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array member has no admitted value-operation contract");
        return NULL;
    }
    return member;
}
static XrXirSourceType source_array_schema_type(SourceContext *ctx, XrNativeTypeTerm term) {
    if (term == XR_NATIVE_TERM_ELEMENT)
        return (XrXirSourceType) {(XrXirType) XR_XIR_TYPE_PARAMETER_BASE, ctx->array_declaration, true};
    if (term == XR_NATIVE_TERM_ARRAY_ELEMENT) {
        /* Declaration-only open nodes are projected after executable checking. */
        return (XrXirSourceType) {0};
    }
    XrXirType scalar = term == XR_NATIVE_TERM_I64 ? XR_XIR_I64 : term == XR_NATIVE_TERM_STRING ? XR_XIR_STRING :
        term == XR_NATIVE_TERM_BOOL ? XR_XIR_BOOL : XR_XIR_UNIT;
    return (XrXirSourceType) {scalar, 0, term == XR_NATIVE_TERM_I64 || term == XR_NATIVE_TERM_STRING ||
        term == XR_NATIVE_TERM_BOOL || term == XR_NATIVE_TERM_UNIT};
}
static bool source_array_member_reference(SourceContext *ctx, AstNode *node,
    const XrNativeMemberDeclaration *member) {
    uint32_t operation = (uint32_t) member->operation;
    if (!operation || operation >= sizeof(ctx->array_members) / sizeof(ctx->array_members[0]))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array member identity is not executable");
    if (!ctx->array_members[operation]) {
        SourceName symbol = {0}; symbol.name = source_owned_text(ctx, member->name);
        if (!symbol.name) return false;
        XrXirSourceRange range = {ctx->array_module, (int) member->line, (int) member->column,
            (int) member->line, (int) (member->column + source_text_size(ctx, member->name))};
        if (!source_query_declare(ctx, &symbol, XR_XIR_SOURCE_MEMBER, ctx->array_declaration, range)) return false;
        ctx->array_members[operation] = symbol.declaration;
        XrXirSourceType *parameters = member->parameter_count ? source_alloc(ctx, member->parameter_count, sizeof(*parameters)) : NULL;
        if (member->parameter_count && !parameters) return false;
        for (uint32_t p = 0; p < member->parameter_count; ++p) {
            if (!source_work(ctx, node)) return false;
            parameters[p] = source_array_schema_type(ctx, member->parameters[p].type);
        }
        XrXirSourceDeclaration *record = (XrXirSourceDeclaration *) &ctx->query.declarations[symbol.declaration - 1];
        record->native_identity = member->id;
        record->signature = source_owned_text(ctx, member->signature);
        if (!record->signature) return false;
        record->type = source_array_schema_type(ctx, member->result);
        record->parameters = parameters; record->parameter_count = member->parameter_count;
        record->exported = true; record->mutable = member->receiver == XR_NATIVE_RECEIVER_REF;
        record->generic_parent = ctx->array_declaration; record->generic_parent_count = 1;
        for(uint32_t p=0;p<member->parameter_count;++p) {
            if(!source_work(ctx,node))return false;
            SourceName parameter={0};parameter.name=source_owned_text(ctx,member->parameters[p].name);
            if(!parameter.name||!source_query_declare(ctx,&parameter,XR_XIR_SOURCE_PARAMETER,symbol.declaration,
                (XrXirSourceRange){ctx->array_module,0,0,0,0}))return false;
            ((XrXirSourceDeclaration *)ctx->query.declarations)[parameter.declaration-1].type=parameters[p];
        }
    }
    return source_query_target_token_reference(ctx,node,
        ctx->array_members[operation], member->is_method ? XR_XIR_SOURCE_CALL : XR_XIR_SOURCE_READ);
}
#include "xxir_source_native_calls.inc.c"
static bool source_array_static_receiver(SourceContext *ctx, AstNode *node) {
    return node && node->type == AST_NEW_EXPR && node->as.new_expr.is_type_namespace &&
        node->as.new_expr.class_name && source_text_same(ctx, NULL, node->as.new_expr.class_name, "Array") &&
        !source_native_type_shadowed(ctx, "Array");
}
static bool source_array_static_type(SourceContext *ctx, AstNode *node, XrXirType *type) {
    NewExprNode *space = &node->as.new_expr;
    if (space->module_name || space->arg_count || space->type_arg_count != 1 || !space->type_args)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array static namespace requires one element type");
    XrXirType element = XR_XIR_UNIT;
    return source_type(ctx, space->type_args[0], &element) && source_array_element_type(ctx, element, type) &&
        source_native_array_declaration(ctx) && source_query_target_token_reference(ctx,node, ctx->array_declaration, XR_XIR_SOURCE_TYPE_USE);
}
static bool source_array_static_call(SourceContext *ctx, AstNode *node, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    MemberAccessNode *access = &call->callee->as.member_access;
    const XrNativeMemberDeclaration *member = source_array_member(ctx, call->callee, access->name);
    if (!member || member->operation != XR_NATIVE_OPERATION_ARRAY_WITH_CAPACITY ||
        !member->is_static || !member->is_method || call->type_arg_count || call->default_arg_count ||
        member->parameter_count!=1 || member->parameters[0].type!=XR_NATIVE_TERM_I64)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array static member requires its declared ordinary arguments");
    XrXirType type = XR_XIR_UNIT,parameter=XR_XIR_I64; SourceValue capacity;uint32_t supplied;
    if (!source_array_static_type(ctx, access->object, &type) ||
        !source_native_read_arguments(ctx,node,&parameter,1,1,&capacity,&supplied,
            "Array static member requires its declared ordinary arguments")) return false;
    if (capacity.type != XR_XIR_I64)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array capacity must be exact i64");
    return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_WITH_CAPACITY, type,
        {capacity.id, 0}, {0}, 0, {0}}, value) && source_array_member_reference(ctx, node, member) &&
        source_array_call_signature(ctx,node,&(SourceNativeCallShape){type,&capacity.type,1},value->type);
}
static SourceName *source_array_root(SourceContext *ctx, AstNode *node) {
    if (!node || node->type != AST_VARIABLE) return NULL;
    SourceName *root = visible_name(ctx, node->as.variable.name);
    return root && (root->kind == SOURCE_LOCAL || root->kind == SOURCE_SLOT) &&
        xr_xir_type_is_array(&ctx->types, root->type) ? root : NULL;
}
static bool source_array_read_place(SourceContext *ctx, AstNode *node, SourceName *root, SourceValue *place) {
    if (!root || (root->kind == SOURCE_LOCAL && !root->mutable))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array read requires a stored root");
    if (!source_query_reference(ctx, node, root, root, XR_XIR_SOURCE_READ)) return false;
    if(root->kind==SOURCE_SLOT && root->module!=ctx->bodies[ctx->function].module && ctx->history)
        return source_history_binding_read(ctx,root,place);
    if (!root->mutable) {
        return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_SLOT_PLACE, root->type, {0}, {0}, root->index, {0}}, place);
    }
    SourceValue cell = {0};
    return source_binding_cell(ctx, root, &cell) &&
        source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_PLACE, root->type, {cell.id, 0}, {0}, 0, {0}}, place);
}
static bool source_array_plain_index(SourceContext *ctx, AstNode *index) {
    if (!index) return false;
    if (index->type == AST_LITERAL_INT) return true;
    if (index->type == AST_UNARY_NEG && index->as.unary.operand &&
        index->as.unary.operand->type == AST_LITERAL_INT) return true;
    if (index->type != AST_VARIABLE) return false;
    SourceName *name = visible_name(ctx, index->as.variable.name);
    /* A module slot can still be uninitialized in a call made by its initializer. */
    return name && name->kind == SOURCE_LOCAL && name->type == XR_XIR_I64;
}
static bool source_array_receiver(SourceContext *ctx, AstNode *node, AstNode *index,
    bool writable, SourceValue *value) {
    SourceName *root = source_array_root(ctx, node);
    if (writable) return source_value_place(ctx,node,value);
    if (root && (root->mutable || root->kind == SOURCE_SLOT) &&
        (!index || source_array_plain_index(ctx, index)))
        return source_array_read_place(ctx, node, root, value);
    return expression(ctx, node, value);
}
static bool source_array_spread_literal(SourceContext *ctx, AstNode *node,
    SourceExpectedType expected, SourceValue *value);
static bool source_array_literal(SourceContext *ctx, AstNode *node, SourceExpectedType expected, SourceValue *value) {
    ArrayLiteralNode *literal = &node->as.array_literal;
    if (literal->is_repeat || literal->count < 0 ||
        (literal->count && !literal->elements))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array literal needs an admitted explicit element list");
    if (expected.present && !xr_xir_type_is_array(&ctx->types, expected.type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array literal cannot satisfy a non-Array context");
    for (int i = 0; i < literal->count; ++i) {
        if (!source_work(ctx,literal->elements[i])) return false;
        if (literal->elements[i] && literal->elements[i]->type == AST_SPREAD_EXPR)
            return source_array_spread_literal(ctx,node,expected,value);
    }
    SourceExpectedType element = {expected.present,XR_XIR_UNIT, false, expected.inferred};
    if (expected.present) element.type = xr_xir_array_element(&ctx->types,expected.type);
    if (!literal->count && !element.present)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "empty Array literal needs an element context");
    SourceValue *elements = literal->count ? source_alloc(ctx, (size_t) literal->count, sizeof(*elements)) : NULL;
    if (literal->count && !elements) return false;
    for (int i = 0; i < literal->count; ++i) {
        if (!source_plan_expression(ctx, literal->elements[i], element, &elements[i])) return false;
        if (!i && !element.present) element = (SourceExpectedType){true,elements[i].type, false, true};
        if ((!expected.present || expected.inferred) && !source_root_join_record(ctx,&elements[i])) return false;
        if (elements[i].type != element.type)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array literal elements require one exact type");
    }
    XrXirType array = XR_XIR_UNIT;
    if (!source_array_element_type(ctx, element.type, &array) || !source_native_array_declaration(ctx)) return false;
    if (!source_recipe_group(ctx, (XrXirInstruction) {XR_XIR_ARRAY_NEW, array, {0}, {0}, 0, {0}},
        elements, (uint32_t) literal->count, value)) return false;
    return source_query_target_token_reference(ctx,node,
        ctx->array_declaration, XR_XIR_SOURCE_TYPE_USE);
}
static bool source_default_value(SourceContext *ctx, AstNode *node, XrXirType type, SourceValue *value);
/* `Array<T>()` is empty, `Array<T>(n)` and `Array<T>(n, fill)` hold n copies of the fill or of the
 * element default, and `Array<T>([...])` is the literal. Only governed Array declarations use these recipes. */
static bool source_array_construct(SourceContext *ctx, AstNode *node, SourceValue *value) {
    NewExprNode *call = &node->as.new_expr;
    if (call->is_type_namespace || call->module_name || !call->class_name ||
        !source_text_same(ctx, NULL, call->class_name, "Array") || source_native_type_shadowed(ctx, "Array"))
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "built-in construction is not implemented in XIR");
    if (call->type_arg_count != 1 || !call->type_args || call->arg_count < 0 || call->arg_count > 2 ||
        (call->arg_count && !call->arguments))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array construction takes one element type and at most a length and a fill");
    for (int a = 0; a < call->arg_count; ++a)
        if (call->arg_accesses && call->arg_accesses[a] != XR_CALL_ARG_PLAIN)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array construction arguments are values");
    XrXirType element, array;
    if (!source_type(ctx, call->type_args[0], &element) || !source_array_element_type(ctx, element, &array) ||
        !source_native_array_declaration(ctx)) return false;
    if (call->arg_count == 1 && call->arguments[0]->type == AST_ARRAY_LITERAL) {
        if (!source_array_literal(ctx, call->arguments[0], (SourceExpectedType){true, array, false, false}, value)) return false;
        if (value->type != array) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array literal does not match the constructed element type");
        return true;
    }
    bool ok;
    if (!call->arg_count) ok = source_recipe_group(ctx, (XrXirInstruction) {XR_XIR_ARRAY_NEW, array, {0}, {0}, 0, {0}}, NULL, 0, value);
    else {
        SourceValue length, fill;
        if (!source_plan_expression(ctx, call->arguments[0], (SourceExpectedType){true, XR_XIR_I64, false, false}, &length)) return false;
        if (length.type != XR_XIR_I64) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array length must be i64");
        if (call->arg_count == 2) {
            if (!source_plan_expression(ctx, call->arguments[1], (SourceExpectedType){true, element, false, false}, &fill)) return false;
            if (fill.type != element) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array fill must have the element type");
        } else if (!source_default_value(ctx, node, element, &fill)) return false;
        ok = source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_REPEAT, array, {length.id, fill.id}, {0}, 0, {0}}, value);
    }
    return ok && source_query_target_token_reference(ctx,node,
        ctx->array_declaration, XR_XIR_SOURCE_TYPE_USE);
}
static bool source_array_get(SourceContext *ctx, AstNode *node, AstNode *receiver, AstNode *index,
    const SourceValue *evaluated, const XrNativeMemberDeclaration *member, SourceValue *value) {
    SourceValue array = {0}, at;
    if (evaluated) array = *evaluated;
    else if (!source_array_receiver(ctx, receiver, index, false, &array)) return false;
    if (!xr_xir_type_is_array(&ctx->types, array.type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "indexed receiver is not an Array");
    if (!source_plan_expression(ctx, index, (SourceExpectedType){XR_XIR_I64 != XR_XIR_UNIT,XR_XIR_I64, false, false}, &at)) return false;
    if (!member) member = source_array_member(ctx, node, "get");
    if (!member || member->operation != XR_NATIVE_OPERATION_ARRAY_GET) return false;
    return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_GET, xr_xir_array_element(&ctx->types, array.type),
        {array.id, at.id}, {0}, 0, {0}}, value) && source_array_member_reference(ctx, node, member) &&
        source_array_call_signature(ctx,node,&(SourceNativeCallShape){array.type,&at.type,1},value->type);
}
static bool source_array_set_values(SourceContext *ctx, AstNode *node, SourceValue *args,
    const XrNativeMemberDeclaration *member, bool assignment, SourceValue *value) {
    if (!member) member = source_array_member(ctx, node, "set");
    if (!member || member->operation != XR_NATIVE_OPERATION_ARRAY_SET) return false;
    if (!source_recipe_group(ctx, (XrXirInstruction) {XR_XIR_ARRAY_SET, XR_XIR_UNIT, {0}, {0}, 0, {0}}, args, 3, value) ||
        !source_array_member_reference(ctx, node, member)||
        !source_array_call_signature(ctx,node,&(SourceNativeCallShape){args[0].type,
            (XrXirType[]){args[1].type,args[2].type},2},XR_XIR_UNIT)) return false;
    if (assignment) *value = args[2];
    return true;
}
static bool source_array_call(SourceContext *ctx, AstNode *node, const SourceValue *evaluated, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    MemberAccessNode *access = &call->callee->as.member_access;
    const XrNativeMemberDeclaration *member = source_array_member(ctx, call->callee, access->name);
    if (!member) return false;
    if (member->is_static || !member->is_method)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array call requires an instance method");
    SourceArrayRecipe recipe = source_array_recipe(member->operation);
    if (recipe != SOURCE_ARRAY_NONE)
        return source_array_member_reference(ctx, node, member) &&
            source_array_recipe_call(ctx, node, recipe, evaluated, value);
    if (call->type_arg_count || call->default_arg_count || call->arg_count<0 ||
        (call->arg_count && !call->arguments))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array method requires its declared arguments");
    switch (member->operation) {
    case XR_NATIVE_OPERATION_ARRAY_RESERVE: {
        SourceValue root, capacity, current, candidate;XrXirType parameter=XR_XIR_I64;uint32_t supplied;
        if (!source_value_place(ctx, access->object, &root) ||
            !source_native_read_arguments(ctx,node,&parameter,1,1,&capacity,&supplied,
                "Array method requires its declared arguments")) return false;
        if (!xr_xir_type_is_array(&ctx->types, root.type) || capacity.type != XR_XIR_I64)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array reserve requires an Array place and exact i64");
        /* Arguments may rebind the root, so read the current value only after them. */
        return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_PLACE_READ, root.type,
                {root.id, 0}, {0}, 0, {0}}, &current) &&
            source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_RESERVE, root.type,
                {current.id, capacity.id}, {0}, 0, {0}}, &candidate) &&
            source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_PLACE_WRITE, XR_XIR_UNIT,
                {root.id, candidate.id}, {0}, 0, {0}}, NULL) &&
            source_array_member_reference(ctx, node, member) && (*value = candidate, true) &&
            source_array_call_signature(ctx,node,&(SourceNativeCallShape){root.type,&capacity.type,1},value->type);
    }
    case XR_NATIVE_OPERATION_ARRAY_GET: case XR_NATIVE_OPERATION_ARRAY_SET: {
        bool setting=member->operation==XR_NATIVE_OPERATION_ARRAY_SET;
        SourceValue args[3];uint32_t supplied;
        if (setting) {
            if (!source_array_receiver(ctx,access->object,NULL,true,&args[0])) return false;
        } else if (evaluated) args[0]=*evaluated;
        else if (call->arg_count==1 && call->arguments[0] && call->arguments[0]->type!=AST_SPREAD_EXPR) {
            if (!source_array_receiver(ctx,access->object,call->arguments[0],false,&args[0])) return false;
        } else if (!expression(ctx,access->object,&args[0])) return false;
        if (!xr_xir_type_is_array(&ctx->types,args[0].type))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Array method receiver is not an Array");
        XrXirType parameters[]={XR_XIR_I64,xr_xir_array_element(&ctx->types,args[0].type)};
        uint32_t count=setting ? 2u : 1u;
        if (member->parameter_count!=count || !source_native_read_arguments(ctx,node,parameters,count,count,
            &args[1],&supplied,"Array method requires its declared arguments")) return false;
        return (setting ? source_recipe_group(ctx,(XrXirInstruction){XR_XIR_ARRAY_SET,XR_XIR_UNIT,{0},{0},0,{0}},args,3,value) :
            source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_GET,parameters[1],{args[0].id,args[1].id},{0},0,{0}},value)) &&
            source_array_member_reference(ctx,node,member);
    }
    case XR_NATIVE_OPERATION_ARRAY_PUSH: {
        SourceValue place, input;uint32_t supplied;
        SourceName *root=source_array_root(ctx,access->object);
        if(root && root->kind==SOURCE_SLOT && root->module!=ctx->bodies[ctx->function].module && ctx->history) {
            if(!root->mutable)return source_fail(ctx,access->object,XR_XIR_BAD_TYPE,"push requires a mutable Array binding");
            SourceName saved=*root;XrXirType element=xr_xir_array_element(&ctx->types,saved.type);
            return source_query_reference(ctx,access->object,root,root,XR_XIR_SOURCE_READ_WRITE) &&
                source_native_read_arguments(ctx,node,&element,1,1,&input,&supplied,
                    "Array method requires its declared arguments") &&
                source_history_binding_push(ctx,&saved,input,value) && source_array_member_reference(ctx,node,member);
        }
        if (!source_array_receiver(ctx, access->object, NULL, true, &place)) return false;
        XrXirType parameter=xr_xir_array_element(&ctx->types,place.type);
        if (!source_native_read_arguments(ctx,node,&parameter,1,1,&input,&supplied,
            "Array method requires its declared arguments")) return false;
        return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_PUSH, XR_XIR_UNIT, {place.id, input.id}, {0}, 0, {0}}, value) &&
            source_array_member_reference(ctx, node, member) &&
            source_array_call_signature(ctx,node,&(SourceNativeCallShape){place.type,&input.type,1},value->type);
    }
    default: return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array member operation is not implemented");
    }
}
static bool source_length(SourceContext *ctx, AstNode *node,
    const XrCoreIntrinsicDesc *descriptor, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    if (!descriptor || descriptor->id != XR_CORE_BUILTIN_LEN || !xr_core_intrinsic_descriptor_validate(descriptor) ||
        call->arg_count != 1 || call->type_arg_count || call->default_arg_count ||
        (call->arg_accesses && call->arg_accesses[0] != XR_CALL_ARG_PLAIN))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "len requires one ordinary value operand");
    SourceValue array;
    if (!source_array_receiver(ctx, call->arguments[0], NULL, false, &array)) return false;
    uint32_t view_kind=0;XrXirType view_element=XR_XIR_UNIT;
    bool view=source_view_shape(ctx,array.type,&view_kind,&view_element);
    bool builder=source_stringbuilder_type(ctx,array.type),range=source_range_is(ctx,array.type);
    if(ctx->diagnostic.status!=XR_XIR_OK) return false;
    if (array.type != XR_XIR_STRING && !xr_xir_type_is_array(&ctx->types, array.type) &&
        !xr_xir_type_is_map(&ctx->types, array.type) && !xr_xir_type_is_set(&ctx->types, array.type) && !view && !builder && !range)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "len receiver family is not admitted");
    if (!ctx->length_declaration) {
        XrXirSourceQueryModule *modules = source_query_append(ctx, ctx->query.modules,
            &ctx->query.module_count, &ctx->query_module_capacity, sizeof(*modules));
        if (!modules) return false;
        ctx->query.modules = modules;
        /* Core identity has no source-position/content-hash claim in this snapshot. */
        modules[ctx->query.module_count - 1] = (XrXirSourceQueryModule) {"xray-core:prelude", NULL, {{0}}};
        SourceName symbol = {0}; symbol.name = source_owned_text(ctx, descriptor->source_name);
        if (!symbol.name) return false;
        if (!source_query_declare(ctx, &symbol, XR_XIR_SOURCE_INTRINSIC, 0,
            (XrXirSourceRange) {ctx->query.module_count - 1, 0, 0, 0, 0})) return false;
        ctx->length_declaration = symbol.declaration;
        XrXirSourceType *parameter = source_alloc(ctx, 1, sizeof(*parameter));
        if (!parameter) return false;
        XrXirSourceDeclaration *record = (XrXirSourceDeclaration *) &ctx->query.declarations[symbol.declaration - 1];
        record->native_identity = (uint32_t) descriptor->id; record->exported = true;
        record->type = (XrXirSourceType) {XR_XIR_I64, 0, true};
        record->parameters = parameter; record->parameter_count = 1;
    }
    return (range?source_range_length(ctx,node,array,value):builder?source_stringbuilder_length(ctx,node,array,value):source_recipe_record(ctx, (XrXirInstruction) {view ? XR_XIR_VIEW_LEN : array.type == XR_XIR_STRING ? XR_XIR_STRING_LEN : xr_xir_type_is_array(&ctx->types, array.type) ? XR_XIR_ARRAY_LEN : XR_XIR_HASH_LEN, XR_XIR_I64, {array.id, 0}, {0}, 0, {0}}, value)) &&
        source_query_target_token_reference(ctx,call->callee,
            ctx->length_declaration, XR_XIR_SOURCE_CALL);
}
