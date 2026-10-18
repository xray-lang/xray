/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_hash.inc.c - Governed hash construction, queries and mutations
 */
static const XrNativeTypeDeclaration *source_hash_native(SourceContext *ctx, XrXirType type) {
    if (!source_work(ctx, NULL)) return NULL;
    return xr_native_declaration_by_id(xr_xir_type_is_map(&ctx->types, type) ?
        XR_NATIVE_DECLARATION_MAP : xr_xir_type_is_set(&ctx->types, type) ? XR_NATIVE_DECLARATION_SET : 0);
}
static bool source_hash_projection_type(SourceContext *ctx, XrXirType key, XrXirType element,
    bool entries, XrXirType *output) {
    if (entries) {
        XrXirCallableParameter *fields = source_alloc(ctx, 2, sizeof(*fields));
        if (!fields) return false;
        fields[0] = (XrXirCallableParameter){key, 0}; fields[1] = (XrXirCallableParameter){element, 0};
        if (!source_intern_type(ctx, (XrXirTypeNode){.kind = XR_XIR_TYPE_TUPLE,
            .parameters = fields, .parameter_count = 2}, &element)) return false;
    }
    return source_intern_type(ctx, (XrXirTypeNode){.kind = XR_XIR_TYPE_ARRAY, .element = element}, output);
}
#include "xxir_source_hash_query.inc.c"
static XrXirSourceType source_hash_schema(SourceContext *ctx, XrNativeTypeTerm term,
    uint32_t parent, SourceHashQueryTypes *query) {
    if (term == XR_NATIVE_TERM_KEY || term == XR_NATIVE_TERM_ELEMENT || term == XR_NATIVE_TERM_VALUE)
        return (XrXirSourceType){(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE + (term == XR_NATIVE_TERM_VALUE)), parent, true};
    if (term == XR_NATIVE_TERM_CALLBACK_KEY_VALUE_VISIT || term == XR_NATIVE_TERM_CALLBACK_ELEMENT_VISIT) {
        if (!query) return (XrXirSourceType){XR_XIR_UNIT, parent, false};
        uint32_t count = term == XR_NATIVE_TERM_CALLBACK_KEY_VALUE_VISIT ? 2 : 1;
        XrXirCallableParameter *parameters = source_alloc(ctx, count, sizeof(*parameters));
        if (!parameters) return (XrXirSourceType){0};
        for (uint32_t i = 0; i < count; ++i)
            parameters[i] = (XrXirCallableParameter){(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE + i), XR_PARAM_READ};
        XrXirType signature = XR_XIR_UNIT;
        if (!source_hash_query_intern(ctx,query,(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,
            .parameters=parameters,.parameter_count=count,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED},&signature)) return (XrXirSourceType){0};
        return (XrXirSourceType){signature, parent, true};
    }
    if (term == XR_NATIVE_TERM_SET_ELEMENT) {
        if (!query) return (XrXirSourceType){XR_XIR_UNIT, parent, false};
        XrXirType set = XR_XIR_UNIT;
        if (!source_hash_query_intern(ctx,query, (XrXirTypeNode){.kind = XR_XIR_TYPE_SET,
            .element = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE}, &set)) return (XrXirSourceType){0};
        return (XrXirSourceType){set, parent, true};
    }
    if (term == XR_NATIVE_TERM_NULLABLE_VALUE) {
        if (!query) return (XrXirSourceType){XR_XIR_UNIT, parent, false};
        XrXirType optional = XR_XIR_UNIT;
        if (!source_hash_query_intern(ctx,query, (XrXirTypeNode){.kind = XR_XIR_TYPE_NULLABLE,
            .element = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE + 1)}, &optional)) return (XrXirSourceType){0};
        return (XrXirSourceType){optional, parent, true};
    }
    if (term == XR_NATIVE_TERM_ARRAY_KEY || term == XR_NATIVE_TERM_ARRAY_VALUE ||
        term == XR_NATIVE_TERM_ARRAY_ELEMENT || term == XR_NATIVE_TERM_ARRAY_KEY_VALUE) {
        if (!query) return (XrXirSourceType){XR_XIR_UNIT, parent, false};
        XrXirType key = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
        XrXirType element = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE +
            (term == XR_NATIVE_TERM_ARRAY_VALUE || term == XR_NATIVE_TERM_ARRAY_KEY_VALUE));
        XrXirType array = XR_XIR_UNIT;
        if (!source_hash_query_projection(ctx,query,key,element,term == XR_NATIVE_TERM_ARRAY_KEY_VALUE,&array))
            return (XrXirSourceType){0};
        return (XrXirSourceType){array, parent, true};
    }
    return (XrXirSourceType){term == XR_NATIVE_TERM_BOOL ? XR_XIR_BOOL : XR_XIR_UNIT,
        0, term == XR_NATIVE_TERM_BOOL || term == XR_NATIVE_TERM_UNIT};
}
static bool source_hash_reference(SourceContext *ctx, AstNode *node,
    const XrNativeTypeDeclaration *native, const XrNativeMemberDeclaration *member) {
    uint32_t parent = 0, declaration = 0;
    if (!source_native_hash_declaration(ctx, native, &parent)) return false;
    for (uint32_t i = 0; i < ctx->query.declaration_count; ++i) {
        if (!source_work(ctx, node)) return false;
        const XrXirSourceDeclaration *record = (XrXirSourceDeclaration *)&ctx->query.declarations[i];
        if (record->kind == XR_XIR_SOURCE_MEMBER && record->parent == parent && record->native_identity == member->id) {
            declaration = record->id; break;
        }
    }
    if (!declaration) {
        uint32_t module = ctx->query.declarations[parent - 1].range.module;
        SourceName symbol = {0}; symbol.name = source_owned_text(ctx, member->name);
        if (!symbol.name || !source_query_declare(ctx, &symbol, XR_XIR_SOURCE_MEMBER, parent,
            (XrXirSourceRange){module, (int)member->line, (int)member->column,
                (int)member->line, (int)(member->column + source_text_size(ctx, member->name))})) return false;
        declaration = symbol.declaration;
        XrXirSourceType *parameters = member->parameter_count ? source_alloc(ctx, member->parameter_count, sizeof(*parameters)) : NULL;
        if (member->parameter_count && !parameters) return false;
        for (uint32_t p = 0; p < member->parameter_count; ++p) {
            if (!source_work(ctx, node)) return false;
            parameters[p] = source_hash_schema(ctx, member->parameters[p].type, parent, NULL);
        }
        const char *signature = source_owned_text(ctx, member->signature);
        if (!signature) return false;
        XrXirSourceDeclaration *record = (XrXirSourceDeclaration *)&ctx->query.declarations[declaration - 1];
        record->native_identity = member->id; record->signature = signature;
        record->parameters = parameters; record->parameter_count = member->parameter_count;
        record->type = source_hash_schema(ctx, member->result, parent, NULL);
        record->exported = true; record->mutable = member->receiver == XR_NATIVE_RECEIVER_REF;
        record->generic_parent = parent; record->generic_parent_count = native->parameter_count;
    }
    return source_query_target_token_reference(ctx, node, declaration, XR_XIR_SOURCE_CALL);
}
static bool source_hash_query_types(SourceContext *ctx) {
    SourceHashQueryTypes query={0};
    for (uint32_t i = 0; i < ctx->query.declaration_count; ++i) {
        if (!source_work(ctx, NULL)) return false;
        XrXirSourceDeclaration *record = (XrXirSourceDeclaration *)&ctx->query.declarations[i];
        if (record->kind != XR_XIR_SOURCE_MEMBER || !record->parent || record->parent > ctx->query.declaration_count) continue;
        const XrXirSourceDeclaration *parent = &ctx->query.declarations[record->parent - 1];
        if (parent->kind != XR_XIR_SOURCE_TYPE || (parent->native_identity != XR_NATIVE_DECLARATION_MAP &&
            parent->native_identity != XR_NATIVE_DECLARATION_SET)) continue;
        if (!query.types) {
            query.types=source_alloc(ctx,1,sizeof(*query.types));
            if (!query.types) return false;
            *query.types=ctx->query.types?*ctx->query.types:ctx->types;
            query.capacity=query.types->count;
        }
        const XrNativeTypeDeclaration *native = xr_native_declaration_by_id(parent->native_identity);
        if (!record->native_identity || record->native_identity > native->member_count) return false;
        record->type = source_hash_schema(ctx, native->members[record->native_identity - 1].result, record->parent, &query);
        if (!record->type.known) return false;
        XrXirSourceType *parameters = (XrXirSourceType *)record->parameters;
        for (uint32_t p = 0; p < record->parameter_count; ++p) {
            if (!source_work(ctx, NULL)) return false;
            parameters[p] = source_hash_schema(ctx, native->members[record->native_identity - 1].parameters[p].type, record->parent, &query);
            if (!parameters[p].known) return false;
        }
    }
    if (query.types) ctx->query.types=query.types;
    return true;
}
static bool source_hash_call(SourceContext *ctx, AstNode *node, const SourceValue *evaluated,
    XrXirType receiver_type, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    MemberAccessNode *access = &call->callee->as.member_access;
    const XrNativeTypeDeclaration *native = source_hash_native(ctx, receiver_type);
    const XrNativeMemberDeclaration *member = native ? source_native_member(ctx, native, access->name) : NULL;
    if (!member || member->operation == XR_NATIVE_OPERATION_NONE || !member->is_method || member->is_static || !member->is_public)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "hash member has no admitted operation contract");
    if (call->type_arg_count || call->default_arg_count || call->arg_count < 0 ||
        (uint32_t)call->arg_count != member->parameter_count)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "hash method requires its declared ordinary arguments");
    for (int i = 0; i < call->arg_count; ++i)
        if (call->arg_accesses && call->arg_accesses[i] != XR_CALL_ARG_PLAIN)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "hash method arguments are READ values");
    if (member->operation == XR_NATIVE_OPERATION_HASH_FOR_EACH)
        return source_hash_for_each(ctx, node, evaluated, receiver_type, value) &&
            source_hash_reference(ctx, call->callee, native, member);
    SourceValue operands[3] = {{0}};
    bool write = member->receiver == XR_NATIVE_RECEIVER_REF;
    if (evaluated && !write) operands[0] = *evaluated;
    else if (!source_array_receiver(ctx, access->object, call->arg_count ? call->arguments[0] : NULL, write, &operands[0])) return false;
    if (operands[0].type != receiver_type) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "hash receiver type changed during planning");
    const XrXirTypeNode *container = xr_xir_type_node(&ctx->types, receiver_type);
    /* Planning an argument can grow the type pool. Copy both identities first. */
    XrXirType key = container->element, element = container->result;
    bool binary_set = member->operation >= XR_NATIVE_OPERATION_SET_UNION &&
        member->operation <= XR_NATIVE_OPERATION_SET_IS_SUPERSET;
    for (int i = 0; i < call->arg_count; ++i) {
        XrXirType expected = binary_set ? receiver_type :
            member->operation == XR_NATIVE_OPERATION_MAP_CONTAINS_VALUE || i ? element : key;
        if (!source_plan_expression(ctx, call->arguments[i], (SourceExpectedType){true, expected, false, false}, &operands[i + 1])) return false;
        if (operands[i + 1].type != expected) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "hash argument requires its exact declared type");
    }
    XrXirOp op; XrXirType result = XR_XIR_UNIT;
    switch (member->operation) {
    case XR_NATIVE_OPERATION_MAP_CONTAINS_VALUE: op = XR_XIR_MAP_CONTAINS_VALUE; result = XR_XIR_BOOL; break;
    case XR_NATIVE_OPERATION_MAP_GET:
        op = XR_XIR_MAP_GET;
        if (!source_intern_type(ctx, (XrXirTypeNode){.kind = XR_XIR_TYPE_NULLABLE, .element = element}, &result)) return false;
        break;
    case XR_NATIVE_OPERATION_MAP_SET: op = XR_XIR_MAP_SET; break;
    case XR_NATIVE_OPERATION_SET_ADD: op = XR_XIR_SET_ADD; break;
    case XR_NATIVE_OPERATION_HASH_CONTAINS: op = XR_XIR_HASH_CONTAINS; result = XR_XIR_BOOL; break;
    case XR_NATIVE_OPERATION_HASH_DELETE: op = XR_XIR_HASH_DELETE; result = XR_XIR_BOOL; break;
    case XR_NATIVE_OPERATION_HASH_CLEAR: op = XR_XIR_HASH_CLEAR; break;
    case XR_NATIVE_OPERATION_HASH_KEYS:
        op = XR_XIR_HASH_KEYS;
        if (!source_hash_projection_type(ctx, key, key, false, &result)) return false;
        break;
    case XR_NATIVE_OPERATION_HASH_VALUES:
        op = XR_XIR_HASH_VALUES;
        if (!source_hash_projection_type(ctx, key,
            xr_xir_type_is_map(&ctx->types, receiver_type) ? element : key, false, &result)) return false;
        break;
    case XR_NATIVE_OPERATION_MAP_ENTRIES:
        op = XR_XIR_MAP_ENTRIES;
        if (!source_hash_projection_type(ctx, key, element, true, &result)) return false;
        break;
    case XR_NATIVE_OPERATION_SET_UNION: op = XR_XIR_SET_UNION; result = receiver_type; break;
    case XR_NATIVE_OPERATION_SET_INTERSECTION: op = XR_XIR_SET_INTERSECTION; result = receiver_type; break;
    case XR_NATIVE_OPERATION_SET_DIFFERENCE: op = XR_XIR_SET_DIFFERENCE; result = receiver_type; break;
    case XR_NATIVE_OPERATION_SET_SYMMETRIC_DIFFERENCE: op = XR_XIR_SET_SYMMETRIC_DIFFERENCE; result = receiver_type; break;
    case XR_NATIVE_OPERATION_SET_IS_SUBSET: op = XR_XIR_SET_IS_SUBSET; result = XR_XIR_BOOL; break;
    case XR_NATIVE_OPERATION_SET_IS_SUPERSET: op = XR_XIR_SET_IS_SUPERSET; result = XR_XIR_BOOL; break;
    default: return source_fail(ctx, node, XR_XIR_BAD_TYPE, "hash member operation is not executable");
    }
    bool ok = op == XR_XIR_MAP_SET ? source_recipe_group(ctx,
        (XrXirInstruction){op, result, {0}, {0}, 0, {0}}, operands, 3, value) :
        source_recipe_record(ctx, (XrXirInstruction){op, result, {operands[0].id, call->arg_count ? operands[1].id : 0}, {0}, 0, {0}}, value);
    return ok && source_hash_reference(ctx, call->callee, native, member);
}
static bool source_hash_literal(SourceContext *ctx, AstNode *node, SourceExpectedType expected, SourceValue *value) {
    bool map = node->type == AST_MAP_LITERAL;
    int count = map ? node->as.map_literal.count : node->as.set_literal.count;
    AstNode **keys = map ? node->as.map_literal.keys : node->as.set_literal.elements;
    AstNode **elements = map ? node->as.map_literal.values : NULL;
    if (count < 0 || count > (map ? 32768 : 65536) || (count && (!keys || (map && !elements))))
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "hash literal has no finite complete element list");
    if (expected.present && !(map ? xr_xir_type_is_map(&ctx->types, expected.type) : xr_xir_type_is_set(&ctx->types, expected.type)))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "hash literal requires its own container context");
    if (!count && !expected.present) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "empty hash literal requires a type context");
    XrXirType key = XR_XIR_UNIT, element = XR_XIR_UNIT;
    if (expected.present) {
        const XrXirTypeNode *type = xr_xir_type_node(&ctx->types, expected.type);
        key = type->element; element = type->result;
    }
    uint32_t width = map ? 2u : 1u;
    SourceValue *operands = count ? source_alloc(ctx, (size_t)count * width, sizeof(*operands)) : NULL;
    if (count && !operands) return false;
    for (int i = 0; i < count; ++i) {
        if (!source_work(ctx, node)) return false;
        SourceValue *entry = &operands[(uint32_t)i * width];
        if (!source_plan_expression(ctx, keys[i], (SourceExpectedType){expected.present || i != 0, key, false, expected.inferred}, entry)) return false;
        if (!i && !expected.present) key = entry->type;
        if (entry->type != key) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "hash keys require one exact type");
        if ((!expected.present || expected.inferred) && !source_root_join_record(ctx, entry)) return false;
        if (map) {
            if (!source_plan_expression(ctx, elements[i], (SourceExpectedType){expected.present || i != 0, element, false, expected.inferred}, entry + 1)) return false;
            if (!i && !expected.present) element = entry[1].type;
            if (entry[1].type != element) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "hash values require one exact type");
            if ((!expected.present || expected.inferred) && !source_root_join_record(ctx, entry + 1)) return false;
        }
    }
    if (key == XR_XIR_UNIT || xr_xir_type_is_cell(&ctx->types, key) ||
        (map && (element == XR_XIR_UNIT || xr_xir_type_is_cell(&ctx->types, element))))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "hash literal elements must be ordinary storable values");
    XrXirType type = XR_XIR_UNIT;
    if (!source_intern_type(ctx, (XrXirTypeNode){.kind = map ? XR_XIR_TYPE_MAP : XR_XIR_TYPE_SET,
        .element = key, .result = element}, &type)) return false;
    const XrNativeTypeDeclaration *native = source_hash_native(ctx, type); uint32_t declaration = 0;
    return native && source_native_hash_declaration(ctx, native, &declaration) &&
        source_recipe_group(ctx, (XrXirInstruction){map ? XR_XIR_MAP_NEW : XR_XIR_SET_NEW, type, {0}, {0}, 0, {0}},
            operands, (uint32_t)count * width, value) &&
        source_query_target_token_reference(ctx, node, declaration, XR_XIR_SOURCE_TYPE_USE);
}
static bool source_hash_construct(SourceContext *ctx, AstNode *node, SourceValue *value) {
    NewExprNode *call = &node->as.new_expr;
    const XrNativeTypeDeclaration *native = call->class_name ? source_native_find(ctx, call->class_name) : NULL;
    if (!native || (native->id != XR_NATIVE_DECLARATION_MAP && native->id != XR_NATIVE_DECLARATION_SET) ||
        call->is_type_namespace || call->module_name || call->type_arg_count != (int)native->parameter_count ||
        !call->type_args || call->arg_count < 0 || call->arg_count > 1 || (call->arg_count && !call->arguments))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "hash construction requires its exact type arguments and optional literal");
    XrTypeRef reference = {.kind = XR_TREF_GENERIC, .name = call->class_name, .children = call->type_args,
        .nchildren = (uint8_t)call->type_arg_count, .line = node->line, .column = node->column};
    XrXirType type = XR_XIR_UNIT;
    if (!source_native_hash_type(ctx, &reference, &type)) return false;
    if (call->arg_count) {
        if ((call->arg_accesses && call->arg_accesses[0] != XR_CALL_ARG_PLAIN) ||
            call->arguments[0]->type != (native->id == XR_NATIVE_DECLARATION_MAP ? AST_MAP_LITERAL : AST_SET_LITERAL))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "hash construction argument must be its literal");
        return source_hash_literal(ctx, call->arguments[0], (SourceExpectedType){true, type, false, false}, value);
    }
    return source_recipe_group(ctx, (XrXirInstruction){native->id == XR_NATIVE_DECLARATION_MAP ? XR_XIR_MAP_NEW : XR_XIR_SET_NEW,
        type, {0}, {0}, 0, {0}}, NULL, 0, value);
}
