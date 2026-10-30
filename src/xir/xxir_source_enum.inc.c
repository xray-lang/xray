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
        if (owner->checked_library) continue;
        if (owner->node->type != AST_ENUM_DECL) continue;
        EnumDeclNode *source = &owner->node->as.enum_decl;
        ctx->module = owner->module; ctx->function = owner->module;
        ctx->type_scope = (SourceTypeScope){true,owner->node,source->type_params,
            (uint32_t)source->type_param_count,source->type_param_count ? owner->declaration : 0,0};
        uint32_t count = 0;
        for (int v = 0; v < source->member_count; ++v) {
            AstNode *member = source->members[v];
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
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
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            variants[v] = (XrXirNominalVariant) {{member->name,(uint32_t)source_text_size(ctx, member->name)},next,(uint32_t)member->payload_count};
            SourceName variant = {0}; variant.name = source_owned_text(ctx, member->name); variant.node = node; variant.type = owner->type;
            if (!variant.name || !source_query_declare(ctx, &variant, XR_XIR_SOURCE_MEMBER, owner->declaration,
                source_query_range(ctx, node, member->name))) return false;
            source_query_binding_type(ctx, &variant); variant_ids[v] = variant.declaration;
            for (int f = 0; f < member->payload_count; ++f, ++next) {
                if (!((ctx)->diagnostic.status == XR_XIR_OK) || !source_type(ctx, member->payload_types[f], &types[next]) || types[next] == XR_XIR_UNIT)
                    return source_fail(ctx, node, XR_XIR_BAD_TYPE, "enum payload requires an admitted value type");
                XrXirType logical=types[next];
                if (record->native.native_id==XR_NATIVE_DECLARATION_TASK_RESULT && next==1)
                    types[next]=XR_XIR_ERROR;
                const char *name = member->payload_names[f];
                fields[next] = (XrXirNominalField) {{name,(uint32_t)source_text_size(ctx, name)},types[next],0};
                SourceName field = {0}; field.name = source_owned_text(ctx, name); field.node = node; field.type = logical;
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
            if (!((ctx)->diagnostic.status == XR_XIR_OK) || !text.name ||
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
        if (!owner) binding = owner = source_prelude_lookup(ctx, space->class_name);
        if (owner && owner->kind == SOURCE_IMPORT) owner = imported_declaration(ctx, owner, owner->imported);
        arguments->refs = space->type_args; arguments->count = (uint32_t)space->type_arg_count;
    } else if (path->type == AST_VARIABLE) {
        binding = owner = visible_name(ctx, path->as.variable.name);
        if (!owner) binding = owner = source_prelude_lookup(ctx, path->as.variable.name);
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
    size_t length=source_text_size(ctx,node->as.member_access.name);
    if(length>UINT32_MAX)return source_fail(ctx,node,XR_XIR_BUDGET,"variant name size exhausted");
    XrHirNominalSelection variant={0};XrXirStatus status=xr_hir_nominal_variant_find(&ctx->types,owner->type,
        (XrXirLiteral){node->as.member_access.name,(uint32_t)length},ctx->request,&variant);
    if(!source_hir_nominal_status(ctx,node,status,"enum variant receiving failed"))return false;
    if(variant.present) {
        if(!source_nominal_apply(ctx,owner,arguments.refs,arguments.count,&selected->type))return false;
        selected->owner=owner;selected->binding=binding;selected->path=path;selected->variant=variant.index;return true;
    }
    SourceName *method = find_name(ctx,ctx->nominal_methods[owner->index],node->as.member_access.name);
    if (method) {
        const XrXirFunctionIdentity *identity = source_method_identity(ctx,node,method);
        if (!identity) return false;
        if (identity->method_kind == XR_XIR_STATIC_METHOD) return true;
    }
    return source_fail(ctx,node,XR_XIR_BAD_TYPE,"unknown enum variant or static method");
}
static bool source_enum_construct(SourceContext *ctx,AstNode *node,SourceEnumSelection *selected,
    const SourceEnumFields *provided,SourceValue *value) {
    XrXirTaskResultType outcome={0};
    bool task_result=xr_xir_task_result_type_info(&ctx->types,selected->type,&outcome);
    if(task_result&&selected->variant==XR_XIR_TASK_RESULT_ERROR&&outcome.error==XR_XIR_NEVER)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"TaskResult Error cannot be constructed with Never");
    int count=provided?provided->count:0;
    if(count<0)return source_fail(ctx,node,XR_XIR_BAD_TYPE,"enum variant requires exactly its declared named payload");
    XrHirNominalShape shape={0};XrHirNominalInfo info={0};XrHirBodyScope scope={0};
    if(!source_hir_nominal_shape(ctx,node,selected->type,&shape,&info)||!source_hir_current_scope(ctx,&scope))return false;
    XrHirNominalPlanInput input={.shape=shape,.scope=scope,.origin=source_hir_origin(ctx,node,selected->owner->declaration),
        .variant=selected->variant,.count=(uint32_t)count,.kind=XR_HIR_NOMINAL_ENUM_LITERAL,.provided=provided!=NULL};
    XrHirNominalPlan plan={0};const char *reason=NULL;XrXirStatus status=xr_hir_body_nominal_begin(ctx->typed_body,&input,ctx->request,&plan,&reason);
    if(!source_hir_nominal_status(ctx,node,status,reason))return false;
    XrHirNominalPool pool=source_hir_nominal_pool(ctx);
    uint32_t begin=info.variants[selected->variant].field_begin;
    for(uint32_t i=0;i<(uint32_t)count;++i) {
        size_t length=source_text_size(ctx,provided->names[i]);if(length>UINT32_MAX)return source_fail(ctx,node,XR_XIR_BUDGET,"field name size exhausted");
        uint32_t at=0;status=xr_hir_body_nominal_select(ctx->typed_body,plan,(XrXirLiteral){provided->names[i],(uint32_t)length},ctx->request,&at,&reason);
        if(!source_hir_nominal_status(ctx,node,status,reason))return false;
        XrHirNominalField field={0};if(!source_hir_nominal_field(ctx,node,shape,begin+at,&field))return false;
        SourceValue received;
        if(!source_plan_expression(ctx,provided->values[i],(SourceExpectedType){field.type!=XR_XIR_UNIT,field.type,false,false},&received))return false;
        status=xr_hir_body_nominal_supply(ctx->typed_body,plan,&pool,at,(XrHirNominalValue){.type=received.type,.value=received.id},&reason);
        if(!source_hir_nominal_status(ctx,node,status,reason))return false;
        XrXirSourceRange range=source_query_range(ctx,node,NULL);
        if(provided->spans) {
            XrNameSpan span=provided->spans[i];range=(XrXirSourceRange){ctx->module,span.line,span.column,span.line,span.column};
            if(span.column>0&&length<=(size_t)(INT_MAX-span.column))range.end_column+=(int)length;
        }
        if(!source_query_target_reference(ctx,range,ctx->nominal_members[selected->owner->index][begin+at],XR_XIR_SOURCE_READ))return false;
    }
    XrHirNominalAction action;status=xr_hir_body_nominal_complete(ctx->typed_body,plan,&pool,&action,&reason);
    return source_hir_nominal_status(ctx,node,status,reason)&&source_query_reference(ctx,selected->path,selected->binding,selected->owner,XR_XIR_SOURCE_TYPE_USE)&&
        source_query_target_token_reference(ctx,node,ctx->nominal_variants[selected->owner->index][selected->variant],XR_XIR_SOURCE_READ)&&
        source_hir_nominal_emit(ctx,node,&action,value);
}
static bool source_enum_literal(SourceContext *ctx, AstNode *node, SourceValue *value) {
    EnumConstructNode *literal = &node->as.enum_construct; SourceEnumSelection selected = {0};
    if (!source_enum_select(ctx,literal->variant_path,&selected)) return false;
    if (!selected.owner) return source_struct_literal(ctx,node,value);
    SourceEnumFields fields = {literal->field_names,literal->field_values,literal->field_name_spans,literal->field_count};
    return source_enum_construct(ctx,node,&selected,&fields,value);
}
