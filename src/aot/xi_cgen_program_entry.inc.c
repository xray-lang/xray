/* ========== Multi-module API ========== */

XR_FUNC void xi_cgen_emit_str_literal_defs(XiCgenCtx *ctx, FILE *out) {
    XR_DCHECK(ctx != NULL, "xi_cgen_emit_str_literal_defs: NULL ctx");
    XR_DCHECK(out != NULL, "xi_cgen_emit_str_literal_defs: NULL output");
    for (int i = 0; i < ctx->nstrlit; i++) {
        const CgStrLit *lit = ctx->strlit_list[i];
        /* Hash precomputed with the runtime's shared primitive:
         * literal strings never pay a lazy hash at runtime. */
        fprintf(out,
                "static const xrt_str_t _xstr_%d = {INT64_C(%zu), INT64_C(%zu), 0x%08xu, "
                "XRT_STR_LITERAL, (char *) ",
                lit->id, lit->len, xr_utf8_strlen(lit->str, lit->len),
                xr_hash_core_str_hash_bytes(lit->str, lit->len));
        emit_c_string_literal_bytes(out, lit->str, lit->len);
        fprintf(out, "};\n");
    }
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    if (bundle) {
        for (uint32_t i = 0; i < bundle->nfixed_bytes_blobs; i++) {
            const XaotFixedBytesBlob *blob = &bundle->fixed_bytes_blobs[i];
            fprintf(out, "static const char _xbytes_%u", i + 1);
            if (blob->length == 0) {
                fprintf(out, "[1] = {0};\n");
                continue;
            }
            fprintf(out, "[] = ");
            emit_c_string_literal_bytes(out, (const char *) blob->data, blob->length);
            fprintf(out, ";\n");
        }
    }
    if (ctx->nstrlit > 0 || (bundle && bundle->nfixed_bytes_blobs > 0))
        fprintf(out, "\n");
}

typedef struct CgBuiltinInitPlan {
    bool process;
    bool file;
    bool dir;
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
    emit_c_string_literal_bytes(out, s, len);
}

static void cg_emit_global_asm(FILE *out, const XiModule *module) {
    if (!out || !module || !module->global_asm_templates || module->nglobal_asm == 0)
        return;
    for (uint16_t i = 0; i < module->nglobal_asm; i++) {
        fprintf(out, "__asm__(");
        emit_c_string_literal(out, module->global_asm_templates[i] ? module->global_asm_templates[i]
                                                                   : "");
        fprintf(out, ");\n");
    }
    fprintf(out, "\n");
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

static void cg_builtin_value_plan_visit(CgBuiltinInitPlan *plan, const XiValue *v) {
    if (!plan || !v)
        return;

    switch (v->op) {
        case XI_GET_BUILTIN:
            if (v->aux_int == XR_GLOBAL_VAR_PROCESS) {
                plan->process = true;
            } else if (v->aux_int == XR_GLOBAL_VAR_FILE) {
                plan->file = true;
            } else if (v->aux_int == XR_GLOBAL_VAR_DIR) {
                plan->dir = true;
            }
            break;
        default:
            break;
    }
}

static void cg_builtin_value_plan_visit_func(CgBuiltinInitPlan *plan, const XiFunc *func) {
    if (!plan || !func)
        return;
    for (uint16_t ci = 0; ci < func->nchildren; ci++)
        cg_builtin_value_plan_visit_func(plan, func->children[ci]);
    for (uint16_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint16_t vi = 0; vi < blk->nvalues; vi++)
            cg_builtin_value_plan_visit(plan, blk->values[vi]);
    }
}

static CgBuiltinInitPlan cg_builtin_init_plan_for_modules(XiModule **modules, int n) {
    CgBuiltinInitPlan plan = {0};
    for (int i = 0; i < n; i++) {
        if (modules[i])
            cg_builtin_value_plan_visit_func(&plan, modules[i]->init);
    }
    return plan;
}

static CgBuiltinInitPlan cg_builtin_init_plan_for_func(const XiFunc *func) {
    CgBuiltinInitPlan plan = {0};
    cg_builtin_value_plan_visit_func(&plan, func);
    return plan;
}

static uint32_t cg_runtime_caps_from_entry_plan(XiCgenCtx *ctx) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    uint32_t required;
    uint32_t caps = XR_AOT_CAP_NONE;
    if (!bundle || !bundle->has_entry_plan) {
        ctx->error = true;
        return caps;
    }
    required = bundle->entry_plan.runtime_component_bits;
    if ((required & XG_CAP_COROUTINE) != 0)
        caps |= XR_AOT_CAP_CORO;
    if ((required & XG_CAP_TIMER) != 0)
        caps |= XR_AOT_CAP_TIMER;
    if ((required & XG_CAP_CHANNEL) != 0)
        caps |= XR_AOT_CAP_CHANNEL;
    if ((required & XG_CAP_WORK_QUEUE) != 0)
        caps |= XR_AOT_CAP_WORK_QUEUE;
    if ((required & XG_CAP_RESULT_GROUP) != 0)
        caps |= XR_AOT_CAP_RESULT_GROUP;
    if ((required & (XG_CAP_DEEP_COPY | XG_CAP_SCOPE)) != 0)
        caps |= XR_AOT_CAP_TRANSFER;
    if ((required & XG_CAP_TASK) != 0)
        caps |= XR_AOT_CAP_TASK;
    if ((required & XG_CAP_OBJECTS) != 0)
        caps |= XR_AOT_CAP_OBJECTS;
    if ((required & XG_CAP_ATOMIC) != 0)
        caps |= XR_AOT_CAP_ATOMIC;
    if ((required & XG_CAP_COUNTDOWN_LATCH) != 0)
        caps |= XR_AOT_CAP_COUNTDOWN_LATCH;
    if ((required & XG_CAP_SEMAPHORE) != 0)
        caps |= XR_AOT_CAP_SEMAPHORE;
    if ((required & XG_CAP_EVENT_COUNT) != 0)
        caps |= XR_AOT_CAP_EVENT_COUNT;
    return caps;
}

static bool cg_entry_uses_resumable_frame(XiCgenCtx *ctx) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    if (!bundle || !bundle->has_entry_plan) {
        ctx->error = true;
        return false;
    }
    return bundle->entry_plan.root_representation == XR_ROOT_RESUMABLE_FRAME;
}

static bool cg_entry_init_uses_resumable_frame(XiCgenCtx *ctx, XiModule **modules, int n,
                                               int entry_index) {
    if (cg_entry_uses_resumable_frame(ctx))
        return true;
    if (!modules || entry_index < 0 || entry_index >= n || !modules[entry_index] ||
        !modules[entry_index]->init)
        return false;
    return cg_func_needs_aot_coro_ctx(ctx, modules[entry_index]->init);
}

static bool cg_entry_uses_root_descriptor(XiCgenCtx *ctx) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    if (!bundle || !bundle->has_entry_plan) {
        ctx->error = true;
        return false;
    }
    return bundle->entry_plan.root_representation == XR_ROOT_DESCRIPTOR;
}

static void cg_emit_main_pending_error_return(FILE *out, bool entry_needs_runtime) {
    fprintf(out, "    if (XR_UNLIKELY(xrt_has_pending_error())) {\n");
    if (entry_needs_runtime)
        fprintf(out, "        xr_aot_runtime_delete(rt);\n");
    fprintf(out, "        xrt_bump_destroy();\n");
    fprintf(out, "        return 1;\n");
    fprintf(out, "    }\n");
}

/* Runtime include block shared by every generated translation unit.  The
 * runtime headers are stb-style: defining XRT_IMPL emits the single definition
 * of every runtime global; without it they are extern declarations.  Exactly
 * one object per program must define XRT_IMPL (the entry unit), or globals such
 * as xrt_bump_enabled collide at link time. */
static void cg_emit_tu_includes(FILE *out, bool define_impl, bool freestanding_profile,
                                uint32_t simd_features) {
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
    if (freestanding_profile) {
        fprintf(out, "#include \"xrt_core_freestanding.h\"\n\n");
    } else {
        fprintf(out, "#define XRT_THREAD_USE_PENDING_ERROR 1\n");
        fprintf(out, "#include <math.h>\n");
        fprintf(out, "#include \"xrt.h\"\n\n");
        fprintf(out, "#include \"xaot_coro.h\"\n\n");
        fprintf(out, "#include \"xrt_thread_aot.h\"\n\n");
    }
    if ((simd_features & XAOT_SIMD_FEATURE_NEON) != 0)
        fprintf(out, "#include <arm_neon.h>\n\n");
    else if ((simd_features & (XAOT_SIMD_FEATURE_SSE2 | XAOT_SIMD_FEATURE_AVX2)) != 0)
        fprintf(out, "#include <immintrin.h>\n\n");
}

XR_FUNC void xi_cgen_header(XiCgenCtx *ctx, FILE *out) {
    XR_DCHECK(out != NULL, "xi_cgen_header: NULL output");
    cg_emit_tu_includes(out, true, ctx && ctx->freestanding_profile,
                        ctx && ctx->simd_active && ctx->target ? ctx->target->simd_features : 0);
    fprintf(out, "static XrAotContext xrt_global_ctx;\n");
    fprintf(out, "static XrValue xrt_builtins[%d];\n\n", XR_USER_GLOBALS_START);
}

static void emit_xrt_builtin_init(FILE *out, const CgBuiltinInitPlan *plan, const char *source_path,
                                  const char *argc_expr, const char *argv_expr) {
    if (plan && plan->process) {
        fprintf(out, "    xrt_builtins[%d] = xrt_process_new(", XR_GLOBAL_VAR_PROCESS);
        emit_optional_c_string_literal(out, source_path);
        fprintf(out, ", %s, %s, ", argc_expr ? argc_expr : "0", argv_expr ? argv_expr : "NULL");
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
        const char *name; /* owned: static string literal (cap_names table below) */
    };
    static const struct CapName cap_names[] = {
        {XR_AOT_CAP_CORO, "XR_AOT_CAP_CORO"},
        {XR_AOT_CAP_TIMER, "XR_AOT_CAP_TIMER"},
        {XR_AOT_CAP_CHANNEL, "XR_AOT_CAP_CHANNEL"},
        {XR_AOT_CAP_WORK_QUEUE, "XR_AOT_CAP_WORK_QUEUE"},
        {XR_AOT_CAP_RESULT_GROUP, "XR_AOT_CAP_RESULT_GROUP"},
        {XR_AOT_CAP_PROCESS, "XR_AOT_CAP_PROCESS"},
        {XR_AOT_CAP_TRANSFER, "XR_AOT_CAP_TRANSFER"},
        {XR_AOT_CAP_TASK, "XR_AOT_CAP_TASK"},
        {XR_AOT_CAP_OBJECTS, "XR_AOT_CAP_OBJECTS"},
        {XR_AOT_CAP_ATOMIC, "XR_AOT_CAP_ATOMIC"},
        {XR_AOT_CAP_COUNTDOWN_LATCH, "XR_AOT_CAP_COUNTDOWN_LATCH"},
        {XR_AOT_CAP_SEMAPHORE, "XR_AOT_CAP_SEMAPHORE"},
        {XR_AOT_CAP_EVENT_COUNT, "XR_AOT_CAP_EVENT_COUNT"},
        {XR_AOT_CAP_PARALLEL, "XR_AOT_CAP_PARALLEL"},
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

static bool cg_runtime_caps_need_destroy_config(uint32_t caps) {
    return (caps & (XR_AOT_CAP_OBJECTS | XR_AOT_CAP_TASK | XR_AOT_CAP_CHANNEL |
                    XR_AOT_CAP_WORK_QUEUE | XR_AOT_CAP_RESULT_GROUP | XR_AOT_CAP_COUNTDOWN_LATCH |
                    XR_AOT_CAP_SEMAPHORE | XR_AOT_CAP_EVENT_COUNT)) != 0;
}

static void emit_xrt_runtime_core_configure_fn(FILE *out, uint32_t caps) {
    if (!cg_runtime_caps_need_destroy_config(caps))
        return;
    fprintf(out,
            "static void xrt_configure_runtime_core(struct XrRuntimeCore *core, uint32_t caps, "
            "void *userdata) {\n");
    fprintf(out, "    (void) caps;\n");
    fprintf(out, "    (void) userdata;\n");
    if ((caps & XR_AOT_CAP_OBJECTS) != 0)
        fprintf(out, "    xr_runtime_core_enable_object_destroy_ops(core);\n");
    if ((caps & XR_AOT_CAP_TASK) != 0)
        fprintf(out, "    xr_runtime_core_enable_task_destroy_ops(core);\n");
    if ((caps & XR_AOT_CAP_CHANNEL) != 0)
        fprintf(out, "    xr_runtime_core_enable_channel_destroy_ops(core);\n");
    if ((caps & XR_AOT_CAP_WORK_QUEUE) != 0)
        fprintf(out, "    xr_runtime_core_enable_work_queue_destroy_ops(core);\n");
    if ((caps & XR_AOT_CAP_RESULT_GROUP) != 0)
        fprintf(out, "    xr_runtime_core_enable_result_group_destroy_ops(core);\n");
    if ((caps & XR_AOT_CAP_COUNTDOWN_LATCH) != 0)
        fprintf(out, "    xr_runtime_core_enable_countdown_latch_destroy_ops(core);\n");
    if ((caps & XR_AOT_CAP_SEMAPHORE) != 0)
        fprintf(out, "    xr_runtime_core_enable_semaphore_destroy_ops(core);\n");
    if ((caps & XR_AOT_CAP_EVENT_COUNT) != 0)
        fprintf(out, "    xr_runtime_core_enable_event_count_destroy_ops(core);\n");
    fprintf(out, "}\n\n");
}

static void emit_xrt_runtime_value_ops(FILE *out) {
    fprintf(
        out,
        "static XrValue xrt_runtime_string_new(const char *data, size_t len) {\n"
        "    XrValue value = xrt_str_alloc(len);\n"
        "    if (len > 0 && data) memcpy(xr_str_buf(value), data, len);\n"
        "    return value;\n"
        "}\n"
        "static const char *xrt_runtime_string_data(XrValue value) {\n"
        "    return XR_IS_STR(value) ? xr_str_data(value) : NULL;\n"
        "}\n"
        "static XrValue xrt_runtime_map_new(int64_t capacity) {\n"
        "    return xrt_map_new(capacity);\n"
        "}\n"
        "static void xrt_runtime_map_set(XrValue map, XrValue key, XrValue value) {\n"
        "    if (XR_IS_MAP(map) && map.ptr) xrt_map_set((xrt_map_t *)map.ptr, key, value);\n"
        "}\n"
        "static XrValue xrt_runtime_map_get(XrValue map, XrValue key, bool *found) {\n"
        "    if (!XR_IS_MAP(map) || !map.ptr) { if (found) *found = false; return XR_NULL_VAL; }\n"
        "    xrt_map_t *m = (xrt_map_t *)map.ptr;\n"
        "    if (found) *found = xrt_map_has(m, key) != 0;\n"
        "    return xrt_map_get(m, key);\n"
        "}\n"
        "static XrValue xrt_runtime_array_new(int64_t length) {\n"
        "    return xrt_array_new(length);\n"
        "}\n"
        "#define xrt_runtime_array_append xrt_array_push\n"
        "static void xrt_runtime_array_push(XrValue array, XrValue value) {\n"
        "    if (XR_IS_ARRAY(array) && array.ptr) xrt_runtime_array_append(array, value);\n"
        "}\n"
        "static XrValue xrt_runtime_record_new(int64_t field_count, const char *const "
        "*field_names) {\n"
        "    return xrt_record_new_named(field_count, field_names);\n"
        "}\n"
        "static void xrt_runtime_record_set(XrValue record, int64_t field_index, XrValue value) {\n"
        "    xrt_json_set_field(record, (int)field_index, value);\n"
        "}\n"
        "static XrValue xrt_runtime_enum_new(const char *enum_name, const char *member_name, "
        "int64_t member_index) {\n"
        "    return xrt_enum_box_new(0, enum_name, member_name, member_index);\n"
        "}\n"
        "static int64_t xrt_runtime_enum_ordinal(XrValue value, int64_t fallback) {\n"
        "    uint32_t ordinal = 0;\n"
        "    return xrt_enum_key_parts(value, NULL, NULL, &ordinal, NULL) ? (int64_t)ordinal : "
        "fallback;\n"
        "}\n"
        "static const XrAotValueOps xrt_runtime_value_ops = {\n"
        "    .string_new = xrt_runtime_string_new,\n"
        "    .string_data = xrt_runtime_string_data,\n"
        "    .map_new = xrt_runtime_map_new,\n"
        "    .map_set = xrt_runtime_map_set,\n"
        "    .map_get = xrt_runtime_map_get,\n"
        "    .array_new = xrt_runtime_array_new,\n"
        "    .array_push = xrt_runtime_array_push,\n"
        "    .record_new = xrt_runtime_record_new,\n"
        "    .record_set = xrt_runtime_record_set,\n"
        "    .enum_new = xrt_runtime_enum_new,\n"
        "    .enum_ordinal = xrt_runtime_enum_ordinal,\n"
        "};\n\n");
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
                                  const char *source_path, bool has_value_ops) {
    fprintf(out, "    XrAotRuntimeConfig runtime_cfg;\n");
    fprintf(out, "    xr_aot_runtime_config_init(&runtime_cfg);\n");
    if (has_value_ops)
        fprintf(out, "    runtime_cfg.value_ops = &xrt_runtime_value_ops;\n");
    if (cg_runtime_caps_need_destroy_config(runtime_caps))
        fprintf(out, "    runtime_cfg.configure_core = xrt_configure_runtime_core;\n");
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

static bool cg_runtime_caps_need_runtime(uint32_t caps) {
    return (caps & ~XR_AOT_CAP_OBJECTS) != XR_AOT_CAP_NONE;
}

/* Shared-library init: --shared exports a C ABI library, so loading it executes
 * the complete module graph before any @c_export wrapper can run. This is the
 * library equivalent of main's ordered module-init sequence: both initialized
 * globals and explicit top-level side effects retain normal Xray semantics. */
static void xi_cgen_shared_lib_ctor(XiCgenCtx *ctx, FILE *out, XiModule **modules, int n,
                                    int entry_index) {
    CgBuiltinInitPlan builtin_plan = cg_builtin_init_plan_for_modules(modules, n);
    uint32_t runtime_caps = cg_runtime_caps_from_entry_plan(ctx);
    bool entry_is_coro = cg_entry_init_uses_resumable_frame(ctx, modules, n, entry_index);
    if (entry_is_coro)
        runtime_caps |= XR_AOT_CAP_CORO;
    if (entry_is_coro || cg_runtime_caps_need_runtime(runtime_caps)) {
        fprintf(out, "/* --shared: runtime-backed bundle; no load-time init emitted. */\n");
        return;
    }
    const char *entry_source_path = cg_entry_source_path(ctx, modules, n, entry_index);

    fprintf(out, "#if defined(__GNUC__) || defined(__clang__)\n");
    fprintf(out, "__attribute__((constructor))\n");
    fprintf(out, "#endif\n");
    fprintf(out, "static void xrt_shared_lib_ctor(void) {\n");
    fprintf(out, "    xrt_arc_init();\n");
    emit_xrt_builtin_init(out, &builtin_plan, entry_source_path, "0", "NULL");
    for (int m = 0; m < n; m++) {
        if (!modules[m] || !modules[m]->init)
            continue;
        if (cg_func_needs_aot_coro_ctx(ctx, modules[m]->init)) {
            fprintf(stderr,
                    "[xi_cgen] ERROR: suspendable module init '%s' is unsupported in a "
                    "shared library\n",
                    modules[m]->name ? modules[m]->name : "mod");
            ctx->error = true;
            continue;
        }
        fprintf(out, "    ");
        emit_fname(ctx, out, modules[m]->name ? modules[m]->name : "mod", modules[m]->init);
        fprintf(out, "(NULL);\n");
        if (ctx->freestanding_profile) {
            fprintf(out, "    if (XR_UNLIKELY(xrt_has_pending_error())) "
                         "xrt_freestanding_trap(\"module initialization failed\");\n");
        } else {
            fprintf(out, "    if (XR_UNLIKELY(xrt_has_pending_error())) abort();\n");
        }
    }
    fprintf(out, "}\n");
}

XR_FUNC void xi_cgen_main(XiCgenCtx *ctx, FILE *out, XiModule **modules, int n, int entry_index) {
    XR_DCHECK(ctx != NULL, "xi_cgen_main: NULL ctx");
    XR_DCHECK(out != NULL, "xi_cgen_main: NULL output");
    XR_DCHECK(n > 0, "xi_cgen_main: no modules");
    XR_DCHECK(entry_index >= 0 && entry_index < n, "xi_cgen_main: bad entry_index");

    bool entry_is_coro = cg_entry_init_uses_resumable_frame(ctx, modules, n, entry_index);
    bool entry_has_descriptor = cg_entry_uses_root_descriptor(ctx);
    CgBuiltinInitPlan builtin_plan = cg_builtin_init_plan_for_modules(modules, n);
    uint32_t runtime_caps = cg_runtime_caps_from_entry_plan(ctx);
    if (entry_is_coro)
        runtime_caps |= XR_AOT_CAP_CORO;
    bool entry_needs_runtime =
        entry_is_coro || entry_has_descriptor || cg_runtime_caps_need_runtime(runtime_caps);
    const char *entry_source_path = cg_entry_source_path(ctx, modules, n, entry_index);

    bool has_runtime_value_ops = !ctx->freestanding_profile;
    if (entry_needs_runtime && has_runtime_value_ops) {
        emit_xrt_runtime_value_ops(out);
    }
    if (entry_needs_runtime) {
        emit_xrt_runtime_core_configure_fn(out, runtime_caps);
    }

    fprintf(out, "int main(int argc, char **argv) {\n");
    fprintf(out, "    xrt_arc_init();\n");
    // main() has the real argv, so process.args drops argv[0] (the program
    // name); the --shared load-constructor path has no argv and passes "0"/NULL.
    emit_xrt_builtin_init(out, &builtin_plan, entry_source_path, "argc > 1 ? argc - 1 : 0",
                          "argc > 1 ? argv + 1 : NULL");
    if (entry_needs_runtime) {
        emit_xrt_runtime_init(out, &builtin_plan, runtime_caps, entry_source_path,
                              has_runtime_value_ops);
    } else {
        fprintf(out, "    (void) argc;\n");
        fprintf(out, "    (void) argv;\n");
    }
    if (entry_has_descriptor)
        fprintf(out, "    if (!xr_aot_root_descriptor_begin(rt)) { xr_aot_runtime_delete(rt); "
                     "xrt_bump_destroy(); return 1; }\n");
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
            cg_emit_main_pending_error_return(out, entry_needs_runtime);
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
            cg_emit_main_pending_error_return(out, entry_needs_runtime);
        }
    }
    if (entry_has_descriptor)
        fprintf(out, "    if (!xr_aot_root_descriptor_end(rt)) { xr_aot_runtime_delete(rt); "
                     "xrt_bump_destroy(); return 1; }\n");
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

    if (!cg_require_backend_tree(ctx, main_func))
        return;

    /* Reset function name counter for each compilation unit */
    ctx->fname_counter = 0;
    cg_reset_emitted_funcs(ctx);

    XiModule *single_module[1] = {module};
    ctx->all_modules = single_module;
    ctx->all_nmodules = 1;
    cg_reachability_cache_clear(ctx);
    ctx->nshared_native_exports = 0;
    if (ctx->shared_native_exports)
        memset(ctx->shared_native_exports, 0,
               (size_t) ctx->shared_native_exports_cap * sizeof(CgSharedNativeExport));
    cg_prepare_sync_go_targets_for_modules(ctx, single_module, 1);

    /* Initialize from module metadata */
    cg_init_from_module(ctx, module);
    ctx->shared_name = "xrt_shared";
    cg_collect_shared_native_instances(ctx);
    if (!cg_mandatory_plans_preflight_func_tree(ctx, main_func))
        return;

    cg_writer_reset(ctx);
    if (ctx->error)
        return;

    /* Build every section off-output.  The final unit is assembled in fixed
     * phase order and copied to the caller only after every buffer closes and
     * Cgen remains error-free. */
    char *typebuf = NULL;
    size_t typesz = 0;
    FILE *types = xr_open_memstream(&typebuf, &typesz);
    char *forwardbuf = NULL;
    size_t forwardsz = 0;
    FILE *forwards = xr_open_memstream(&forwardbuf, &forwardsz);
    char *staticbuf = NULL;
    size_t staticsz = 0;
    FILE *statics = xr_open_memstream(&staticbuf, &staticsz);
    char *bodybuf = NULL;
    size_t bodysz = 0;
    FILE *body = xr_open_memstream(&bodybuf, &bodysz);
    if (!types || !forwards || !statics || !body) {
        ctx->error = true;
        if (types)
            (void) xr_close_memstream(types, &typebuf, &typesz);
        if (forwards)
            (void) xr_close_memstream(forwards, &forwardbuf, &forwardsz);
        if (statics)
            (void) xr_close_memstream(statics, &staticbuf, &staticsz);
        if (body)
            (void) xr_close_memstream(body, &bodybuf, &bodysz);
        xr_free(typebuf);
        xr_free(forwardbuf);
        xr_free(staticbuf);
        xr_free(bodybuf);
        return;
    }

    emit_class_native_typedefs(ctx, types, module, prefix);
    emit_class_shared_native_storage_decls(ctx, types, prefix);
    emit_struct_native_typedefs(types, main_func, prefix);
    emit_enum_native_typedefs(ctx, types, module);

    emit_forward_decls(ctx, forwards, main_func, prefix);
    fprintf(forwards, "\n");

    cg_emit_global_asm(statics, module);
    cg_emit_freestanding_static_scalar_const_defs(ctx, statics, module);
    cg_emit_freestanding_static_fixed_array_defs(ctx, statics, module);
    cg_emit_freestanding_static_fixed_matrix_defs(ctx, statics, module);
    cg_emit_freestanding_static_fixed_cube_defs(ctx, statics, module);
    cg_emit_freestanding_static_fixed_struct_array_defs(ctx, statics, module, prefix);
    cg_emit_freestanding_static_fixed_tuple_array_defs(ctx, statics, module);
    cg_emit_freestanding_static_tuple_defs(ctx, statics, module);
    cg_emit_freestanding_static_struct_defs(ctx, statics, module, prefix);
    cg_emit_freestanding_imported_static_const_decls(ctx, statics, module);

    emit_class_native_clone_helpers(ctx, body, module, prefix);
    xi_cgen_func(ctx, body, main_func, prefix);

    if (ctx->emit_main) {
        CgBuiltinInitPlan builtin_plan = cg_builtin_init_plan_for_func(main_func);
        const char *entry_source_path = cg_entry_source_path(ctx, single_module, 1, 0);
        bool entry_is_coro = cg_entry_init_uses_resumable_frame(ctx, single_module, 1, 0);
        bool entry_has_descriptor = cg_entry_uses_root_descriptor(ctx);
        uint32_t runtime_caps = cg_runtime_caps_from_entry_plan(ctx);
        if (entry_is_coro)
            runtime_caps |= XR_AOT_CAP_CORO;
        bool entry_needs_runtime =
            entry_is_coro || entry_has_descriptor || cg_runtime_caps_need_runtime(runtime_caps);
        bool has_runtime_value_ops = !ctx->freestanding_profile;
        if (entry_needs_runtime && has_runtime_value_ops) {
            emit_xrt_runtime_value_ops(body);
        }
        if (entry_needs_runtime) {
            emit_xrt_runtime_core_configure_fn(body, runtime_caps);
        }

        fprintf(body, "int main(int argc, char **argv) {\n");
        fprintf(body, "    xrt_arc_init();\n");
        emit_xrt_builtin_init(body, &builtin_plan, entry_source_path, "argc > 1 ? argc - 1 : 0",
                              "argc > 1 ? argv + 1 : NULL");
        if (entry_needs_runtime) {
            emit_xrt_runtime_init(body, &builtin_plan, runtime_caps, entry_source_path,
                                  has_runtime_value_ops);
        }
        if (entry_has_descriptor)
            fprintf(body, "    if (!xr_aot_root_descriptor_begin(rt)) { xr_aot_runtime_delete(rt); "
                          "xrt_bump_destroy(); return 1; }\n");
        if (entry_is_coro) {
            fprintf(body, "    void *_entry_frame = ");
            emit_fname_suffix(ctx, body, prefix, main_func, "_aot_frame_new");
            fprintf(body, "();\n");
            fprintf(body, "    xr_aot_run_main(rt, &");
            emit_fname_suffix(ctx, body, prefix, main_func, "_aot_desc");
            fprintf(body, ", _entry_frame);\n");
            cg_emit_main_pending_error_return(body, entry_needs_runtime);
        } else {
            if (!entry_needs_runtime) {
                fprintf(body, "    (void) argc;\n");
                fprintf(body, "    (void) argv;\n");
            }
            fprintf(body, "    ");
            emit_fname(ctx, body, prefix, main_func);
            fprintf(body, "(NULL);\n");
            cg_emit_main_pending_error_return(body, entry_needs_runtime);
        }
        if (entry_has_descriptor)
            fprintf(body, "    if (!xr_aot_root_descriptor_end(rt)) { xr_aot_runtime_delete(rt); "
                          "xrt_bump_destroy(); return 1; }\n");
        if (entry_needs_runtime) {
            fprintf(body, "    xr_aot_runtime_delete(rt);\n");
        }
        fprintf(body, "    xrt_bump_destroy();\n");
        fprintf(body, "    return 0;\n");
        fprintf(body, "}\n");
    } else {
        xi_cgen_shared_lib_ctor(ctx, body, single_module, 1, 0);
    }

    bool close_failed = xr_close_memstream(types, &typebuf, &typesz) != 0;
    close_failed = (xr_close_memstream(forwards, &forwardbuf, &forwardsz) != 0) || close_failed;
    close_failed = (xr_close_memstream(statics, &staticbuf, &staticsz) != 0) || close_failed;
    close_failed = (xr_close_memstream(body, &bodybuf, &bodysz) != 0) || close_failed;
    if (close_failed || ctx->error) {
        ctx->error = true;
        xr_free(typebuf);
        xr_free(forwardbuf);
        xr_free(staticbuf);
        xr_free(bodybuf);
        return;
    }

    char *unitbuf = NULL;
    size_t unitsz = 0;
    FILE *unit = xr_open_memstream(&unitbuf, &unitsz);
    if (!unit) {
        ctx->error = true;
        xr_free(typebuf);
        xr_free(forwardbuf);
        xr_free(staticbuf);
        xr_free(bodybuf);
        return;
    }
    if (cg_writer_enter(ctx, unit, CG_WRITER_PHASE_INCLUDES))
        xi_cgen_header(ctx, unit);
    if (cg_writer_enter(ctx, unit, CG_WRITER_PHASE_TYPES)) {
        /* enum aggregate from_base converters live in the TYPES phase and may
         * reference the module shared-slot array (class native type ids after
         * the unified native class representation). Emit a tentative forward
         * declaration before the type buffer so those file-scope static inline
         * converters see xrt_shared; the real definition (below) may be a
         * second tentative definition or an initialized one, both legal in C. */
        if (main_func->nshared > 0 && typebuf && strstr(typebuf, "xrt_shared["))
            fprintf(unit, "static XrValue xrt_shared[%u];\n\n", (unsigned) main_func->nshared);
        fwrite(typebuf, 1, typesz, unit);
    }
    emit_canonical_extern_decls(ctx, unit);
    if (cg_writer_enter(ctx, unit, CG_WRITER_PHASE_INTERNAL_DECLS)) {
        emit_extern_closure_adapter_decls(ctx, unit);
        fwrite(forwardbuf, 1, forwardsz, unit);
    }
    if (cg_writer_enter(ctx, unit, CG_WRITER_PHASE_STATIC_DATA))
        xi_cgen_emit_str_literal_defs(ctx, unit);
    if (main_func->nshared > 0 && ((bodybuf && strstr(bodybuf, "xrt_shared[")) ||
                                   (staticbuf && strstr(staticbuf, "xrt_shared[")) ||
                                   (typebuf && strstr(typebuf, "xrt_shared["))))
        cg_emit_shared_array_definition(ctx, unit, "static ", "xrt_shared", module,
                                        main_func->nshared);
    if (!ctx->error)
        fwrite(staticbuf, 1, staticsz, unit);
    if (cg_writer_enter(ctx, unit, CG_WRITER_PHASE_BODIES)) {
        emit_extern_closure_adapter_defs(ctx, unit);
        fwrite(bodybuf, 1, bodysz, unit);
    }
    ctx->writer.phase = ctx->error ? ctx->writer.phase : CG_WRITER_PHASE_FINALIZED;
    bool unit_close_failed = xr_close_memstream(unit, &unitbuf, &unitsz) != 0;
    if (!ctx->error && !unit_close_failed)
        fwrite(unitbuf, 1, unitsz, out);
    else
        ctx->error = true;

    xr_free(typebuf);
    xr_free(forwardbuf);
    xr_free(staticbuf);
    xr_free(bodybuf);
    xr_free(unitbuf);
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
    /* Reachability still depends on module-local class/import resolution.
     * Until that analysis is fully prepared into XaotBundle, each translation
     * unit must rebuild it under its own module context. */
    cg_reachability_cache_clear(ctx);
    ctx->extern_linkage = true;
    /* Internal (non-exported) functions take a per-module ordinal suffix, so
     * adding or removing a function in one module never renumbers another's
     * symbols.  Cross-module-visible functions use order-independent names
     * (see emit_fname), so the counter is reset per translation unit. */
    ctx->fname_counter = 0;
    cg_reset_emitted_funcs(ctx);

    /* Every unit references cross-module symbols by name, so every module must
     * arrive as a verified Backend program. CGen never repairs its input. */
    for (int i = 0; i < nmodules; i++) {
        if (modules[i] && modules[i]->init && !cg_require_backend_tree(ctx, modules[i]->init))
            return;
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
    if (!cg_mandatory_plans_preflight_func_tree(ctx, module->init)) {
        ctx->shared_name = "xrt_shared";
        ctx->extern_linkage = false;
        return;
    }

    cg_writer_reset(ctx);
    if (ctx->error) {
        ctx->shared_name = "xrt_shared";
        ctx->extern_linkage = false;
        return;
    }

    char *staticbuf = NULL;
    size_t staticsz = 0;
    FILE *statics = xr_open_memstream(&staticbuf, &staticsz);
    char *bodybuf = NULL;
    size_t bodysz = 0;
    FILE *body = xr_open_memstream(&bodybuf, &bodysz);
    if (!statics || !body) {
        ctx->error = true;
        if (statics)
            (void) xr_close_memstream(statics, &staticbuf, &staticsz);
        if (body)
            (void) xr_close_memstream(body, &bodybuf, &bodysz);
        xr_free(staticbuf);
        xr_free(bodybuf);
        ctx->shared_name = "xrt_shared";
        ctx->extern_linkage = false;
        return;
    }

    ctx->n_xmod_refs = 0;
    ctx->collect_xmod_refs = true;
    cg_emit_global_asm(statics, module);
    cg_emit_freestanding_static_scalar_const_defs(ctx, statics, module);
    cg_emit_freestanding_static_fixed_array_defs(ctx, statics, module);
    cg_emit_freestanding_static_fixed_matrix_defs(ctx, statics, module);
    cg_emit_freestanding_static_fixed_cube_defs(ctx, statics, module);
    cg_emit_freestanding_static_fixed_struct_array_defs(ctx, statics, module, prefix);
    cg_emit_freestanding_static_fixed_tuple_array_defs(ctx, statics, module);
    cg_emit_freestanding_static_tuple_defs(ctx, statics, module);
    cg_emit_freestanding_static_struct_defs(ctx, statics, module, prefix);
    emit_class_native_clone_helpers(ctx, body, module, prefix);
    xi_cgen_func(ctx, body, module->init, prefix);

    if (is_entry) {
        if (ctx->emit_main)
            xi_cgen_main(ctx, body, modules, nmodules, entry_index);
        else
            xi_cgen_shared_lib_ctor(ctx, body, modules, nmodules, entry_index);
    }
    ctx->collect_xmod_refs = false;

    bool close_failed = xr_close_memstream(statics, &staticbuf, &staticsz) != 0;
    close_failed = (xr_close_memstream(body, &bodybuf, &bodysz) != 0) || close_failed;
    if (close_failed || ctx->error) {
        ctx->error = true;
        xr_free(staticbuf);
        xr_free(bodybuf);
        ctx->shared_name = "xrt_shared";
        ctx->extern_linkage = false;
        return;
    }

    char *unitbuf = NULL;
    size_t unitsz = 0;
    FILE *unit = xr_open_memstream(&unitbuf, &unitsz);
    if (!unit) {
        ctx->error = true;
        xr_free(staticbuf);
        xr_free(bodybuf);
        ctx->shared_name = "xrt_shared";
        ctx->extern_linkage = false;
        return;
    }

    /* Shared includes; the entry unit defines the runtime impl (XRT_IMPL) and
     * xrt_builtins, every other unit only declares them. */
    if (cg_writer_enter(ctx, unit, CG_WRITER_PHASE_INCLUDES))
        cg_emit_tu_includes(unit, is_entry, ctx->freestanding_profile,
                            ctx->simd_active && ctx->target ? ctx->target->simd_features : 0);

    /* Native class / struct typedefs must precede the forward declarations and
     * bodies that use them.  Imported (and cross-module base) class typedefs
     * come first so a locally-defined class can embed an imported base; then
     * this module's own classes. */
    if (cg_writer_enter(ctx, unit, CG_WRITER_PHASE_TYPES)) {
        emit_imported_class_native_typedefs(ctx, unit);
        emit_class_native_typedefs(ctx, unit, module, prefix);
        emit_class_shared_native_storage_decls(ctx, unit, prefix);
        emit_imported_class_shared_native_storage_decls(ctx, unit);
        emit_struct_native_typedefs(unit, module->init, prefix);
        /* enum aggregate from_base converters (emitted next, in the TYPES phase
         * as file-scope static inline functions) may reference module shared-slot
         * arrays for class native type ids after the unified native class
         * representation. Declare this module's shared array (tentative, matching
         * the definition's external linkage below) and imported modules' shared
         * arrays (extern) before the enum typedefs so the converters resolve. */
        if (module->init->nshared > 0)
            fprintf(unit, "XrValue %s[%u];\n", shared_buf, (unsigned) module->init->nshared);
        for (int i = 0; i < nmodules; i++) {
            XiModule *m = modules[i];
            if (i == mod_index || !m || !m->init || m->init->nshared == 0)
                continue;
            fprintf(unit, "extern XrValue xrt_shared_%s[];\n", m->name ? m->name : "mod");
        }
        emit_imported_enum_native_typedefs(ctx, unit);
        emit_enum_native_typedefs(ctx, unit, module);
        fprintf(unit, "\n");
    }

    emit_canonical_extern_decls(ctx, unit);

    /* Forward declarations: this module's own functions in full, plus only the
     * cross-module symbols this unit actually references. */
    if (cg_writer_enter(ctx, unit, CG_WRITER_PHASE_INTERNAL_DECLS)) {
        emit_extern_closure_adapter_decls(ctx, unit);
        emit_forward_decls(ctx, unit, module->init, prefix);
        for (int i = 0; i < ctx->n_xmod_refs; i++)
            emit_one_forward_decl(ctx, unit, ctx->xmod_ref_funcs[i], ctx->xmod_ref_prefixes[i]);
        fprintf(unit, "\n");
    }

    if (cg_writer_enter(ctx, unit, CG_WRITER_PHASE_STATIC_DATA)) {
        fprintf(unit, "%sXrAotContext xrt_global_ctx;\n", is_entry ? "" : "extern ");
        fprintf(unit, "%sXrValue xrt_builtins[%d];\n\n", is_entry ? "" : "extern ",
                XR_USER_GLOBALS_START);
        if (module->init->nshared > 0)
            cg_emit_shared_array_definition(ctx, unit, "", shared_buf, module,
                                            module->init->nshared);
        for (int i = 0; i < nmodules; i++) {
            XiModule *m = modules[i];
            if (i == mod_index || !m || !m->init || m->init->nshared == 0)
                continue;
            fprintf(unit, "extern XrValue xrt_shared_%s[];\n", m->name ? m->name : "mod");
        }
        fprintf(unit, "\n");
        cg_emit_freestanding_imported_static_const_decls(ctx, unit, module);
        xi_cgen_emit_str_literal_defs(ctx, unit);
        fwrite(staticbuf, 1, staticsz, unit);
    }
    if (cg_writer_enter(ctx, unit, CG_WRITER_PHASE_BODIES)) {
        emit_extern_closure_adapter_defs(ctx, unit);
        fwrite(bodybuf, 1, bodysz, unit);
    }
    ctx->writer.phase = ctx->error ? ctx->writer.phase : CG_WRITER_PHASE_FINALIZED;
    bool unit_close_failed = xr_close_memstream(unit, &unitbuf, &unitsz) != 0;
    if (!ctx->error && !unit_close_failed)
        fwrite(unitbuf, 1, unitsz, out);
    else
        ctx->error = true;
    xr_free(staticbuf);
    xr_free(bodybuf);
    xr_free(unitbuf);

    ctx->shared_name = "xrt_shared";
    ctx->extern_linkage = false;
}
