/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_enum.inc.c - Declaration-owned enum payload construction
 */
static bool source_enum_declare(SourceContext *ctx, AstNode *node) {
    EnumDeclNode *d = &node->as.enum_decl;
    if (d->type_param_count < 0 || d->type_param_count > 65536 || d->member_count <= 0 ||
        d->method_count < 0 || (d->method_count && !d->methods) ||
        d->interface_count < 0 || (d->interface_count && !d->interfaces) || d->attr_count)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "enum declaration contract is not admitted");
    return source_nominal_declare(ctx, node, d->name, d->type_params,
        (uint32_t)d->type_param_count, XR_XIR_NOMINAL_ENUM);
}
static bool source_enum_fields(SourceContext *ctx) {
    for (uint32_t d = 0; d < ctx->nominals.count; ++d) {
        SourceName *owner = ctx->nominal_sources[d];
        if (owner->node->type != AST_ENUM_DECL) continue;
        EnumDeclNode *source = &owner->node->as.enum_decl;
        ctx->module = owner->module; ctx->function = owner->module;
        ctx->type_scope = (SourceTypeScope){true,owner->node,source->type_params,
            (uint32_t)source->type_param_count,source->type_param_count ? owner->declaration : 0,0};
        uint32_t count = 0;
        for (int v = 0; v < source->member_count; ++v) {
            AstNode *member = source->members[v];
            if (!source_work(ctx, member)) return false;
            if (member->type == AST_ENUM_MEMBER && source_text_same(ctx, NULL, member->as.enum_member.name, "variants"))
                return source_fail(ctx,member,XR_XIR_BAD_TYPE,"variant conflicts with enum type metadata member");
            if (member->type != AST_ENUM_MEMBER || member->as.enum_member.payload_count < 0 ||
                (uint32_t)member->as.enum_member.payload_count > UINT32_MAX - count)
                return source_fail(ctx, member, XR_XIR_BAD_TYPE, "invalid enum payload declaration");
            count += (uint32_t)member->as.enum_member.payload_count;
        }
        XrXirNominalVariant *variants = source_alloc(ctx, (uint32_t)source->member_count, sizeof(*variants));
        uint32_t *variant_ids = source_alloc(ctx, (uint32_t)source->member_count + 3, sizeof(*variant_ids));
        XrXirNominalField *fields = count ? source_alloc(ctx, count, sizeof(*fields)) : NULL;
        XrXirType *types = count ? source_alloc(ctx, count, sizeof(*types)) : NULL;
        uint32_t *members = count ? source_alloc(ctx, count, sizeof(*members)) : NULL;
        if (!variants || !variant_ids || (count && (!fields || !types || !members))) return false;
        ctx->nominal_variants[d] = variant_ids; ctx->nominal_members[d] = members;
        XrXirNominalDeclaration *record = (XrXirNominalDeclaration *)&ctx->nominals.declarations[d];
        record->variants = variants; record->variant_count = (uint32_t)source->member_count;
        record->fields = fields; record->field_count = count;
        uint32_t next = 0;
        for (uint32_t v = 0; v < record->variant_count; ++v) {
            AstNode *node = source->members[v]; EnumMemberNode *member = &node->as.enum_member;
            if (!source_work(ctx, node)) return false;
            variants[v] = (XrXirNominalVariant) {{member->name,(uint32_t)source_text_size(ctx, member->name)},next,(uint32_t)member->payload_count};
            SourceName variant = {0}; variant.name = source_owned_text(ctx, member->name); variant.node = node; variant.type = owner->type;
            if (!variant.name || !source_query_declare(ctx, &variant, XR_XIR_SOURCE_MEMBER, owner->declaration,
                source_query_range(ctx, node, member->name))) return false;
            source_query_binding_type(ctx, &variant); variant_ids[v] = variant.declaration;
            for (int f = 0; f < member->payload_count; ++f, ++next) {
                if (!source_work(ctx, node) || !source_type(ctx, member->payload_types[f], &types[next]) || types[next] == XR_XIR_UNIT)
                    return source_fail(ctx, node, XR_XIR_BAD_TYPE, "enum payload requires an admitted value type");
                const char *name = member->payload_names[f];
                fields[next] = (XrXirNominalField) {{name,(uint32_t)source_text_size(ctx, name)},types[next],0};
                SourceName field = {0}; field.name = source_owned_text(ctx, name); field.node = node; field.type = types[next];
                XrNameSpan span = member->payload_name_spans[f];
                XrXirSourceRange range = {ctx->module,span.line,span.column,span.line,span.column};
                if (span.column > 0 && source_text_size(ctx, name) <= (size_t)(INT_MAX - span.column)) range.end_column += (int)source_text_size(ctx, name);
                if (!field.name || !source_query_declare(ctx, &field, XR_XIR_SOURCE_MEMBER, variant.declaration, range)) return false;
                source_query_binding_type(ctx, &field); members[next] = field.declaration;
            }
        }
        SourceName ordinal = {0}; ordinal.name = source_owned_text(ctx, "ordinal"); ordinal.type = XR_XIR_I64;
        if (!ordinal.name || !source_query_declare(ctx, &ordinal, XR_XIR_SOURCE_INTRINSIC, owner->declaration,
            (XrXirSourceRange) {ctx->module,0,0,0,0})) return false;
        source_query_binding_type(ctx, &ordinal); variant_ids[record->variant_count] = ordinal.declaration;
        const char *text_names[] = {"name", "toString"};
        for (uint32_t intrinsic = 0; intrinsic < 2; ++intrinsic) {
            SourceName text = {0}; text.name = source_owned_text(ctx, text_names[intrinsic]); text.type = XR_XIR_STRING;
            if (!source_work(ctx, owner->node) || !text.name ||
                !source_query_declare(ctx, &text, XR_XIR_SOURCE_INTRINSIC, owner->declaration,
                    (XrXirSourceRange) {ctx->module,0,0,0,0})) return false;
            source_query_binding_type(ctx, &text);
            variant_ids[record->variant_count + intrinsic + 1] = text.declaration;
        }
        XrXirTypeNode *type = (XrXirTypeNode *)&ctx->types.nodes[(uint32_t)owner->type - XR_XIR_CONSTRUCTED_TYPE_BASE];
        type->nominal.fields = types; type->nominal.field_count = count;
        ctx->type_scope = (SourceTypeScope){0};
    }
    return true;
}
static bool source_nominal_path(SourceContext *ctx, AstNode *path, SourceTypeArguments *arguments,
    SourceName **binding_out, SourceName **owner_out) {
    AstNode *node = path;
    if (path->type == AST_FUNCTION_REF) {
        if (path->as.function_ref.type_arg_count < 0)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "invalid static owner type arguments");
        arguments->refs = path->as.function_ref.type_args;
        arguments->count = (uint32_t)path->as.function_ref.type_arg_count;
        path = path->as.function_ref.callee;
    }
    SourceName *owner = NULL, *binding = NULL;
    if (path->type == AST_NEW_EXPR && path->as.new_expr.is_type_namespace) {
        NewExprNode *space = &path->as.new_expr;
        if (space->module_name || space->arg_count || space->type_arg_count < 0 || !space->class_name)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "invalid static type namespace");
        binding = owner = visible_name(ctx, space->class_name);
        if (owner && owner->kind == SOURCE_IMPORT) owner = imported_declaration(ctx, owner, owner->imported);
        arguments->refs = space->type_args; arguments->count = (uint32_t)space->type_arg_count;
    } else if (path->type == AST_VARIABLE) {
        binding = owner = visible_name(ctx, path->as.variable.name);
        if (owner && owner->kind == SOURCE_IMPORT) owner = imported_declaration(ctx, owner, owner->imported);
    } else if (path->type == AST_MEMBER_ACCESS && path->as.member_access.object->type == AST_VARIABLE) {
        binding = visible_name(ctx, path->as.member_access.object->as.variable.name);
        if (binding && binding->kind == SOURCE_MODULE)
            owner = imported_declaration(ctx, binding, path->as.member_access.name);
    }
    if (ctx->diagnostic.status != XR_XIR_OK) return false;
    *binding_out = binding; *owner_out = owner; return true;
}
typedef struct SourceEnumSelection { SourceName *owner, *binding; AstNode *path; XrXirType type; uint32_t variant; } SourceEnumSelection;
typedef struct SourceEnumFields { char **names; AstNode **values; XrNameSpan *spans; int count; } SourceEnumFields;
static bool source_enum_select(SourceContext *ctx, AstNode *node, SourceEnumSelection *selected) {
    if (!node || node->type != AST_MEMBER_ACCESS) return true;
    SourceTypeArguments arguments = {0}; SourceName *owner = NULL, *binding = NULL;
    AstNode *path = node->as.member_access.object;
    if (!source_nominal_path(ctx, path, &arguments, &binding, &owner)) return false;
    if (!owner || owner->kind != SOURCE_NOMINAL || ctx->nominals.declarations[owner->index].kind != XR_XIR_NOMINAL_ENUM) return true;
    const XrXirNominalDeclaration *d = &ctx->nominals.declarations[owner->index];
    for (uint32_t v = 0; v < d->variant_count; ++v) {
        if (!source_work(ctx, node)) return false;
        const XrXirLiteral name = d->variants[v].name;
        if (source_text_size(ctx, node->as.member_access.name) != name.length || !source_span_same(ctx, NULL, node->as.member_access.name, name.bytes, name.length)) continue;
        if (!source_nominal_apply(ctx, owner, arguments.refs, arguments.count, &selected->type)) return false;
        selected->owner = owner; selected->binding = binding; selected->path = path; selected->variant = v; return true;
    }
    SourceName *method = find_name(ctx,ctx->nominal_methods[owner->index],node->as.member_access.name);
    if (method && method->node->as.method_decl.is_static) return true;
    return source_fail(ctx,node,XR_XIR_BAD_TYPE,"unknown enum variant or static method");
}
static bool source_enum_construct(SourceContext *ctx, AstNode *node, SourceEnumSelection *selected,
    const SourceEnumFields *provided, SourceValue *value) {
    const XrXirNominalDeclaration *d = &ctx->nominals.declarations[selected->owner->index];
    const XrXirNominalVariant variant = d->variants[selected->variant];
    int count = provided ? provided->count : 0;
    if (count < 0 || (uint32_t)count != variant.field_count || (!variant.field_count && provided))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "enum variant requires exactly its declared named payload");
    SourceValue *fields = count ? source_alloc(ctx,(uint32_t)count,sizeof(*fields)) : NULL;
    bool *seen = count ? source_alloc(ctx,(uint32_t)count,sizeof(*seen)) : NULL;
    if (count && (!fields || !seen)) return false;
    const XrXirNominalType *instance = &xr_xir_type_node(&ctx->types,selected->type)->nominal;
    SourceSubstitution substitution = {instance->arguments,instance->argument_count};
    for (int i = 0; i < count; ++i) {
        uint32_t at = 0;
        for (; at < variant.field_count; ++at) {
            if (!source_work(ctx,node)) return false;
            XrXirLiteral name = d->fields[variant.field_begin + at].name;
            if (source_text_size(ctx, provided->names[i]) == name.length && source_span_same(ctx, NULL, provided->names[i], name.bytes, name.length)) break;
        }
        if (at == variant.field_count || seen[at]) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"unknown or duplicate enum payload field");
        seen[at] = true; XrXirType type;
        if (!source_substitute(ctx,&substitution,d->fields[variant.field_begin+at].type,0,&type) ||
            !source_plan_expression(ctx, provided->values[i], (SourceExpectedType){type != XR_XIR_UNIT,type, false}, &fields[at])) return false;
        if (fields[at].type != type) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"enum payload type mismatch");
        XrXirSourceRange range = source_query_range(ctx,node,NULL);
        if (provided->spans) {
            XrNameSpan span = provided->spans[i];
            range = (XrXirSourceRange){ctx->module,span.line,span.column,span.line,span.column};
            if (span.column > 0 && source_text_size(ctx, provided->names[i]) <= (size_t)(INT_MAX-span.column)) range.end_column += (int)source_text_size(ctx, provided->names[i]);
        }
        if (!source_query_target_reference(ctx,range,ctx->nominal_members[selected->owner->index][variant.field_begin+at],XR_XIR_SOURCE_READ)) return false;
    }
    return source_query_reference(ctx,selected->path,selected->binding,selected->owner,XR_XIR_SOURCE_TYPE_USE) &&
        source_query_target_reference(ctx,source_query_range(ctx,node,NULL),ctx->nominal_variants[selected->owner->index][selected->variant],XR_XIR_SOURCE_READ) &&
        source_recipe_group(ctx,(XrXirInstruction){XR_XIR_ENUM_NEW,selected->type,{0},{0},selected->variant, {0}},fields,(uint32_t)count,value);
}
static bool source_enum_literal(SourceContext *ctx, AstNode *node, SourceValue *value) {
    EnumConstructNode *literal = &node->as.enum_construct; SourceEnumSelection selected = {0};
    if (!source_enum_select(ctx,literal->variant_path,&selected)) return false;
    if (!selected.owner) return source_struct_literal(ctx,node,value);
    SourceEnumFields fields = {literal->field_names,literal->field_values,literal->field_name_spans,literal->field_count};
    return source_enum_construct(ctx,node,&selected,&fields,value);
}
