/* ========== Multi-module API ========== */

XR_FUNC void xi_cgen_emit_str_literal_defs(XiCgenCtx *ctx, FILE *out) {
    XR_DCHECK(ctx != NULL, "xi_cgen_emit_str_literal_defs: NULL ctx");
    XR_DCHECK(out != NULL, "xi_cgen_emit_str_literal_defs: NULL output");
    if (ctx->nstrlit == 0)
        return;
    for (int i = 0; i < ctx->nstrlit; i++) {
        const CgStrLit *lit = ctx->strlit_list[i];
        /* Hash precomputed with the runtime's own primitive (xrt_hash.h):
         * literal strings never pay a lazy hash at runtime. */
        fprintf(
            out,
            "static const xrt_str_t _xstr_%d = {INT64_C(%zu), 0x%08xu, XRT_STR_LITERAL, (char *) ",
            lit->id, lit->len, xrt_str_hash_bytes(lit->str, lit->len));
        emit_c_string_literal(out, lit->str);
        fprintf(out, "};\n");
    }
    fprintf(out, "\n");
}

typedef struct CgBuiltinInitPlan {
    bool process;
    bool file;
    bool dir;
    uint32_t runtime_caps;
} CgBuiltinInitPlan;

static const char *cg_entry_source_path(XiCgenCtx *ctx, XiModule **modules, int n,
                                        int entry_index) {
    if (modules && entry_index >= 0 && entry_index < n && modules[entry_index]) {
        XiModule *entry = modules[entry_index];
        if (entry->path && entry->path[0])
            return entry->path;
        if (entry->name && entry->name[0])
            return entry->name;
    }
    return cg_current_source_path(ctx);
}

static const char *cg_source_dir_bounds(const char *file, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!file || !file[0])
        return NULL;
    const char *last_slash = strrchr(file, '/');
    if (!last_slash)
        return NULL;
    size_t len = (size_t) (last_slash - file);
    if (len == 0)
        len = 1;
    if (out_len)
        *out_len = len;
    return file;
}

static void emit_c_string_literal_n(FILE *out, const char *s, size_t len) {
    fputc('"', out);
    if (s) {
        for (size_t i = 0; i < len; i++) {
            char ch = s[i];
            if (ch == '"')
                fprintf(out, "\\\"");
            else if (ch == '\\')
                fprintf(out, "\\\\");
            else if (ch == '\n')
                fprintf(out, "\\n");
            else if (ch == '\t')
                fprintf(out, "\\t");
            else
                fputc(ch, out);
        }
    }
    fputc('"', out);
}

static void emit_optional_c_string_literal(FILE *out, const char *s) {
    if (s && s[0])
        emit_c_string_literal(out, s);
    else
        fprintf(out, "NULL");
}

static void emit_optional_source_dir_literal(FILE *out, const char *source_path) {
    size_t dir_len = 0;
    const char *dir = cg_source_dir_bounds(source_path, &dir_len);
    if (dir && dir_len > 0)
        emit_c_string_literal_n(out, dir, dir_len);
    else
        fprintf(out, "NULL");
}

static void cg_builtin_init_scan_value(CgBuiltinInitPlan *plan, const XiValue *v) {
    if (!plan || !v)
        return;

    switch (v->op) {
        case XI_YIELD:
        case XI_GO:
        case XI_AWAIT:
            plan->runtime_caps |= XR_AOT_CAP_CORO;
            break;
        case XI_SCOPE_ENTER:
        case XI_SCOPE_EXIT:
            plan->runtime_caps |= XR_AOT_CAP_CORO | XR_AOT_CAP_TRANSFER;
            break;
        case XI_CHAN_NEW:
        case XI_CHAN_SEND:
        case XI_CHAN_RECV:
        case XI_CHAN_TRY_SEND:
        case XI_CHAN_TRY_RECV:
        case XI_CHAN_RECV_STATUS:
        case XI_CHAN_IS_CLOSED:
        case XI_SELECT_BLOCK:
            plan->runtime_caps |= XR_AOT_CAP_CORO | XR_AOT_CAP_CHANNEL;
            break;
        case XI_TIME_AFTER:
        case XI_CHAN_TIMER_DISPOSE:
            plan->runtime_caps |= XR_AOT_CAP_CORO | XR_AOT_CAP_CHANNEL | XR_AOT_CAP_TIMER;
            break;
        case XI_GET_BUILTIN:
            if (v->aux_int == XR_GLOBAL_VAR_PROCESS) {
                plan->process = true;
            } else if (v->aux_int == XR_GLOBAL_VAR_FILE) {
                plan->file = true;
            } else if (v->aux_int == XR_GLOBAL_VAR_DIR) {
                plan->dir = true;
            } else if (v->aux_int == XR_GLOBAL_VAR_WORKQUEUE) {
                plan->runtime_caps |= XR_AOT_CAP_WORK_QUEUE;
            } else if (v->aux_int == XR_GLOBAL_VAR_RESULTGROUP) {
                plan->runtime_caps |= XR_AOT_CAP_RESULT_GROUP;
            }
            break;
        default:
            break;
    }
}

static void cg_builtin_init_scan_func(CgBuiltinInitPlan *plan, const XiFunc *func) {
    if (!plan || !func)
        return;
    for (uint16_t ci = 0; ci < func->nchildren; ci++)
        cg_builtin_init_scan_func(plan, func->children[ci]);
    for (uint16_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint16_t vi = 0; vi < blk->nvalues; vi++)
            cg_builtin_init_scan_value(plan, blk->values[vi]);
    }
}

static CgBuiltinInitPlan cg_builtin_init_plan_for_modules(XiModule **modules, int n) {
    CgBuiltinInitPlan plan = {0};
    for (int i = 0; i < n; i++) {
        if (modules[i])
            cg_builtin_init_scan_func(&plan, modules[i]->init);
    }
    return plan;
}

static CgBuiltinInitPlan cg_builtin_init_plan_for_func(const XiFunc *func) {
    CgBuiltinInitPlan plan = {0};
    cg_builtin_init_scan_func(&plan, func);
    return plan;
}

/* Runtime include block shared by every generated translation unit.  The
 * runtime headers are stb-style: defining XRT_IMPL emits the single definition
 * of every runtime global; without it they are extern declarations.  Exactly
 * one object per program must define XRT_IMPL (the entry unit), or globals such
 * as xrt_bump_enabled collide at link time. */
static void cg_emit_tu_includes(FILE *out, bool define_impl) {
    fprintf(out, "/* Generated by xi_cgen - do not edit */\n");
    if (define_impl) {
        fprintf(out, "#ifndef XRT_IMPL\n#define XRT_IMPL\n#endif\n");
    } else {
        /* The link manifest defines XRT_IMPL on the command line for every
         * object; a non-entry unit cancels it so the stb-style runtime headers
         * emit extern declarations here and the single definition stays in the
         * entry unit. */
        fprintf(out, "#undef XRT_IMPL\n");
    }
    fprintf(out, "#include <math.h>\n");
    fprintf(out, "#include \"xrt.h\"\n\n");
    fprintf(out, "#include \"xaot_coro.h\"\n\n");
}

XR_FUNC void xi_cgen_header(FILE *out) {
    XR_DCHECK(out != NULL, "xi_cgen_header: NULL output");
    cg_emit_tu_includes(out, true);
    fprintf(out, "static XrAotContext xrt_global_ctx;\n");
    fprintf(out, "static XrValue xrt_builtins[%d];\n\n", XR_USER_GLOBALS_START);
}

static void emit_xrt_builtin_init(FILE *out, const CgBuiltinInitPlan *plan,
                                  const char *source_path) {
    if (plan && plan->process) {
        fprintf(out, "    xrt_builtins[%d] = xrt_process_new(", XR_GLOBAL_VAR_PROCESS);
        emit_optional_c_string_literal(out, source_path);
        fprintf(out, ", argc > 1 ? argc - 1 : 0, argc > 1 ? argv + 1 : NULL, ");
        emit_optional_source_dir_literal(out, source_path);
        fprintf(out, ");\n");
    }
    if (plan && plan->file) {
        fprintf(out, "    xrt_builtins[%d] = ", XR_GLOBAL_VAR_FILE);
        if (source_path && source_path[0]) {
            fprintf(out, "xr_box_str(");
            emit_c_string_literal(out, source_path);
            fprintf(out, ")");
        } else {
            fprintf(out, "XR_NULL_VAL");
        }
        fprintf(out, ";\n");
    }
    if (plan && plan->dir) {
        fprintf(out, "    xrt_builtins[%d] = ", XR_GLOBAL_VAR_DIR);
        size_t dir_len = 0;
        const char *dir = cg_source_dir_bounds(source_path, &dir_len);
        if (dir && dir_len > 0) {
            fprintf(out, "xr_box_str(");
            emit_c_string_literal_n(out, dir, dir_len);
            fprintf(out, ")");
        } else {
            fprintf(out, "XR_NULL_VAL");
        }
        fprintf(out, ";\n");
    }
}

static void emit_xrt_runtime_caps_expr(FILE *out, uint32_t caps) {
    struct CapName {
        uint32_t bit;
        const char *name;
    };
    static const struct CapName cap_names[] = {
        {XR_AOT_CAP_CORO, "XR_AOT_CAP_CORO"},
        {XR_AOT_CAP_TIMER, "XR_AOT_CAP_TIMER"},
        {XR_AOT_CAP_CHANNEL, "XR_AOT_CAP_CHANNEL"},
        {XR_AOT_CAP_WORK_QUEUE, "XR_AOT_CAP_WORK_QUEUE"},
        {XR_AOT_CAP_RESULT_GROUP, "XR_AOT_CAP_RESULT_GROUP"},
        {XR_AOT_CAP_PROCESS, "XR_AOT_CAP_PROCESS"},
        {XR_AOT_CAP_TRANSFER, "XR_AOT_CAP_TRANSFER"},
    };

    if (caps == XR_AOT_CAP_NONE) {
        fprintf(out, "XR_AOT_CAP_NONE");
        return;
    }

    bool first = true;
    for (uint32_t i = 0; i < (uint32_t) (sizeof(cap_names) / sizeof(cap_names[0])); i++) {
        if ((caps & cap_names[i].bit) == 0)
            continue;
        if (!first)
            fprintf(out, " | ");
        fprintf(out, "%s", cap_names[i].name);
        first = false;
    }
}

static void emit_xrt_runtime_builtin_sync(FILE *out, const CgBuiltinInitPlan *plan,
                                          const char *runtime_var) {
    if (!plan)
        return;
    if (plan->process) {
        fprintf(out, "    xr_aot_runtime_set_builtin(%s, %d, xrt_builtins[%d]);\n", runtime_var,
                XR_GLOBAL_VAR_PROCESS, XR_GLOBAL_VAR_PROCESS);
    }
}

static void emit_xrt_runtime_init(FILE *out, const CgBuiltinInitPlan *plan, uint32_t runtime_caps,
                                  const char *source_path) {
    fprintf(out, "    XrAotRuntimeConfig runtime_cfg;\n");
    fprintf(out, "    xr_aot_runtime_config_init(&runtime_cfg);\n");
    fprintf(out, "    runtime_cfg.caps = ");
    emit_xrt_runtime_caps_expr(out, runtime_caps);
    fprintf(out, ";\n");
    fprintf(out, "    runtime_cfg.argc = argc > 1 ? argc - 1 : 0;\n");
    fprintf(out, "    runtime_cfg.argv = argc > 1 ? argv + 1 : NULL;\n");
    fprintf(out, "    runtime_cfg.file = ");
    emit_optional_c_string_literal(out, source_path);
    fprintf(out, ";\n");
    fprintf(out, "    XrAotRuntime *rt = xr_aot_runtime_new(&runtime_cfg);\n");
    fprintf(out, "    if (!rt) { xrt_bump_destroy(); return 1; }\n");
    if ((runtime_caps & XR_AOT_CAP_TRANSFER) != 0)
        fprintf(out, "    xr_aot_runtime_enable_transfer(rt);\n");
    emit_xrt_runtime_builtin_sync(out, plan, "rt");
    fprintf(out, "    xrt_global_ctx.runtime = rt;\n");
    fprintf(out, "    xrt_global_ctx.coro = NULL;\n");
    fprintf(out, "    xrt_global_ctx.vm_host_ops = NULL;\n");
    fprintf(out, "    xrt_global_ctx.vm_host = NULL;\n");
    fprintf(out, "    xrt_global_ctx.worker = NULL;\n");
}

XR_FUNC void xi_cgen_main(XiCgenCtx *ctx, FILE *out, XiModule **modules, int n, int entry_index) {
    XR_DCHECK(ctx != NULL, "xi_cgen_main: NULL ctx");
    XR_DCHECK(out != NULL, "xi_cgen_main: NULL output");
    XR_DCHECK(n > 0, "xi_cgen_main: no modules");
    XR_DCHECK(entry_index >= 0 && entry_index < n, "xi_cgen_main: bad entry_index");

    bool entry_is_coro = modules[entry_index] && modules[entry_index]->init &&
                         cg_func_needs_aot_coro_ctx(ctx, modules[entry_index]->init);
    CgBuiltinInitPlan builtin_plan = cg_builtin_init_plan_for_modules(modules, n);
    uint32_t runtime_caps = builtin_plan.runtime_caps;
    if (entry_is_coro)
        runtime_caps |= XR_AOT_CAP_CORO;
    bool entry_needs_runtime = runtime_caps != XR_AOT_CAP_NONE;
    const char *entry_source_path = cg_entry_source_path(ctx, modules, n, entry_index);

    fprintf(out, "int main(int argc, char **argv) {\n");
    fprintf(out, "    xrt_arc_init();\n");
    emit_xrt_builtin_init(out, &builtin_plan, entry_source_path);
    if (entry_needs_runtime) {
        emit_xrt_runtime_init(out, &builtin_plan, runtime_caps, entry_source_path);
    } else {
        fprintf(out, "    (void) argc;\n");
        fprintf(out, "    (void) argv;\n");
    }
    for (int m = 0; m < n; m++) {
        if (!modules[m] || !modules[m]->init)
            continue;
        if (m == entry_index && entry_is_coro) {
            fprintf(out, "    void *_entry_frame = ");
            emit_fname_suffix(ctx, out, modules[m]->name ? modules[m]->name : "mod",
                              modules[m]->init, "_aot_frame_new");
            fprintf(out, "();\n");
            fprintf(out, "    xr_aot_run_main(rt, &");
            emit_fname_suffix(ctx, out, modules[m]->name ? modules[m]->name : "mod",
                              modules[m]->init, "_aot_desc");
            fprintf(out, ", _entry_frame);\n");
        } else {
            if (cg_func_needs_aot_coro_ctx(ctx, modules[m]->init)) {
                fprintf(stderr,
                        "[xi_cgen] ERROR: suspendable AOT dependency module init '%s' must "
                        "be the entry module\n",
                        modules[m]->name ? modules[m]->name : "mod");
                ctx->error = true;
                fprintf(out, "    return 1;\n");
                continue;
            }
            fprintf(out, "    ");
            emit_fname(ctx, out, modules[m]->name ? modules[m]->name : "mod", modules[m]->init);
            fprintf(out, "(NULL);\n");
        }
    }
    if (entry_needs_runtime) {
        fprintf(out, "    xr_aot_runtime_delete(rt);\n");
    }
    fprintf(out, "    xrt_bump_destroy();\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
}

XR_FUNC void xi_cgen_program(XiCgenCtx *ctx, FILE *out, XiModule *module) {
    XR_DCHECK(ctx != NULL, "xi_cgen_program: NULL ctx");
    XR_DCHECK(out != NULL, "xi_cgen_program: NULL output");
    XR_DCHECK(module != NULL, "xi_cgen_program: NULL module");
    XR_DCHECK(module->init != NULL, "xi_cgen_program: NULL init func");

    XiFunc *main_func = module->init;
    const char *prefix = module->name ? module->name : "mod";

    cg_prepare_func_tree_for_cgen(main_func);

    /* Reset function name counter for each compilation unit */
    ctx->fname_counter = 0;

    XiModule *single_module[1] = {module};
    ctx->all_modules = single_module;
    ctx->all_nmodules = 1;
    ctx->nshared_native_exports = 0;
    if (ctx->shared_native_exports)
        memset(ctx->shared_native_exports, 0,
               (size_t) ctx->shared_native_exports_cap * sizeof(CgSharedNativeExport));
    cg_prepare_sync_go_targets_for_modules(ctx, single_module, 1);

    /* Initialize from module metadata */
    cg_init_from_module(ctx, module);
    ctx->shared_name = "xrt_shared";
    cg_collect_shared_native_instances(ctx);

    xi_cgen_header(out);

    /* Bodies are buffered so interned string literal definitions (only
     * known after emission) can be placed ahead of every use. */
    char *bodybuf = NULL;
    size_t bodysz = 0;
    FILE *body = xr_open_memstream(&bodybuf, &bodysz);
    if (!body) {
        ctx->error = true;
        return;
    }

    if (main_func->nshared > 0)
        fprintf(body, "static XrValue xrt_shared[%u];\n", main_func->nshared);

    fprintf(body, "\n");
    emit_class_native_typedefs(ctx, body, module, prefix);
    emit_class_shared_native_storage_decls(ctx, body, prefix);
    emit_struct_native_typedefs(body, main_func, prefix);

    emit_forward_decls(ctx, body, main_func, prefix);
    fprintf(body, "\n");

    xi_cgen_func(ctx, body, main_func, prefix);

    if (ctx->emit_main) {
        fprintf(body, "int main(int argc, char **argv) {\n");
        fprintf(body, "    xrt_arc_init();\n");
        CgBuiltinInitPlan builtin_plan = cg_builtin_init_plan_for_func(main_func);
        const char *entry_source_path = cg_entry_source_path(ctx, single_module, 1, 0);
        emit_xrt_builtin_init(body, &builtin_plan, entry_source_path);
        bool entry_is_coro = cg_func_needs_aot_coro_ctx(ctx, main_func);
        uint32_t runtime_caps = builtin_plan.runtime_caps;
        if (entry_is_coro)
            runtime_caps |= XR_AOT_CAP_CORO;
        bool entry_needs_runtime = runtime_caps != XR_AOT_CAP_NONE;
        if (entry_needs_runtime) {
            emit_xrt_runtime_init(body, &builtin_plan, runtime_caps, entry_source_path);
        }
        if (entry_is_coro) {
            fprintf(body, "    void *_entry_frame = ");
            emit_fname_suffix(ctx, body, prefix, main_func, "_aot_frame_new");
            fprintf(body, "();\n");
            fprintf(body, "    xr_aot_run_main(rt, &");
            emit_fname_suffix(ctx, body, prefix, main_func, "_aot_desc");
            fprintf(body, ", _entry_frame);\n");
        } else {
            if (!entry_needs_runtime) {
                fprintf(body, "    (void) argc;\n");
                fprintf(body, "    (void) argv;\n");
            }
            fprintf(body, "    ");
            emit_fname(ctx, body, prefix, main_func);
            fprintf(body, "(NULL);\n");
        }
        if (entry_needs_runtime) {
            fprintf(body, "    xr_aot_runtime_delete(rt);\n");
        }
        fprintf(body, "    xrt_bump_destroy();\n");
        fprintf(body, "    return 0;\n");
        fprintf(body, "}\n");
    }

    if (xr_close_memstream(body, &bodybuf, &bodysz) != 0) {
        ctx->error = true;
        return;
    }
    xi_cgen_emit_str_literal_defs(ctx, out);
    fwrite(bodybuf, 1, bodysz, out);
    xr_free(bodybuf);
}

/* Emit one self-contained translation unit for modules[mod_index], suitable
 * for independent compilation into its own object file (114 separate
 * compilation).  Cross-module symbols use external linkage: every module's
 * functions are forward-declared (extern) and its shared-slot array declared,
 * with this module supplying the matching definitions; xrt_builtins is defined
 * only in the entry unit.  main() is emitted into the entry unit.  A single
 * XiCgenCtx is reused across every unit of a bundle so function ids stay
 * globally consistent; the per-unit string pool is reset so each object only
 * carries its own literals. */
XR_FUNC void xi_cgen_module_tu(XiCgenCtx *ctx, FILE *out, XiModule **modules, int nmodules,
                               int mod_index, int entry_index) {
    XR_DCHECK(ctx != NULL, "xi_cgen_module_tu: NULL ctx");
    XR_DCHECK(out != NULL, "xi_cgen_module_tu: NULL output");
    XR_DCHECK(modules != NULL, "xi_cgen_module_tu: NULL modules");
    XR_DCHECK(mod_index >= 0 && mod_index < nmodules, "xi_cgen_module_tu: bad mod_index");
    XR_DCHECK(entry_index >= 0 && entry_index < nmodules, "xi_cgen_module_tu: bad entry_index");

    XiModule *module = modules[mod_index];
    XR_DCHECK(module != NULL && module->init != NULL, "xi_cgen_module_tu: NULL module/init");
    const char *prefix = module->name ? module->name : "mod";
    bool is_entry = (mod_index == entry_index);

    ctx->all_modules = modules;
    ctx->all_nmodules = nmodules;
    ctx->extern_linkage = true;
    /* Internal (non-exported) functions take a per-module ordinal suffix, so
     * adding or removing a function in one module never renumbers another's
     * symbols.  Cross-module-visible functions use order-independent names
     * (see emit_fname), so the counter is reset per translation unit. */
    ctx->fname_counter = 0;

    /* Every unit references cross-module symbols by name, so all module IR must
     * be lowered before any unit is emitted.  cg_prepare_func_tree_for_cgen is
     * idempotent (stage-gated), so repeating it across units is cheap. */
    for (int i = 0; i < nmodules; i++) {
        if (modules[i] && modules[i]->init)
            cg_prepare_func_tree_for_cgen(modules[i]->init);
    }

    /* This module's definitions.  Reset the literal pool so this object carries
     * only its own _xstr_* defs; buffer the body so those defs precede uses and
     * the cross-module references it uses can be collected for forward
     * declaration. */
    cg_reset_str_lits(ctx);
    cg_init_from_module(ctx, module);
    cg_register_imported_classes(ctx);
    char shared_buf[256];
    snprintf(shared_buf, sizeof(shared_buf), "xrt_shared_%s", prefix);
    ctx->shared_name = shared_buf;
    cg_collect_shared_native_instances(ctx);

    char *bodybuf = NULL;
    size_t bodysz = 0;
    FILE *body = xr_open_memstream(&bodybuf, &bodysz);
    if (!body) {
        ctx->error = true;
        ctx->shared_name = "xrt_shared";
        ctx->extern_linkage = false;
        return;
    }

    ctx->n_xmod_refs = 0;
    ctx->collect_xmod_refs = true;
    xi_cgen_func(ctx, body, module->init, prefix);

    if (is_entry && ctx->emit_main)
        xi_cgen_main(ctx, body, modules, nmodules, entry_index);
    ctx->collect_xmod_refs = false;

    if (xr_close_memstream(body, &bodybuf, &bodysz) != 0) {
        ctx->error = true;
        ctx->shared_name = "xrt_shared";
        ctx->extern_linkage = false;
        return;
    }

    /* Shared includes; the entry unit defines the runtime impl (XRT_IMPL) and
     * xrt_builtins, every other unit only declares them. */
    cg_emit_tu_includes(out, is_entry);
    fprintf(out, "%sXrAotContext xrt_global_ctx;\n", is_entry ? "" : "extern ");
    fprintf(out, "%sXrValue xrt_builtins[%d];\n\n", is_entry ? "" : "extern ",
            XR_USER_GLOBALS_START);

    /* Define this module's shared-slot array.  Other modules' arrays are
     * declared without a bound so this object stays byte-identical when an
     * unrelated module gains or loses a shared slot (114 incremental caching);
     * the defining unit still carries the sized definition. */
    if (module->init->nshared > 0)
        fprintf(out, "XrValue %s[%u];\n", shared_buf, module->init->nshared);
    for (int i = 0; i < nmodules; i++) {
        XiModule *m = modules[i];
        if (i == mod_index || !m || !m->init || m->init->nshared == 0)
            continue;
        fprintf(out, "extern XrValue xrt_shared_%s[];\n", m->name ? m->name : "mod");
    }
    fprintf(out, "\n");

    /* Native class / struct typedefs must precede the forward declarations and
     * bodies that use them.  Imported (and cross-module base) class typedefs
     * come first so a locally-defined class can embed an imported base; then
     * this module's own classes. */
    emit_imported_class_native_typedefs(ctx, out);
    emit_class_native_typedefs(ctx, out, module, prefix);
    emit_class_shared_native_storage_decls(ctx, out, prefix);
    emit_imported_class_shared_native_storage_decls(ctx, out);
    emit_struct_native_typedefs(out, module->init, prefix);
    fprintf(out, "\n");

    /* Forward declarations: this module's own functions in full, plus only the
     * cross-module symbols this unit actually references. */
    emit_forward_decls(ctx, out, module->init, prefix);
    for (int i = 0; i < ctx->n_xmod_refs; i++)
        emit_one_forward_decl(ctx, out, ctx->xmod_ref_funcs[i], ctx->xmod_ref_prefixes[i]);
    fprintf(out, "\n");

    xi_cgen_emit_str_literal_defs(ctx, out);
    fwrite(bodybuf, 1, bodysz, out);
    xr_free(bodybuf);

    ctx->shared_name = "xrt_shared";
    ctx->extern_linkage = false;
}
