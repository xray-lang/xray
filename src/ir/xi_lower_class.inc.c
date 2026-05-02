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

/* Lower a class method body to a child XiFunc.
 * Instance methods get an implicit 'this' parameter at index 0. */
static XiFunc *lower_method_as_func(XiLower *l, MethodDeclNode *m,
                                     bool is_inst) {
    XiLower ml;
    xi_lower_init(&ml, l->analyzer, l->isolate);
    ml.parent = l;

    ml.func = xi_func_new(m->name, m->return_type ? m->return_type : ml.type_void);
    if (!ml.func) { xi_lower_cleanup(&ml); return NULL; }
    ml.func->analyzer = l->analyzer;

    XiBlock *entry = xi_block_new(ml.func);
    entry->sealed = true;
    ml.cur_block = entry;

    int np = m->param_count + (is_inst ? 1 : 0);
    ml.func->nparams = (uint16_t)np;
    if (np > 0) {
        ml.func->params = (XiValue **)xr_calloc(np, sizeof(XiValue *));
        if (!ml.func->params) {
            xi_func_free(ml.func);
            xi_lower_cleanup(&ml);
            return NULL;
        }
    }

    /* Instance methods: 'this' is param 0 */
    int base = 0;
    if (is_inst) {
        XiValue *th = xi_param(ml.func, entry, 0, ml.type_any);
        ml.func->params[0] = th;
        xi_lower_braun_write(&ml,
                    xi_lower_var_create(&ml, "this", ml.type_any),
                    entry, th);
        base = 1;
    }

    /* User-declared parameters */
    for (int i = 0; i < m->param_count; i++) {
        struct XrType *pt = (m->param_types && m->param_types[i])
                            ? m->param_types[i] : ml.type_any;
        XiValue *p = xi_param(ml.func, entry, (uint16_t)(base + i), pt);
        ml.func->params[base + i] = p;
        XR_DCHECK(m->parameters != NULL && m->parameters[i] != NULL,
                  "method param name must not be NULL");
        xi_lower_braun_write(&ml,
                    xi_lower_var_create(&ml, m->parameters[i], pt),
                    entry, p);
    }

    if (m->body) xi_lower_stmt(&ml, m->body);

    /* Constructors auto-return 'this' (param 0) so the caller gets
     * the freshly-created instance, matching the legacy codegen
     * convention (xemit_return(emitter, 0, 1) at end of constructor). */
    if (ml.cur_block) {
        bool is_ctor = m->is_constructor
                       || (m->name && strcmp(m->name, "constructor") == 0);
        if (is_ctor && is_inst) {
            int this_var = xi_lower_var_create(&ml, "this", ml.type_any);
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
static void lower_class_decl(XiLower *l, AstNode *node) {
    ClassDeclNode *cd = &node->as.class_decl;
    XR_DCHECK(cd->name != NULL, "class name must not be NULL");

    /* Count instance / static methods (skip static constructors) */
    uint16_t inst_n = 0, stat_n = 0;
    for (int i = 0; i < cd->method_count; i++) {
        if (cd->methods[i]->type != AST_METHOD_DECL) continue;
        MethodDeclNode *m = &cd->methods[i]->as.method_decl;
        if (m->is_static_constructor) continue;
        if (m->is_static) stat_n++; else inst_n++;
    }

    /* Lower each method body to a child XiFunc, recording child indices */
    uint16_t total = inst_n + stat_n;
    uint16_t *cidx = total
        ? (uint16_t *)xi_func_arena_alloc(l->func,
                                           total * sizeof(uint16_t))
        : NULL;
    uint16_t ci = 0;
    for (int i = 0; i < cd->method_count; i++) {
        if (cd->methods[i]->type != AST_METHOD_DECL) continue;
        MethodDeclNode *m = &cd->methods[i]->as.method_decl;
        if (m->is_static_constructor) continue;

        XiFunc *mf = lower_method_as_func(l, m, !m->is_static);
        if (!mf) continue;
        func_add_child(l->func, mf);
        if (cidx) cidx[ci] = (uint16_t)(l->func->nchildren - 1);
        ci++;
    }

    /* Create XI_CLASS_CREATE value with XiClassData metadata */
    XiValue *v = xi_value_new(l->func, l->cur_block,
                               XI_CLASS_CREATE, l->type_any, 0);
    if (!v) return;

    XiClassData *data = (XiClassData *)xi_func_arena_alloc(
        l->func, sizeof(XiClassData));
    XR_DCHECK(data != NULL, "class data alloc failed");
    data->ast = node;
    data->class_name = arena_strdup(l->func, cd->name);
    data->super_name = arena_strdup(l->func, cd->super_name);
    data->child_idx = cidx;
    data->ninst = inst_n;
    data->nstat = stat_n;

    /* Build arena-safe method descriptor array so cgen can resolve
     * class methods without depending on AST after lowering. */
    data->nmethod = total;
    data->methods = NULL;
    if (total > 0) {
        data->methods = (XiClassMethod *)xi_func_arena_alloc(
            l->func, total * sizeof(XiClassMethod));
        if (data->methods) {
            uint16_t mi = 0;
            for (int i = 0; i < cd->method_count && mi < total; i++) {
                if (cd->methods[i]->type != AST_METHOD_DECL) continue;
                MethodDeclNode *m = &cd->methods[i]->as.method_decl;
                if (m->is_static_constructor) continue;
                data->methods[mi].name = arena_strdup(l->func, m->name);
                data->methods[mi].is_constructor =
                    m->is_constructor ||
                    (m->name && strcmp(m->name, "constructor") == 0);
                data->methods[mi].is_static = m->is_static;
                data->methods[mi].is_static_constructor = false;
                mi++;
            }
        }
    }
    v->aux = data;
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t)node->line;

    /* Bind class to its name in SSA */
    int var_id = xi_lower_var_create(l, cd->name, l->type_any);
    xi_lower_braun_write(l, var_id, l->cur_block, v);

    /* Top-level classes: also store into shared array for cross-scope access */
    if (l->is_program && var_id < l->var_count
        && l->shared_map[var_id] >= 0) {
        XiValue *st = xi_value_new(l->func, l->cur_block,
                                    XI_SET_SHARED, l->type_void, 1);
        if (st) {
            st->args[0] = v;
            st->aux_int = l->shared_map[var_id];
            st->flags |= XI_FLAG_SIDE_EFFECT;
        }
    }
}
