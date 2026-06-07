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

static XiClassData *class_find_native_super(XiLower *l, const ClassDeclNode *cd) {
    if (!l || !cd || !cd->super_name || cd->super_module)
        return NULL;
    for (int i = 0; i < XI_LOWER_MAX_VARS; i++) {
        XiClassData *data = l->shared_slot_classes[i];
        if (data && data->class_name && strcmp(data->class_name, cd->super_name) == 0 &&
            data->instance_layout)
            return data;
    }
    return NULL;
}

static XrStructLayout *class_make_native_instance_layout(XiLower *l, ClassDeclNode *cd,
                                                         uint16_t *out_inherited) {
    if (!l || !l->func || !l->isolate || !cd)
        return NULL;

    XiClassData *super_data = class_find_native_super(l, cd);
    if ((cd->super_name || cd->super_module) && !super_data)
        return NULL;
    uint16_t inherited = super_data ? super_data->instance_layout->field_count : 0;
    if (out_inherited)
        *out_inherited = inherited;

    int instance_fields = 0;
    for (int i = 0; i < cd->field_count; i++) {
        if (cd->fields[i]->type != AST_FIELD_DECL)
            continue;
        FieldDeclNode *f = &cd->fields[i]->as.field_decl;
        if (!f->is_static)
            instance_fields++;
    }
    int total_fields = (int) inherited + instance_fields;
    if (total_fields <= 0 || total_fields > XR_MAX_STRUCT_FIELDS)
        return NULL;

    XrStructLayout *layout =
        (XrStructLayout *) xi_func_arena_alloc(l->func, sizeof(XrStructLayout));
    if (!layout)
        return NULL;
    layout->field_count = (uint16_t) total_fields;
    layout->field_names = (const char **) xi_func_arena_alloc(
        l->func, (uint32_t) (sizeof(const char *) * (size_t) total_fields));
    if (!layout->field_names)
        return NULL;

    uint16_t out_idx = 0;
    if (super_data && super_data->instance_layout) {
        XrStructLayout *parent = super_data->instance_layout;
        for (uint16_t i = 0; i < parent->field_count; i++) {
            layout->field_names[out_idx] = parent->field_names ? parent->field_names[i] : NULL;
            layout->fields[out_idx] = parent->fields[i];
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
        if (!type || type->kind == XR_KIND_UNKNOWN)
            return NULL;
        int native = xr_type_kind_to_native(type->kind, type->native_width);
        if (native < 0 && type->kind == XR_KIND_ARRAY)
            native = XR_NATIVE_ARRAY_REF;
        if (native < 0 && type->kind == XR_KIND_MAP)
            native = XR_NATIVE_MAP_REF;
        if (native < 0 && type->kind == XR_KIND_SET)
            native = XR_NATIVE_SET_REF;
        if (native < 0 || native == XR_NATIVE_STRING)
            return NULL;
        layout->field_names[out_idx] = arena_strdup(l->func, f->name);
        layout->fields[out_idx].native_type = (uint8_t) native;
        out_idx++;
    }

    xr_struct_layout_compute(layout);
    return layout;
}

/* Lower a class method body to a child XiFunc.
 * Instance methods get an implicit 'this' parameter at index 0.
 * For constructors, cd provides field declarations so complex
 * default values can be lowered as IR before the user body. */
static XiFunc *lower_method_as_func(XiLower *l, MethodDeclNode *m, bool is_inst,
                                    ClassDeclNode *cd) {
    XiLower ml;
    xi_lower_init(&ml, l->analyzer, l->isolate);
    ml.parent = l;
    ml.repl_mode = l->repl_mode;

    /* MethodDeclNode->return_type is XrTypeRef* (AST syntax). Resolve it
     * to a runtime XrType* before assigning to XiFunc->return_type;
     * mixing the two struct layouts produces garbage downstream when
     * lowering reads bool fields like is_value_type / is_nullable. */
    struct XrType *m_ret =
        m->return_type ? xr_tref_resolve(l->isolate, m->return_type) : ml.type_unit;
    if (!m_ret)
        m_ret = ml.type_unit;
    ml.func = xi_func_new(m->name, m_ret);
    if (!ml.func) {
        xi_lower_cleanup(&ml);
        return NULL;
    }
    ml.func->analyzer = l->analyzer;

    XiBlock *entry = xi_block_new(ml.func);
    entry->sealed = true;
    ml.cur_block = entry;

    int np = m->param_count + (is_inst ? 1 : 0);
    ml.func->nparams = (uint16_t) np;
    if (np > 0) {
        ml.func->params = (XiValue **) xr_calloc(np, sizeof(XiValue *));
        if (!ml.func->params) {
            xi_func_free(ml.func);
            xi_lower_cleanup(&ml);
            return NULL;
        }
    }

    struct XrType *this_type = ml.type_any;
    if (is_inst && cd && cd->name) {
        struct XrType *named_this = xr_type_new_named_instance(l->isolate, cd->name);
        if (named_this)
            this_type = named_this;
    }

    /* Instance methods: 'this' is param 0 */
    int base = 0;
    if (is_inst) {
        XiValue *th = xi_param(ml.func, entry, 0, this_type);
        ml.func->params[0] = th;
        xi_lower_braun_write(&ml, xi_lower_var_create(&ml, 0, "this", this_type), entry, th);
        base = 1;
    }

    /* User-declared parameters. m->param_types is XrTypeRef** (AST
     * syntax), not XrType** — resolve each entry through the analyzer
     * resolver so XiValue->type carries a real runtime type. */
    for (int i = 0; i < m->param_count; i++) {
        struct XrType *pt = ml.type_any;
        if (m->param_types && m->param_types[i]) {
            struct XrType *resolved = xr_tref_resolve(l->isolate, m->param_types[i]);
            if (resolved)
                pt = resolved;
        }
        XiValue *p = xi_param(ml.func, entry, (uint16_t) (base + i), pt);
        ml.func->params[base + i] = p;
        XR_DCHECK(m->parameters != NULL && m->parameters[i] != NULL,
                  "method param name must not be NULL");
        xi_lower_braun_write(&ml, xi_lower_var_create(&ml, 0, m->parameters[i], pt), entry, p);
    }

    /* For constructors: emit field default init for complex expressions.
     * Simple literals (int/float/bool/string) are handled by the VM via
     * field_default_values on the class descriptor; complex expressions
     * (array, map, json, new-expr, etc.) must be lowered as IR. */
    bool is_ctor = m->is_constructor || (m->name && strcmp(m->name, "constructor") == 0);

    /* Non-constructor instance methods receive `this` BORROWED: the caller
     * retains ownership and does not dup before the call, so xi_arc must not
     * drop param 0 here. Constructors own their freshly-allocated `this` and
     * move it out via the auto-return below, so they are NOT borrowed. */
    ml.func->receiver_borrowed = (is_inst && !is_ctor);

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

    xi_lower_cleanup(&ml);
    return ml.func;
}

/* Lower AST_CLASS_DECL: compile methods as child XiFuncs,
 * emit XI_CLASS_CREATE carrying XiClassData for the emitter. */
XR_FUNC void xi_lower_class_decl(XiLower *l, AstNode *node) {
    ClassDeclNode *cd = &node->as.class_decl;
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

        XiFunc *mf = lower_method_as_func(l, m, true, cd);
        if (!mf)
            continue;
        func_add_child(l->func, mf);
        if (cidx)
            cidx[ci] = (uint16_t) (l->func->nchildren - 1);
        ci++;
    }

    if (synth_ctor) {
        MethodDeclNode synth = {0};
        synth.name = "constructor";
        synth.is_constructor = true;

        XiFunc *mf = lower_method_as_func(l, &synth, true, cd);
        if (mf) {
            func_add_child(l->func, mf);
            if (cidx)
                cidx[ci] = (uint16_t) (l->func->nchildren - 1);
            ci++;
            emitted_synth_ctor = true;
        }
    }

    for (int i = 0; i < cd->method_count; i++) {
        if (cd->methods[i]->type != AST_METHOD_DECL)
            continue;
        MethodDeclNode *m = &cd->methods[i]->as.method_decl;
        if (m->is_static_constructor || !m->is_static)
            continue;

        XiFunc *mf = lower_method_as_func(l, m, false, cd);
        if (!mf)
            continue;
        func_add_child(l->func, mf);
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
        XiFunc *cf = lower_method_as_func(l, m, false, cd);
        if (cf) {
            func_add_child(l->func, cf);
            clinit_idx = (int) (l->func->nchildren - 1);
        }
        break;
    }

    XiClassData *data = (XiClassData *) xi_func_arena_alloc(l->func, sizeof(XiClassData));
    XR_DCHECK(data != NULL, "class data alloc failed");
    data->ast = node;
    data->class_name = arena_strdup(l->func, cd->name);
    data->super_name = arena_strdup(l->func, cd->super_name);
    data->generic_origin_name = arena_strdup(l->func, cd->generic_origin_name);
    data->display_name = arena_strdup(l->func, cd->display_name);
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
            if (links && links->class_info && links->class_info->struct_layout)
                data->struct_layout = links->class_info->struct_layout;
            if (links && links->class_info && !links->class_info->struct_layout)
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
                data->methods[mi].is_constructor =
                    m->is_constructor || (m->name && strcmp(m->name, "constructor") == 0);
                data->methods[mi].is_static = false;
                data->methods[mi].is_static_constructor = false;
                mi++;
            }
            if (synth_ctor && mi < total) {
                data->methods[mi].name = arena_strdup(l->func, "constructor");
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
        if (slot >= 0 && slot < XI_LOWER_MAX_VARS)
            l->shared_slot_classes[slot] = data;
    }
}
