/*
 * xi_lower_class.inc.c - Class declaration lowering for Xi IR
 *
 * Included directly by xi_lower.c — shares all statics (XiLower,
 * var_lookup_or_create, braun_write, lower_stmt, func_add_child, etc.).
 *
 * Converts AST_CLASS_DECL into XI_CLASS_CREATE:
 *   1. Each method body is lowered to a child XiFunc.
 *   2. XiClassData records the AST node and child-function indices.
 *   3. The emitter (xi_emit.c) builds XrClassDescriptor at emit time,
 *      recursively emits child protos, and generates
 *      OP_CLASS_CREATE_FROM_DESCRIPTOR.
 */

/* Returns true if the initializer is a simple literal that
 * ast_field_default_to_value() can handle at compile time. */
static bool is_simple_literal(AstNode *init) {
    if (!init)
        return true;
    switch (init->type) {
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_LITERAL_RUNE:
        case AST_LITERAL_STRING:
            return true;
        default:
            return false;
    }
}

static bool class_has_complex_instance_initializer(ClassDeclNode *cd) {
    if (!cd)
        return false;
    for (int i = 0; i < cd->field_count; i++) {
        if (cd->fields[i]->type != AST_FIELD_DECL)
            continue;
        FieldDeclNode *f = &cd->fields[i]->as.field_decl;
        if (f->is_static)
            continue;
        if (f->initializer && !is_simple_literal(f->initializer))
            return true;
    }
    return false;
}

static uint32_t class_decl_derive_flags(XrAttribute **attrs, int count) {
    uint32_t flags = 0;
    for (int i = 0; i < count; i++) {
        if (attrs[i] && attrs[i]->kind == ATTR_DERIVE)
            flags |= attrs[i]->derive_flags;
    }
    return flags;
}

static XiClassData *class_find_native_super(XiLower *l, const ClassDeclNode *cd) {
    if (!l || !cd || !cd->super_name || cd->super_module)
        return NULL;
    for (int i = 0; i < l->var_cap; i++) {
        XiClassData *data = l->shared_slot_classes[i];
        if (data && data->class_name && strcmp(data->class_name, cd->super_name) == 0 &&
            data->instance_layout)
            return data;
    }
    return NULL;
}

static uint32_t class_method_evidence_source_node_id(XiLower *l, const ClassDeclNode *cd,
                                                     const AstNode *method_node) {
    uint32_t fallback = xi_lower_source_node_id(l, method_node);
    const XgGlobalEvidence *ev = l ? l->global_evidence : NULL;
    const MethodDeclNode *method = NULL;
    uint32_t class_name_id;
    uint32_t method_name_id;
    const XgClassSummary *owner = NULL;
    const XgMethodSummary *match = NULL;
    if (!l || !cd || !method_node || method_node->type != AST_METHOD_DECL || !ev || !cd->name)
        return fallback;
    method = &method_node->as.method_decl;
    if (!method->name)
        return fallback;
    class_name_id = xg_name_id(cd->name);
    method_name_id = xg_name_id(method->name);
    if (class_name_id == 0 || method_name_id == 0)
        return fallback;
    for (uint32_t i = 0; i < ev->nclasses; i++) {
        const XgClassSummary *cls = &ev->classes[i];
        if (cls->module_id == l->xg_module_id && cls->name_id == class_name_id) {
            if (owner)
                return fallback;
            owner = cls;
        }
    }
    if (!owner || owner->method_start == 0 || owner->method_count == 0)
        return fallback;
    uint32_t start = owner->method_start - 1;
    if (start >= ev->nmethods || owner->method_count > ev->nmethods - start)
        return fallback;
    for (uint32_t i = 0; i < owner->method_count; i++) {
        const XgMethodSummary *candidate = &ev->methods[start + i];
        if (candidate->owner_class_id != owner->class_id || candidate->name_id != method_name_id)
            continue;
        if (match)
            return fallback;
        match = candidate;
    }
    return match && match->source_node_id != 0 ? match->source_node_id : fallback;
}

static XaSymbol *class_method_analyzer_symbol(XiLower *l, const ClassDeclNode *cd,
                                              const MethodDeclNode *m) {
    if (!l || !l->analyzer || !cd || !m || !m->name)
        return NULL;

    XaSymbol *class_sym =
        cd->symbol_id ? xa_scope_lookup_by_id(l->analyzer->global_scope, cd->symbol_id) : NULL;
    if (!class_sym && cd->name)
        class_sym = xa_analyzer_lookup(l->analyzer, cd->name);
    if (!class_sym && cd->name)
        class_sym = xa_analyzer_lookup_deep(l->analyzer, cd->name);

    XaSymbolLinks *class_links = class_sym ? xa_analyzer_get_links(l->analyzer, class_sym) : NULL;
    XrClassInfo *class_info = class_links ? class_links->class_info : NULL;
    XaSymbol *method_sym = m->is_static ? xa_class_info_lookup_static_member(class_info, m->name)
                                        : xa_class_info_lookup_instance_member(class_info, m->name);
    return method_sym;
}

static XrType *class_method_analyzer_signature(XiLower *l, const ClassDeclNode *cd,
                                               const MethodDeclNode *m) {
    XaSymbol *method_sym = class_method_analyzer_symbol(l, cd, m);
    XaSymbolLinks *method_links =
        method_sym && l && l->analyzer ? xa_analyzer_get_links(l->analyzer, method_sym) : NULL;
    XrType *method_type = method_links ? method_links->type : NULL;
    return (method_type && method_type->kind == XR_KIND_FUNCTION) ? method_type : NULL;
}

/* Resolve the analyzer class metadata for a class declaration. The hierarchy in
 * XrClassInfo is linked across module boundaries, so this gives access to a
 * cross-module base class and its fields during per-module lowering. */
static XrClassInfo *class_info_for_decl(XiLower *l, const ClassDeclNode *cd) {
    if (!l || !l->analyzer || !cd)
        return NULL;
    XaSymbol *class_sym =
        cd->symbol_id ? xa_scope_lookup_by_id(l->analyzer->global_scope, cd->symbol_id) : NULL;
    if (!class_sym && cd->name)
        class_sym = xa_analyzer_lookup(l->analyzer, cd->name);
    if (!class_sym && cd->name)
        class_sym = xa_analyzer_lookup_deep(l->analyzer, cd->name);
    XaSymbolLinks *class_links = class_sym ? xa_analyzer_get_links(l->analyzer, class_sym) : NULL;
    return class_links ? class_links->class_info : NULL;
}

/* A class participates in polymorphic vtable dispatch when it has a parent
 * (it is a subclass) or is extended by some subclass. Only such classes require
 * the native heap type-id representation; keeping the check here lets the native
 * layout admit string/tagged fields for them while non-polymorphic string
 * classes stay on the boxed/map path (whose value-boundary ABI is unchanged). */
static bool class_info_is_polymorphic(const XrClassInfo *info) {
    return info && (info->has_subclass || info->base != NULL);
}

/* Append the native field layout for `info` (walking its base chain, base fields
 * first) into `layout` starting at `*out_idx`. Returns false when any field is
 * not representable in a native class instance, in which case the whole class
 * must stay on the boxed/map path. Field native types match
 * class_make_native_instance_layout so a cross-module base and its subclass
 * agree on offsets. */
static bool class_collect_native_fields_from_info(XiLower *l, XrClassInfo *info,
                                                  XrAggregateLayout *layout, uint16_t *out_idx) {
    if (!info)
        return true;
    if (info->base && !class_collect_native_fields_from_info(l, info->base, layout, out_idx))
        return false;
    const bool polymorphic = class_info_is_polymorphic(info);
    for (int i = 0; i < info->field_count; i++) {
        XaSymbol *fs = info->fields ? info->fields[i] : NULL;
        if (!fs || !fs->name)
            return false;
        XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, fs);
        XrType *type = links ? links->type : NULL;
        if (!type || type->kind == XR_KIND_UNKNOWN || type->is_nullable)
            return false;
        int native = xr_type_kind_to_native(type->kind, type->native_width);
        if (native < 0 && (type->kind == XR_KIND_ARRAY || type->kind == XR_KIND_VIEW ||
                           type->kind == XR_KIND_SPAN))
            native = XR_NATIVE_ARRAY_REF;
        if (native < 0 && type->kind == XR_KIND_MAP)
            native = XR_NATIVE_MAP_REF;
        if (native < 0 && type->kind == XR_KIND_SET)
            native = XR_NATIVE_SET_REF;
        if (native < 0)
            return false;
        (void) polymorphic;
        if ((int) *out_idx >= XR_MAX_AGG_FIELDS)
            return false;
        layout->field_names[*out_idx] = arena_strdup(l->func, fs->name);
        layout->fields[*out_idx].native_type = (uint8_t) native;
        (*out_idx)++;
    }
    return true;
}

/* Build a native instance layout for a class from its analyzer metadata. Used
 * for a cross-module base class whose per-module XiClassData is not visible to
 * the current lowering context. */
static XrAggregateLayout *class_make_native_layout_from_info(XiLower *l, XrClassInfo *info) {
    if (!l || !l->func || !info)
        return NULL;
    int total = 0;
    for (XrClassInfo *c = info; c; c = c->base)
        total += c->field_count;
    if (total < 0 || total > XR_MAX_AGG_FIELDS)
        return NULL;
    XrAggregateLayout *layout =
        (XrAggregateLayout *) xi_func_arena_alloc(l->func, sizeof(XrAggregateLayout));
    if (!layout)
        return NULL;
    layout->field_count = (uint16_t) total;
    layout->field_names = NULL;
    if (total > 0) {
        layout->field_names = (const char **) xi_func_arena_alloc(
            l->func, (uint32_t) (sizeof(const char *) * (size_t) total));
        if (!layout->field_names)
            return NULL;
    }
    uint16_t out_idx = 0;
    if (!class_collect_native_fields_from_info(l, info, layout, &out_idx))
        return NULL;
    if (!xr_aggregate_layout_compute(layout, xi_lower_target_data_layout(l)))
        return NULL;
    return layout;
}

static XrAggregateLayout *class_make_native_instance_layout(XiLower *l, ClassDeclNode *cd,
                                                            uint16_t *out_inherited) {
    if (!l || !l->func || !l->isolate || !cd)
        return NULL;

    XiClassData *super_data = class_find_native_super(l, cd);
    XrClassInfo *class_info = class_info_for_decl(l, cd);
    XrAggregateLayout *super_layout = super_data ? super_data->instance_layout : NULL;
    /* Cross-module base: its per-module XiClassData is not visible to this
     * lowering context, so build the inherited layout from the analyzer's linked
     * class hierarchy instead. */
    if (!super_layout && (cd->super_name || cd->super_module) && class_info && class_info->base)
        super_layout = class_make_native_layout_from_info(l, class_info->base);
    if ((cd->super_name || cd->super_module) && !super_layout)
        return NULL;
    uint16_t inherited = super_layout ? super_layout->field_count : 0;
    if (out_inherited)
        *out_inherited = inherited;
    const bool polymorphic = class_info_is_polymorphic(class_info);

    int instance_fields = 0;
    for (int i = 0; i < cd->field_count; i++) {
        if (cd->fields[i]->type != AST_FIELD_DECL)
            continue;
        FieldDeclNode *f = &cd->fields[i]->as.field_decl;
        if (!f->is_static)
            instance_fields++;
    }
    int total_fields = (int) inherited + instance_fields;
    if (total_fields < 0 || total_fields > XR_MAX_AGG_FIELDS)
        return NULL;

    XrAggregateLayout *layout =
        (XrAggregateLayout *) xi_func_arena_alloc(l->func, sizeof(XrAggregateLayout));
    if (!layout)
        return NULL;
    layout->field_count = (uint16_t) total_fields;
    layout->field_names = NULL;
    if (total_fields > 0) {
        layout->field_names = (const char **) xi_func_arena_alloc(
            l->func, (uint32_t) (sizeof(const char *) * (size_t) total_fields));
        if (!layout->field_names)
            return NULL;
    }

    uint16_t out_idx = 0;
    if (super_layout) {
        for (uint16_t i = 0; i < super_layout->field_count; i++) {
            layout->field_names[out_idx] =
                super_layout->field_names ? super_layout->field_names[i] : NULL;
            layout->fields[out_idx] = super_layout->fields[i];
            out_idx++;
        }
    }
    for (int i = 0; i < cd->field_count; i++) {
        if (cd->fields[i]->type != AST_FIELD_DECL)
            continue;
        FieldDeclNode *f = &cd->fields[i]->as.field_decl;
        if (f->is_static)
            continue;
        if (!f->name || !f->field_type)
            return NULL;
        XrType *type = xr_tref_resolve(l->isolate, f->field_type);
        type = xi_lower_type_or_any(l, type, "class field type", 0);
        if (!type || type->kind == XR_KIND_UNKNOWN)
            return NULL;
        int native = xr_type_kind_to_native(type->kind, type->native_width);
        if (native < 0 && (type->kind == XR_KIND_ARRAY || type->kind == XR_KIND_VIEW ||
                           type->kind == XR_KIND_SPAN))
            native = XR_NATIVE_ARRAY_REF;
        if (native < 0 && type->kind == XR_KIND_MAP)
            native = XR_NATIVE_MAP_REF;
        if (native < 0 && type->kind == XR_KIND_SET)
            native = XR_NATIVE_SET_REF;
        if (native < 0)
            return NULL;
        /* Nullable/optional fields are laid out as generic tagged XrValue by the
         * AOT class-field plan; keep such classes boxed to avoid a native_type
         * mismatch at the class-field-plan boundary. */
        if (type->is_nullable)
            return NULL;
        /* Task 215: all classes use the native layout; string/tagged fields are
         * XrValue members with converged tagged field-access rep (see cgen). */
        (void) polymorphic;
        layout->field_names[out_idx] = arena_strdup(l->func, f->name);
        layout->fields[out_idx].native_type = (uint8_t) native;
        out_idx++;
    }

    if (!xr_aggregate_layout_compute(layout, xi_lower_target_data_layout(l)))
        return NULL;
    return layout;
}

/* Lower a class method body to a child XiFunc.
 * Instance methods get an implicit 'this' parameter at index 0.
 * For constructors, cd provides field declarations so complex
 * default values can be lowered as IR before the user body. */
XR_FUNC XiFunc *xi_lower_method_as_func(XiLower *l, MethodDeclNode *m, bool is_inst,
                                        ClassDeclNode *cd, bool owner_is_value_aggregate,
                                        struct XrType *receiver_type, uint32_t source_node_id) {
    XiLower ml;
    xi_lower_init(&ml, l->analyzer, l->isolate);
    ml.parent = l;
    ml.repl_mode = l->repl_mode;
    xi_lower_inherit_evidence(&ml, l);

    const bool is_ctor = m->is_constructor || (m->name && strcmp(m->name, "constructor") == 0);
    XrType *method_sig = class_method_analyzer_signature(l, cd, m);
    /* Prefer the analyzer-owned method signature: it was resolved in the
     * class lexical scope, so self-references and same-module class names do
     * not depend on whatever scope the backend lowering happens to be in. */
    struct XrType *m_ret =
        (method_sig && method_sig->function.return_type)
            ? method_sig->function.return_type
            : (m->return_type ? xr_tref_resolve_in_analyzer(l->analyzer, m->return_type)
                              : ml.type_unit);
    if (!m_ret)
        m_ret = ml.type_unit;
    if (xi_lower_reject_error_type(&ml, m_ret, "method return type", 0)) {
        xi_lower_cleanup(&ml);
        return NULL;
    }
    ml.func = xi_func_new(m->name, m_ret);
    if (!ml.func) {
        xi_lower_cleanup(&ml);
        return NULL;
    }
    ml.func->analyzer = l->analyzer;
    /* A method's own type parameters use the canonical erased method ABI.
     * Methods on an open generic class skeleton are different: their receiver
     * layout is not concrete, so the skeleton body is not executable. */
    ml.func->is_generic_template =
        cd && !cd->is_monomorphized && (cd->type_param_count > 0 || cd->is_generic_skeleton);
    xi_lower_bind_method_body_id(&ml, source_node_id);

    XiBlock *entry = xi_block_new(ml.func);
    entry->sealed = true;
    ml.cur_block = entry;

    bool has_rest = m->is_variadic;
    int base = is_inst ? 1 : 0;
    int np = m->param_count + base;
    ml.func->is_vararg = has_rest;
    ml.func->min_params = (uint16_t) (base + m->required_count);
    ml.func->nparams = (uint16_t) (has_rest ? (np - 1) : np);
    int fixed_params = (int) ml.func->nparams;
    ml.func->entry_type =
        (m->required_count < (has_rest ? m->param_count - 1 : m->param_count)) ? 1 : 0;
    if (np > 0) {
        ml.func->params = (XiValue **) xr_calloc(np, sizeof(XiValue *));
        if (!ml.func->params) {
            xi_func_free(ml.func);
            xi_lower_cleanup(&ml);
            return NULL;
        }
    }

    struct XrType *this_type = receiver_type ? receiver_type : ml.type_any;
    if (is_inst && !receiver_type && cd && cd->name) {
        struct XrType *named_this = xr_type_new_named_instance(l->isolate, cd->name);
        if (named_this)
            this_type = named_this;
    }
    /* A constructor's executable ABI returns the newly initialized receiver.
     * Keep that fact in XiFunc itself instead of teaching every verifier and
     * backend a unit-return exception for a value-carrying RETURN block. */
    if (is_ctor && is_inst)
        ml.func->return_type = this_type;

    XaSymbol *method_sym = class_method_analyzer_symbol(l, cd, m);
    xi_lower_publish_effect_sidecars(ml.func, l->analyzer, method_sym);
    XaSymbolLinks *owner_links = method_sym && method_sym->parent
                                     ? xa_analyzer_get_links(l->analyzer, method_sym->parent)
                                     : NULL;
    bool value_receiver =
        is_inst && !is_ctor &&
        (owner_is_value_aggregate ||
         ((this_type && this_type->is_value_type) ||
          (owner_links && owner_links->class_info && owner_links->class_info->struct_layout)));

    /* Value-aggregate receivers use the same call-bound place contract as
     * explicit `in`/`ref` parameters. Readonly methods borrow the place;
     * mutating methods write through it. Reference-class receivers retain the
     * ordinary value parameter ABI. */
    if (is_inst) {
        XiValue *th = xi_param(ml.func, entry, 0, this_type);
        ml.func->params[0] = th;
        int this_var = xi_lower_var_create(&ml, 0, "this", this_type);
        if (value_receiver) {
            XrParamMode receiver_mode =
                method_sym && method_sym->mutates_receiver ? XR_PARAM_REF : XR_PARAM_READ;
            if (!xi_func_set_param_passing_mode(ml.func, 0, receiver_mode)) {
                xi_func_free(ml.func);
                xi_lower_cleanup(&ml);
                return NULL;
            }
            ml.vars[this_var].call_place = th;
            ml.vars[this_var].place_mode = receiver_mode;
        } else {
            xi_lower_braun_write(&ml, this_var, entry, th);
        }
    }

    /* Resolve each source ParamContract type through the analyzer so
     * XiValue->type carries a real runtime type. */
    for (int i = 0; i < m->param_count; i++) {
        XrParamNode *param = m->params ? m->params[i] : NULL;
        struct XrType *pt = ml.type_any;
        struct XrType *sig_param = xr_type_function_param_type(method_sig, i);
        if (sig_param) {
            pt = sig_param;
        } else if (param && param->type) {
            struct XrType *resolved = xr_tref_resolve_in_analyzer(l->analyzer, param->type);
            if (resolved)
                pt = resolved;
        }
        if (xi_lower_reject_error_type(&ml, pt, "method parameter type", 0)) {
            xi_func_free(ml.func);
            xi_lower_cleanup(&ml);
            return NULL;
        }
        XiValue *p = xi_param(ml.func, entry, (uint16_t) (base + i), pt);
        ml.func->params[base + i] = p;
        XrParamMode mode = param ? param->passing_mode : XR_PARAM_READ;
        if ((base + i) < fixed_params && mode != XR_PARAM_READ &&
            !xi_func_set_param_passing_mode(ml.func, (uint16_t) (base + i), mode)) {
            xi_func_free(ml.func);
            xi_lower_cleanup(&ml);
            return NULL;
        }
        XR_DCHECK(param != NULL && param->name != NULL, "method param name must not be NULL");
        int param_var = xi_lower_var_create(&ml, param->symbol_id, param->name, pt);
        if ((base + i) < fixed_params && mode == XR_PARAM_REF) {
            ml.vars[param_var].call_place = p;
            ml.vars[param_var].place_mode = mode;
        } else {
            xi_lower_braun_write(&ml, param_var, entry, p);
        }
    }

    /* For constructors: emit field default init for complex expressions.
     * Simple literals (int/float/bool/string) are handled by the VM via
     * field_default_values on the class descriptor; complex expressions
     * (array, map, json, new-expr, etc.) must be lowered as IR. */
    /* Non-constructor instance methods receive `this` BORROWED: the caller
     * retains ownership and does not dup before the call, so xi_arc must not
     * drop param 0 here. Constructors own their freshly-allocated `this` and
     * move it out via the auto-return below, so they are NOT borrowed. */
    ml.func->receiver_borrowed = (is_inst && !is_ctor);
    ml.func->receiver_call_place = value_receiver;

    /* Operator-overload methods receive ALL operands borrowed: the VM
     * operator dispatch (OP_ADD/OP_EQ/OP_INDEX/...) leaves the operands live
     * in the caller's registers and the call site does not dup them, so the
     * operator body must drop none of its params. */
    ml.func->operator_borrowed = m->is_operator;

    if (is_ctor && is_inst && cd) {
        int this_var_init = xi_lower_var_create(&ml, 0, "this", this_type);
        XiValue *this_val = xi_lower_braun_read(&ml, this_var_init, ml.cur_block);
        for (int fi = 0; fi < cd->field_count; fi++) {
            if (cd->fields[fi]->type != AST_FIELD_DECL)
                continue;
            FieldDeclNode *f = &cd->fields[fi]->as.field_decl;
            if (f->is_static)
                continue;
            if (!f->initializer || is_simple_literal(f->initializer))
                continue;
            XiValue *val = xi_lower_expr(&ml, f->initializer);
            if (!val)
                continue;
            XiValue *st = xi_value_new(ml.func, ml.cur_block, XI_STORE_FIELD, ml.type_unit, 2);
            if (!st)
                continue;
            st->args[0] = this_val;
            st->args[1] = val;
            st->aux = (void *) arena_strdup(ml.func, f->name);
            st->aux_int = xi_lower_method_symbol(&ml, f->name);
            xi_lower_bind_class_field_id(&ml, st, this_type, f->name);
            st->flags |= XI_FLAG_SIDE_EFFECT;
        }
    }

    if (m->body)
        xi_lower_stmt(&ml, m->body);

    /* Constructors auto-return 'this' (param 0) so the caller gets
     * the freshly-created instance, matching the legacy codegen
     * convention (xemit_return(emitter, 0, 1) at end of constructor). */
    if (ml.cur_block) {
        if (is_ctor && is_inst) {
            int this_var = xi_lower_var_create(&ml, 0, "this", this_type);
            XiValue *this_ret = xi_lower_braun_read(&ml, this_var, ml.cur_block);
            xi_block_set_return(ml.cur_block, this_ret);
        } else {
            xi_block_set_return(ml.cur_block, NULL);
        }
    }

    XiFunc *result = NULL;
    if (!ml.had_error && xi_lower_capture_source_vars(&ml))
        result = ml.func;
    xi_lower_cleanup(&ml);
    return result;
}

/* Lower AST_CLASS_DECL: compile methods as child XiFuncs,
 * emit XI_CLASS_CREATE carrying XiClassData for the emitter. */
XR_FUNC void xi_lower_class_decl(XiLower *l, AstNode *node) {
    ClassDeclNode *cd = &node->as.class_decl;
    bool owner_is_value_aggregate = node->type == AST_STRUCT_DECL || node->type == AST_UNION_DECL;
    XR_DCHECK(cd->name != NULL, "class name must not be NULL");

    /* Count instance / static methods (skip static constructors) */
    uint16_t inst_n = 0, stat_n = 0;
    bool has_ctor = false;
    for (int i = 0; i < cd->method_count; i++) {
        if (cd->methods[i]->type != AST_METHOD_DECL)
            continue;
        MethodDeclNode *m = &cd->methods[i]->as.method_decl;
        if (m->is_static_constructor)
            continue;
        if (m->is_static)
            stat_n++;
        else {
            inst_n++;
            if (m->is_constructor || (m->name && strcmp(m->name, "constructor") == 0))
                has_ctor = true;
        }
    }

    bool synth_ctor = !has_ctor && class_has_complex_instance_initializer(cd);
    if (synth_ctor)
        inst_n++;

    /* Lower each method body to a child XiFunc, recording child indices */
    uint16_t total = inst_n + stat_n;
    uint16_t *cidx =
        total ? (uint16_t *) xi_func_arena_alloc(l->func, total * sizeof(uint16_t)) : NULL;
    uint16_t ci = 0;
    bool emitted_synth_ctor = false;
    for (int i = 0; i < cd->method_count; i++) {
        if (cd->methods[i]->type != AST_METHOD_DECL)
            continue;
        MethodDeclNode *m = &cd->methods[i]->as.method_decl;
        if (m->is_static_constructor || m->is_static)
            continue;

        XiFunc *mf =
            xi_lower_method_as_func(l, m, true, cd, owner_is_value_aggregate, NULL,
                                    class_method_evidence_source_node_id(l, cd, cd->methods[i]));
        if (!mf) {
            l->had_error = true;
            continue;
        }
        xi_lower_func_add_child(l->func, mf);
        if (cidx)
            cidx[ci] = (uint16_t) (l->func->nchildren - 1);
        ci++;
    }

    if (synth_ctor) {
        MethodDeclNode synth = {0};
        synth.name = "constructor";
        synth.is_constructor = true;

        XiFunc *mf =
            xi_lower_method_as_func(l, &synth, true, cd, owner_is_value_aggregate, NULL, 0);
        if (mf) {
            xi_lower_func_add_child(l->func, mf);
            if (cidx)
                cidx[ci] = (uint16_t) (l->func->nchildren - 1);
            ci++;
            emitted_synth_ctor = true;
        } else {
            l->had_error = true;
        }
    }

    for (int i = 0; i < cd->method_count; i++) {
        if (cd->methods[i]->type != AST_METHOD_DECL)
            continue;
        MethodDeclNode *m = &cd->methods[i]->as.method_decl;
        if (m->is_static_constructor || !m->is_static)
            continue;

        XiFunc *mf =
            xi_lower_method_as_func(l, m, false, cd, owner_is_value_aggregate, NULL,
                                    class_method_evidence_source_node_id(l, cd, cd->methods[i]));
        if (!mf) {
            l->had_error = true;
            continue;
        }
        xi_lower_func_add_child(l->func, mf);
        if (cidx)
            cidx[ci] = (uint16_t) (l->func->nchildren - 1);
        ci++;
    }

    if (synth_ctor && !emitted_synth_ctor) {
        synth_ctor = false;
        inst_n--;
        total--;
    }

    /* Resolve super class from scope chain so the VM uses the
     * locally-defined class, not a same-named builtin. */
    XiValue *super_val = NULL;
    if (cd->super_name && !cd->super_module) {
        int svar = xi_lower_var_find(l, 0, cd->super_name);
        if (svar >= 0) {
            if (l->is_program && l->shared_map[svar] >= 0) {
                XiTopBinding b;
                b.slot = l->shared_map[svar];
                b.name = l->vars[svar].name;
                b.type = l->vars[svar].type;
                super_val = xi_lower_emit_top_load(l, b, l->type_any);
            } else {
                super_val = xi_lower_braun_read(l, svar, l->cur_block);
            }
        }
        if (!super_val) {
            XiTopBinding tb = xi_lower_find_top_binding(l, 0, cd->super_name);
            if (xi_top_binding_valid(tb))
                super_val = xi_lower_emit_top_load(l, tb, l->type_any);
        }
    }

    /* Create XI_CLASS_CREATE value with XiClassData metadata.
     * args[0] = resolved super class (NULL if none or unresolved). */
    uint16_t nclass_args = super_val ? 1 : 0;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CLASS_CREATE, l->type_any, nclass_args);
    if (!v)
        return;
    if (super_val)
        v->args[0] = super_val;

    /* Lower static constructor (<clinit>) if present */
    int clinit_idx = -1;
    for (int i = 0; i < cd->method_count; i++) {
        if (cd->methods[i]->type != AST_METHOD_DECL)
            continue;
        MethodDeclNode *m = &cd->methods[i]->as.method_decl;
        if (!m->is_static_constructor)
            continue;
        XiFunc *cf =
            xi_lower_method_as_func(l, m, false, cd, owner_is_value_aggregate, NULL,
                                    class_method_evidence_source_node_id(l, cd, cd->methods[i]));
        if (cf) {
            xi_lower_func_add_child(l->func, cf);
            clinit_idx = (int) (l->func->nchildren - 1);
        } else {
            l->had_error = true;
        }
        break;
    }

    XiClassData *data = (XiClassData *) xi_func_arena_alloc(l->func, sizeof(XiClassData));
    XR_DCHECK(data != NULL, "class data alloc failed");
    data->ast = node;
    data->class_info = NULL;
    data->class_name = arena_strdup(l->func, cd->name);
    data->super_name = arena_strdup(l->func, cd->super_name);
    data->generic_origin_name = arena_strdup(l->func, cd->generic_origin_name);
    data->display_name = arena_strdup(l->func, cd->display_name);
    data->is_generic_skeleton = cd->type_param_count > 0 || cd->is_generic_skeleton;
    data->is_monomorphized = cd->is_monomorphized;
    /* Copy concrete type arg display names (arena-duplicated) */
    data->mono_type_arg_names = NULL;
    data->mono_type_arg_count = 0;
    if (cd->mono_type_arg_count > 0 && cd->mono_type_arg_names) {
        const char **names = (const char **) xi_func_arena_alloc(l->func, cd->mono_type_arg_count *
                                                                              sizeof(const char *));
        if (names) {
            for (int ti = 0; ti < cd->mono_type_arg_count; ti++)
                names[ti] = arena_strdup(l->func, cd->mono_type_arg_names[ti]);
            data->mono_type_arg_names = names;
            data->mono_type_arg_count = cd->mono_type_arg_count;
        }
    }
    data->child_idx = cidx;
    data->ninst = inst_n;
    data->nstat = stat_n;
    data->clinit_child_idx = clinit_idx;
    data->derive_flags = class_decl_derive_flags(cd->attributes, cd->attr_count);

    /* Propagate struct_layout from analyzer for VALUE_TYPE classes.
     * The layout is owned by XrClassInfo and outlives the IR arena. */
    data->struct_layout = NULL;
    data->instance_layout = NULL;
    data->inherited_field_count = 0;
    data->is_cycle_candidate = false;
    if (cd->name && l->analyzer) {
        XaSymbol *cls_sym = xa_analyzer_lookup(l->analyzer, cd->name);
        if (!cls_sym)
            cls_sym = xa_analyzer_lookup_deep(l->analyzer, cd->name);
        if (cls_sym) {
            XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, cls_sym);
            XrClassInfo *class_info = links ? links->class_info : NULL;
            data->class_info = class_info;
            if (class_info && class_info->struct_layout)
                data->struct_layout = class_info->struct_layout;
            if (class_info && !class_info->struct_layout)
                data->instance_layout =
                    class_make_native_instance_layout(l, cd, &data->inherited_field_count);
            if (links && links->type && links->type->is_cycle_candidate)
                data->is_cycle_candidate = true;
        }
    }

    /* Build arena-safe method descriptor array so cgen can resolve
     * class methods without depending on AST after lowering. */
    data->nmethod = total;
    data->methods = NULL;
    if (total > 0) {
        data->methods =
            (XiClassMethod *) xi_func_arena_alloc(l->func, total * sizeof(XiClassMethod));
        if (data->methods) {
            uint16_t mi = 0;
            for (int i = 0; i < cd->method_count && mi < total; i++) {
                if (cd->methods[i]->type != AST_METHOD_DECL)
                    continue;
                MethodDeclNode *m = &cd->methods[i]->as.method_decl;
                if (m->is_static_constructor || m->is_static)
                    continue;
                data->methods[mi].name = arena_strdup(l->func, m->name);
                data->methods[mi].symbol_id = xi_lower_method_symbol(l, m->name);
                data->methods[mi].is_constructor =
                    m->is_constructor || (m->name && strcmp(m->name, "constructor") == 0);
                data->methods[mi].is_static = false;
                data->methods[mi].is_static_constructor = false;
                mi++;
            }
            if (synth_ctor && mi < total) {
                data->methods[mi].name = arena_strdup(l->func, "constructor");
                data->methods[mi].symbol_id = xi_lower_method_symbol(l, "constructor");
                data->methods[mi].is_constructor = true;
                data->methods[mi].is_static = false;
                data->methods[mi].is_static_constructor = false;
                mi++;
            }
            for (int i = 0; i < cd->method_count && mi < total; i++) {
                if (cd->methods[i]->type != AST_METHOD_DECL)
                    continue;
                MethodDeclNode *m = &cd->methods[i]->as.method_decl;
                if (m->is_static_constructor || !m->is_static)
                    continue;
                data->methods[mi].name = arena_strdup(l->func, m->name);
                data->methods[mi].symbol_id = xi_lower_method_symbol(l, m->name);
                data->methods[mi].is_constructor =
                    m->is_constructor || (m->name && strcmp(m->name, "constructor") == 0);
                data->methods[mi].is_static = true;
                data->methods[mi].is_static_constructor = false;
                mi++;
            }
        }
    }
    v->aux = data;
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;

    /* Bind class to its name in SSA */
    int var_id = xi_lower_var_create(l, cd->symbol_id, cd->name, l->type_any);
    xi_lower_braun_write(l, var_id, l->cur_block, v);

    /* Top-level classes: also store into backing store for cross-scope access */
    if (l->is_program && var_id < l->var_count && l->shared_map[var_id] >= 0) {
        int slot = l->shared_map[var_id];
        XiTopBinding b;
        b.slot = slot;
        b.name = l->vars[var_id].name;
        b.type = l->vars[var_id].type;
        xi_lower_emit_top_store(l, b, v);
        /* Track class → shared slot for module export metadata */
        if (slot >= 0 && slot < l->var_cap)
            l->shared_slot_classes[slot] = data;
    }
}
