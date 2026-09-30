/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_struct.inc.c - Nominal source declarations and value operations
 *
 * KEY CONCEPT:
 *   Source names resolve to the sole XIR declaration and type pool.
 */
typedef struct SourceNominalDeclaration {
    XrGenericParam **parameters;
    int parameter_count;
    AstNode **methods;
    int method_count;
    XrTypeRef **interfaces;
    int interface_count;
} SourceNominalDeclaration;
static bool source_nominal_declaration(SourceContext *ctx, AstNode *node, SourceNominalDeclaration *output) {
    if (!source_work(ctx,node)) return false;
    if (!node || !output) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"nominal declaration is missing");
    if (node->type == AST_ENUM_DECL) {
        EnumDeclNode *d = &node->as.enum_decl;
        *output = (SourceNominalDeclaration){d->type_params,d->type_param_count,d->methods,
            d->method_count,d->interfaces,d->interface_count};
    } else if (node->type == AST_STRUCT_DECL || node->type == AST_CLASS_DECL) {
        ClassDeclNode *d = node->type == AST_CLASS_DECL ? &node->as.class_decl : &node->as.struct_decl;
        *output = (SourceNominalDeclaration){d->type_params,d->type_param_count,d->methods,
            d->method_count,d->interfaces,d->interface_count};
    } else return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"unsupported nominal declaration owner");
    if (output->parameter_count < 0 || output->parameter_count > 65536 ||
        (output->parameter_count && !output->parameters) || output->method_count < 0 ||
        (output->method_count && !output->methods) || output->interface_count < 0 ||
        (output->interface_count && !output->interfaces))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"nominal declaration arrays are malformed");
    return true;
}
static SourceName *source_nominal_name(SourceContext *ctx, const char *name) {
    if (!name) return NULL;
    const char *dot = strchr(name, '.');
    SourceName *symbol;
    if (dot) {
        size_t length = (size_t) (dot - name);
        char *prefix = source_alloc(ctx, length + 1, 1);
        if (!prefix) return NULL;
        memcpy(prefix, name, length);
        symbol = visible_name(ctx, prefix);
        if (symbol && symbol->kind == SOURCE_MODULE) symbol = imported_declaration(ctx, symbol, dot + 1);
        else return NULL;
    } else {
        symbol = visible_name(ctx, name);
        if (symbol && symbol->kind == SOURCE_IMPORT) symbol = imported_declaration(ctx, symbol, symbol->imported);
    }
    return symbol && symbol->kind == SOURCE_NOMINAL ? symbol : NULL;
}
static bool source_nominal_type(SourceContext *ctx, const char *name, XrXirType *type) {
    SourceName *symbol = source_nominal_name(ctx, name);
    if (!symbol) return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "name does not resolve to an admitted nominal type");
    if (ctx->nominals.declarations[symbol->index].parameter_count)
        return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "nominal type requires explicit arguments");
    *type = symbol->type; return true;
}
static bool source_nominal_apply(SourceContext *ctx, SourceName *symbol, XrTypeRef **arguments,
    uint32_t count, XrXirType *type) {
    if (!symbol || symbol->kind != SOURCE_NOMINAL || count != ctx->nominals.declarations[symbol->index].parameter_count)
        return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "nominal type requires its exact explicit arguments");
    if (!count) { *type = symbol->type; return true; }
    if (!arguments || ctx->depth >= 128)
        return source_fail(ctx, NULL, XR_XIR_BUDGET, "nominal type argument depth exhausted");
    XrXirType *types = source_alloc(ctx, count, sizeof(*types));
    if (!types) return false;
    ++ctx->depth;
    for (uint32_t i = 0; i < count; ++i)
        if (!source_work(ctx, NULL) || !source_type(ctx, arguments[i], &types[i])) return false;
    --ctx->depth;
    XrXirTypeNode node = {0}; node.kind = XR_XIR_TYPE_NOMINAL;
    node.nominal = (XrXirNominalType) {symbol->index, types, count, NULL, 0};
    return source_intern_type(ctx, node, type);
}
static bool source_nominal_arguments(SourceContext *ctx, const char *name, XrTypeRef **arguments,
    uint32_t count, XrXirType *type) {
    return source_nominal_apply(ctx, source_nominal_name(ctx, name), arguments, count, type);
}
static bool source_nominal_declare(SourceContext *ctx, AstNode *node, const char *name,
    XrGenericParam **parameters, uint32_t count, uint32_t kind) {
    if (count && !parameters)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"nominal type parameters are missing");
    SourceName *symbol = add_name(ctx, &ctx->names[ctx->module], name, node);
    if (!symbol) return false;
    symbol->kind = SOURCE_NOMINAL; symbol->index = ctx->nominals.count++;
    symbol->module = ctx->module; ctx->nominal_sources[symbol->index] = symbol;
    XrXirNominalDeclaration *record = (XrXirNominalDeclaration *) &ctx->nominals.declarations[symbol->index];
    const char *module = ctx->graph->specs[ctx->module].canonical;
    *record = (XrXirNominalDeclaration) {{module, (uint32_t) strlen(module)},
        {name, (uint32_t) strlen(name)}, node->is_exported, NULL, 0, NULL, 0, kind, NULL, 0, 0};
    XrXirTypeNode type = {0}; type.kind = XR_XIR_TYPE_NOMINAL; type.nominal.declaration = symbol->index;
    XrXirConstraint *constraints = count ? source_alloc(ctx, count, sizeof(*constraints)) : NULL;
    XrXirType *arguments = count ? source_alloc(ctx, count, sizeof(*arguments)) : NULL;
    if (count && (!constraints || !arguments)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        XrGenericParam *parameter = parameters[i];
        if (!source_work(ctx, node)) return false;
        if (!parameter || !parameter->name || !*parameter->name)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"nominal type parameter is malformed");
        for (uint32_t j = 0; j < i; ++j) {
            if (!source_work(ctx, node)) return false;
            if (!strcmp(parameter->name, parameters[j]->name))
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "duplicate nominal type parameter");
        }
        arguments[i] = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE + i);
    }
    record->constraints = constraints; record->parameter_count = count;
    type.nominal.arguments = arguments; type.nominal.argument_count = count;
    if (!source_intern_type(ctx, type, &symbol->type) ||
        !source_query_declare(ctx, symbol, XR_XIR_SOURCE_TYPE, 0, source_query_range(ctx, node, name))) return false;
    ((XrXirSourceDeclaration *)ctx->query.declarations)[symbol->declaration - 1].generic_parameter_count = count;
    ((XrXirSourceDeclaration *)ctx->query.declarations)[symbol->declaration - 1].generic_constraints = constraints;
    SourceTypeScope saved = ctx->type_scope;
    ctx->type_scope = (SourceTypeScope){true,node,parameters,count,count ? symbol->declaration : 0,0};
    source_query_binding_type(ctx, symbol); ctx->type_scope = saved; return true;
}
static bool source_nominal_constraints(SourceContext *ctx) {
    SourceTypeScope saved = ctx->type_scope;
    uint32_t module = ctx->module;
    bool ok = true;
    for (uint32_t d = 0; d < ctx->nominals.count && ok; ++d) {
        SourceName *owner = ctx->nominal_sources[d];
        SourceNominalDeclaration syntax;
        if (!source_nominal_declaration(ctx,owner->node,&syntax)) { ok=false; break; }
        XrGenericParam **parameters = syntax.parameters;
        const XrXirNominalDeclaration *declaration = &ctx->nominals.declarations[d];
        uint32_t count = declaration->parameter_count;
        ctx->module = owner->module;
        ctx->type_scope = (SourceTypeScope){true,owner->node,parameters,count,count ? owner->declaration : 0,0};
        XrXirConstraint *constraints = (XrXirConstraint *)declaration->constraints;
        for (uint32_t p = 0; p < count && ok; ++p)
            ok = source_parameter_constraints(ctx,owner->node,parameters[p],&constraints[p]);
    }
    ctx->type_scope = saved; ctx->module = module; return ok;
}
static bool source_struct_declare(SourceContext *ctx, AstNode *node) {
    ClassDeclNode *decl = &node->as.struct_decl;
    if (decl->type_param_count < 0 || decl->type_param_count > 65536 || decl->super_name ||
        decl->is_packed || decl->explicit_align || decl->attr_count || decl->field_count < 0)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "struct declaration contract is not admitted");
    return source_nominal_declare(ctx, node, decl->name, decl->type_params,
        (uint32_t) decl->type_param_count, XR_XIR_NOMINAL_STRUCT);
}
static bool source_class_declare(SourceContext *ctx, AstNode *node) {
    ClassDeclNode *decl = &node->as.class_decl;
    if (!decl->explicit_final || decl->super_name || decl->super_module || decl->is_packed ||
        decl->explicit_align || decl->attr_count || decl->type_param_count < 0 ||
        decl->type_param_count > 65536 || decl->field_count < 0)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"class execution currently requires an explicit final root declaration");
    uint32_t constructors=0;
    for (int i=0;i<decl->method_count;++i) {
        if (!source_work(ctx,decl->methods[i])) return false;
        if (decl->methods[i]->type == AST_METHOD_DECL && decl->methods[i]->as.method_decl.is_constructor) ++constructors;
    }
    if (constructors!=1) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"class execution currently requires one explicit complete constructor");
    if (!source_nominal_declare(ctx,node,decl->name,decl->type_params,
        (uint32_t)decl->type_param_count,XR_XIR_NOMINAL_CLASS)) return false;
    ((XrXirNominalDeclaration *)ctx->nominals.declarations)[ctx->nominals.count-1].flags=XR_XIR_NOMINAL_FINAL;
    return true;
}
static bool source_struct_fields(SourceContext *ctx) {
    for (uint32_t m = 0; m < (uint32_t) ctx->graph->spec_count; ++m) {
        ctx->module = m; ctx->function = m;
        for (SourceName *symbol = ctx->names[m]; symbol; symbol = symbol->next) {
            if (symbol->kind != SOURCE_NOMINAL || (symbol->node->type != AST_STRUCT_DECL && symbol->node->type != AST_CLASS_DECL)) continue;
            ClassDeclNode *decl = symbol->node->type == AST_CLASS_DECL ? &symbol->node->as.class_decl : &symbol->node->as.struct_decl;
            ctx->type_scope = (SourceTypeScope){true,symbol->node,decl->type_params,
                (uint32_t)decl->type_param_count,decl->type_param_count ? symbol->declaration : 0,0};
            uint32_t count = (uint32_t) decl->field_count;
            XrXirNominalField *fields = count ? source_alloc(ctx, count, sizeof(*fields)) : NULL;
            XrXirType *types = count ? source_alloc(ctx, count, sizeof(*types)) : NULL;
            uint32_t *members = count ? source_alloc(ctx, count, sizeof(*members)) : NULL;
            uint32_t *defaults = count ? source_alloc(ctx, count, sizeof(*defaults)) : NULL;
            if (count && (!fields || !types || !members || !defaults)) return false;
            ctx->nominal_members[symbol->index] = members;
            ctx->nominal_defaults[symbol->index] = defaults;
            XrXirNominalDeclaration *record = (XrXirNominalDeclaration *) &ctx->nominals.declarations[symbol->index];
            record->fields = fields; record->field_count = count;
            for (uint32_t f = 0; f < count; ++f) {
                AstNode *node = decl->fields[f];
                if (!source_work(ctx, node) || node->type != AST_FIELD_DECL) return false;
                FieldDeclNode *field = &node->as.field_decl;
                if (!field->field_type || field->is_static || field->is_final || field->is_flexible || field->is_weak)
                    return source_fail(ctx, node, XR_XIR_BAD_TYPE, "stored field contract is not admitted");
                if (!source_type(ctx, field->field_type, &types[f]) || types[f] == XR_XIR_UNIT)
                    return source_fail(ctx, node, XR_XIR_BAD_TYPE, "field requires an admitted value type");
                if (symbol->node->type == AST_CLASS_DECL) {
                    if (field->initializer)
                        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"class field requires an explicit admitted constructor value");
                }
                fields[f] = (XrXirNominalField) {{field->name, (uint32_t) strlen(field->name)}, types[f],
                    (field->is_private ? XR_XIR_FIELD_PRIVATE : 0) | (field->is_protected ? XR_XIR_FIELD_PROTECTED : 0) |
                    (field->is_const ? 0 : XR_XIR_FIELD_MUTABLE)};
                SourceName member = {0}; member.name = source_owned_text(ctx, field->name); member.node = node;
                if (!member.name) return false;
                member.type = types[f]; member.mutable = !field->is_const;
                if (!source_query_declare(ctx, &member, XR_XIR_SOURCE_MEMBER, symbol->declaration,
                    source_query_range(ctx, node, field->name))) return false;
                source_query_binding_type(ctx, &member);
                members[f] = member.declaration;
            }
            XrXirTypeNode *type = (XrXirTypeNode *) &ctx->types.nodes[(uint32_t) symbol->type - XR_XIR_CONSTRUCTED_TYPE_BASE];
            type->nominal.fields = types; type->nominal.field_count = count;
            ctx->type_scope = (SourceTypeScope){0};
        }
    }
    return true;
}
static bool source_nominal_function_scope(SourceContext *ctx, uint32_t index, SourceName *symbol) {
    const XrXirNominalDeclaration *nominal = &ctx->nominals.declarations[symbol->index];
    ctx->bodies[index].type_owner = symbol->node;
    SourceNominalDeclaration declaration;
    if (!source_nominal_declaration(ctx,symbol->node,&declaration)) return false;
    ctx->bodies[index].type_parameters = declaration.parameters;
    ctx->bodies[index].type_parameter_count = nominal->parameter_count;
    ctx->bodies[index].generic_owner = nominal->parameter_count ? symbol->declaration : 0;
    ctx->generics[index].parameter_count = nominal->parameter_count;
    ctx->generics[index].constraints = nominal->constraints;
    ctx->has_generics |= nominal->parameter_count != 0;
    return true;
}
static bool source_struct_default_functions(SourceContext *ctx, uint32_t *next) {
    for (uint32_t m = 0; m < (uint32_t) ctx->graph->spec_count; ++m) {
        for (SourceName *symbol = ctx->names[m]; symbol; symbol = symbol->next) {
            if (!source_work(ctx, symbol->node)) return false;
            if (symbol->kind != SOURCE_NOMINAL || symbol->node->type != AST_STRUCT_DECL) continue;
            ClassDeclNode *decl = &symbol->node->as.struct_decl;
            const XrXirNominalDeclaration *nominal = &ctx->nominals.declarations[symbol->index];
            for (uint32_t f = 0; f < nominal->field_count; ++f) {
                AstNode *node = decl->fields[f];
                if (!source_work(ctx, node)) return false;
                if (!node->as.field_decl.initializer) continue;
                if (*next >= ctx->first_closure)
                    return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "field initializer function range exhausted");
                uint32_t index = (*next)++;
                ctx->nominal_defaults[symbol->index][f] = index;
                ctx->functions[index] = (XrXirFunction) {"$field_default", 14, NULL, 0,
                    nominal->fields[f].type, NULL, 0, NULL, 0, NULL, 0};
                ctx->bodies[index].node = node; ctx->bodies[index].module = m;
                if (!source_nominal_function_scope(ctx,index,symbol)) return false;
                ctx->bodies[index].declaration = ctx->nominal_members[symbol->index][f];
                bool visible = nominal->exported && !(nominal->fields[f].flags & (XR_XIR_FIELD_PRIVATE | XR_XIR_FIELD_PROTECTED));
                ctx->identities[index] = (XrXirFunctionIdentity) {m, visible, symbol->index + 1, 0, 0, 0, XR_XIR_MEMBER_HELPER};
            }
        }
    }
    return true;
}
static bool source_struct_field(SourceContext *ctx, AstNode *node, XrXirType type,
    const char *name, unsigned write, uint32_t *index, XrXirType *field_type) {
    const XrXirTypeNode *found = xr_xir_type_node(&ctx->types, type);
    if (!found || (!xr_xir_type_is_struct(&ctx->types, type) && !xr_xir_type_is_class(&ctx->types, type)))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "member receiver is not a nominal value");
    const XrXirNominalDeclaration *decl = &ctx->nominals.declarations[found->nominal.declaration];
    for (uint32_t f = 0; f < decl->field_count; ++f) {
        if (!source_work(ctx, node)) return false;
        const XrXirNominalField *field = &decl->fields[f];
        if (strlen(name) != field->name.length || memcmp(name, field->name.bytes, field->name.length)) continue;
        bool owner = ctx->identities[ctx->function].nominal_owner == found->nominal.declaration + 1;
        if ((!owner && (field->flags & (XR_XIR_FIELD_PRIVATE | XR_XIR_FIELD_PROTECTED))) || ((write == 1 || write == 3) && !(field->flags & XR_XIR_FIELD_MUTABLE)) || (write == 2 && !owner))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "field access is not permitted");
        uint32_t declaration = found->nominal.declaration;
        SourceSubstitution substitution = {found->nominal.arguments, found->nominal.argument_count};
        if (!source_substitute(ctx, &substitution, field->type, 0, field_type)) return false;
        *index = f;
        return source_query_target_reference(ctx, source_query_range(ctx, node, NULL),
            ctx->nominal_members[declaration][f], write == 3 ? XR_XIR_SOURCE_READ_WRITE : write ? XR_XIR_SOURCE_WRITE : XR_XIR_SOURCE_READ);
    }
    return source_fail(ctx, node, XR_XIR_BAD_TYPE, "unknown struct field");
}
static bool source_struct_construct(SourceContext *ctx, AstNode *node, AstNode *path,
    char **names, AstNode **values, int field_count, XrTypeRef **type_args, int type_arg_count, SourceValue *value) {
    SourceName *symbol = NULL, *binding = NULL;
    if (path && path->type == AST_VARIABLE) {
        binding = visible_name(ctx, path->as.variable.name);
        symbol = source_nominal_name(ctx, path->as.variable.name);
    } else if (path && path->type == AST_MEMBER_ACCESS && path->as.member_access.object->type == AST_VARIABLE) {
        binding = visible_name(ctx, path->as.member_access.object->as.variable.name);
        if (binding && binding->kind == SOURCE_MODULE) symbol = imported_declaration(ctx, binding, path->as.member_access.name);
    }
    if (!symbol || symbol->kind != SOURCE_NOMINAL || symbol->node->type != AST_STRUCT_DECL || type_arg_count < 0 || field_count < 0)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "struct literal requires an admitted nominal declaration");
    XrXirType instance_type;
    if (!source_nominal_apply(ctx, symbol, type_args, (uint32_t)type_arg_count, &instance_type)) return false;
    const XrXirNominalDeclaration *decl = &ctx->nominals.declarations[symbol->index];
    uint32_t count = decl->field_count;
    if ((uint32_t) field_count > count)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "struct literal has too many fields");
    SourceValue *fields = count ? source_alloc(ctx, count, sizeof(*fields)) : NULL;
    bool *seen = count ? source_alloc(ctx, count, sizeof(*seen)) : NULL;
    if (count && (!fields || !seen)) return false;
    for (uint32_t f = 0; f < (uint32_t) field_count; ++f) {
        uint32_t index; XrXirType type;
        if (!source_struct_field(ctx, node, instance_type, names[f], false, &index, &type)) return false;
        if (seen[index]) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "duplicate struct literal field");
        seen[index] = true;
        if (!source_plan_expression(ctx, values[f], (SourceExpectedType){type != XR_XIR_UNIT,type}, &fields[index])) return false;
        if (fields[index].type != type) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "struct field type mismatch");
    }
    for (uint32_t f = 0; f < count; ++f) {
        if (!source_work(ctx, node)) return false;
        if (seen[f]) continue;
        uint32_t function = ctx->nominal_defaults[symbol->index][f];
        bool owner = ctx->identities[ctx->function].nominal_owner == symbol->index + 1;
        if (!function || (!owner && (decl->fields[f].flags & (XR_XIR_FIELD_PRIVATE | XR_XIR_FIELD_PROTECTED))))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "omitted field requires an accessible declaration default");
        const XrXirTypeNode *instance = xr_xir_type_node(&ctx->types, instance_type);
        SourceSubstitution substitution = {instance->nominal.arguments, instance->nominal.argument_count};
        XrXirType field_type;
        if (!source_substitute(ctx, &substitution, decl->fields[f].type, 0, &field_type)) return false;
        XrXirInstruction op = {XR_XIR_CALL, field_type, {0}, {0}, function, {0}};
        if (!source_type_arguments(ctx, node, substitution.types, substitution.count, &op) ||
            !source_recipe_record(ctx, op, &fields[f])) return false;
        if (!source_query_target_reference(ctx, source_query_range(ctx, node, NULL),
            ctx->nominal_members[symbol->index][f], XR_XIR_SOURCE_READ)) return false;
    }
    return source_query_reference(ctx, path, binding, symbol, XR_XIR_SOURCE_TYPE_USE) &&
        source_recipe_group(ctx, (XrXirInstruction) {XR_XIR_STRUCT_NEW, instance_type, {0}, {0}, 0, {0}}, fields, count, value);
}
static bool source_struct_literal(SourceContext *ctx, AstNode *node, SourceValue *value) {
    if (node->type == AST_STRUCT_LITERAL) {
        StructLiteralNode *literal = &node->as.struct_literal;
        return source_struct_construct(ctx, node, literal->type_path, literal->field_names,
            literal->field_values, literal->field_count, literal->type_args, literal->type_arg_count, value);
    }
    EnumConstructNode *literal = &node->as.enum_construct;
    return source_struct_construct(ctx, node, literal->variant_path, literal->field_names,
        literal->field_values, literal->field_count, NULL, 0, value);
}
static bool source_struct_get_value(SourceContext *ctx, AstNode *node, SourceValue receiver, SourceValue *value) {
    uint32_t index; XrXirType type;
    if (!source_struct_field(ctx, node, receiver.type, node->as.member_access.name, false, &index, &type)) return false;
    return source_recipe_record(ctx, (XrXirInstruction) {xr_xir_type_is_class(&ctx->types,receiver.type) ? XR_XIR_CLASS_GET : XR_XIR_STRUCT_GET, type, {receiver.id, 0}, {0}, index, {0}}, value);
}

/* This is a target carrier boundary after collection, not a generic premise.
 * Closed applications use their original ordered arguments. Symbolic fields
 * are proved by the ordinary NOMINAL/FUNCTION proof pass in Checked. */
static bool source_class_carriers(SourceContext *ctx) {
    for (uint32_t t=0;t<ctx->types.count;++t) {
        if (!source_work(ctx,NULL)) return false;
        XrXirType id=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
        if (!xr_xir_type_is_class(&ctx->types,id)) continue;
        const XrXirTypeNode *node=xr_xir_type_node(&ctx->types,id);
        uint32_t owner=node->nominal.declaration;
        SourceSubstitution substitution={node->nominal.arguments,node->nominal.argument_count};
        const XrXirNominalDeclaration *decl=&ctx->nominals.declarations[owner];
        SourceName *symbol=ctx->nominal_sources[owner];
        for (uint32_t f=0;f<decl->field_count;++f) {
            XrXirType field;
            if (!source_substitute(ctx,&substitution,decl->fields[f].type,0,&field)) return false;
            if (!xr_xir_type_span(&ctx->types,field)) {
                XrXirStatus status=xr_xir_class_field_verify(&ctx->types,field,&ctx->budget);
                if (status!=XR_XIR_OK) {
                    ctx->module=symbol->module;
                    return source_fail(ctx,symbol->node,status,status==XR_XIR_BAD_TYPE ?
                        "class field carrier is not implemented" : "class field capability budget or allocation failed");
                }
            }
        }
    }
    return true;
}

static bool source_class_field_capabilities(SourceContext *ctx) {
    for (uint32_t d=0;d<ctx->nominals.count;++d) {
        if (!source_work(ctx,NULL)) return false;
        const XrXirNominalDeclaration *decl=&ctx->nominals.declarations[d];
        if (decl->kind!=XR_XIR_NOMINAL_CLASS) continue;
        SourceName *owner=ctx->nominal_sources[d];ctx->module=owner->module;
        for (uint32_t f=0;f<decl->field_count;++f) {
            if (xr_xir_type_span(&ctx->types,decl->fields[f].type)) continue;
            XrXirStatus status=xr_xir_class_field_verify(&ctx->types,decl->fields[f].type,&ctx->budget);
            if (status!=XR_XIR_OK) return source_fail(ctx,owner->node,status,
                status==XR_XIR_BAD_TYPE ? "class field carrier is not implemented" : "class field capability budget or allocation failed");
        }
    }
    return true;
}
