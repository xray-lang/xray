/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_stdlib_helpers.inc.c - AOT direct-call dispatch for runtime stdlib
 *
 * Recognizes `<module>.<method>(...)` calls whose receiver resolves to a
 * runtime stdlib module import and emits a direct call to the matching
 * isolate-free helper, bypassing the tagged runtime module table. Pure helpers
 * can live in the freestanding AOT runtime (xrt_*) while runtime-backed
 * helpers may still be local extern shims.
 *
 * This is the general, table-driven successor to the bespoke math/time AOT
 * paths: new functions are added as data rows, not new code paths. A module
 * that appears here is fully "claimed" for AOT — any unsupported method on it
 * raises a clean codegen error rather than silently dispatching on the
 * XR_NULL_VAL module placeholder.
 */

/* How a shim returns its result. */
typedef enum {
    CG_AOT_RET_VALUE,           /* tagged XrValue, converted to the call's rep */
    CG_AOT_RET_STR_BORROWED,    /* (const char *data, int64_t *out_len) slice into an
                                 * input/static buffer; copied into an AOT string */
    CG_AOT_RET_I64_PAIR_RESULT, /* XrtI64PairResult, materialized as a typed
                                 * two-int Record; error_index names a generated
                                 * native enum variant */
} CgAotRetKind;

#define CG_AOT_STDLIB_VARIADIC UINT16_MAX

typedef struct CgAotStdlibMethod {
    const char *module; /* stdlib module identifier (e.g. "path") */
    const char *method; /* method name (e.g. "isAbsolute") */
    uint16_t argc;      /* argument count excluding the receiver */
    const char *shim;   /* xr_aot_<module>_<method> runtime symbol */
    /* One character per argument describing how it is passed to the shim:
     *   's' = string, lowered to specialized (const char *data, int64_t len)
     *   'p' = Path owner, lowered to specialized (const char *data, int64_t len)
     *   'v' = tagged XrValue passed as-is
     *   '*' = variadic strings, lowered to (argc, data[], len[]) */
    const char *arg_spec;
    CgAotRetKind ret_kind;
    const char *extern_decl; /* forward declaration emitted into generated C */
    uint32_t error_layout_id;
    const char *error_enum_name;
    const char *const *error_variant_names;
    uint16_t error_variant_count;
} CgAotStdlibMethod;

#include "xstdlib_aot_methods_generated.inc.c"

static const CgAotStdlibMethod *cg_aot_stdlib_generated_method_at(int index) {
    if (index < 0 || index >= CG_AOT_STDLIB_GENERATED_METHOD_COUNT)
        return NULL;
    return &g_aot_stdlib_generated_methods[index];
}

static const CgAotStdlibMethod *cg_aot_stdlib_method_at(int index) {
    return cg_aot_stdlib_generated_method_at(index);
}

static int cg_aot_stdlib_method_count(void) {
    return CG_AOT_STDLIB_GENERATED_METHOD_COUNT;
}

/* Whether `module` is a stdlib module with AOT direct-call support. The
 * import-ref emitter uses this to materialize the module object as an
 * XR_NULL_VAL placeholder (the real work happens at the call sites). */
static bool cg_module_has_aot_direct_calls(const char *module) {
    if (!module)
        return false;
    if (strcmp(module, "runtime") == 0)
        return true;
    for (int i = 0; i < cg_aot_stdlib_method_count(); i++) {
        const CgAotStdlibMethod *m = cg_aot_stdlib_method_at(i);
        if (m && strcmp(module, m->module) == 0)
            return true;
    }
    return false;
}

static bool cg_aot_stdlib_has_direct_member(const char *module, const char *member) {
    if (!module || !member)
        return false;
    if (strcmp(module, "runtime") == 0 &&
        (strcmp(member, "collectCycles") == 0 || strcmp(member, "disableCycleCollection") == 0 ||
         strcmp(member, "enableCycleCollection") == 0 ||
         strcmp(member, "isCycleCollectionEnabled") == 0 || strcmp(member, "liveBytes") == 0 ||
         strcmp(member, "liveObjects") == 0 || strcmp(member, "info") == 0))
        return true;
    for (int i = 0; i < cg_aot_stdlib_method_count(); i++) {
        const CgAotStdlibMethod *m = cg_aot_stdlib_method_at(i);
        if (m && m->module && m->method && strcmp(module, m->module) == 0 &&
            strcmp(member, m->method) == 0)
            return true;
    }
    return false;
}

static bool cg_module_has_aot_generated_constants(const char *module) {
    return cg_aot_stdlib_generated_module_has_constants(module);
}

static bool cg_emit_aot_stdlib_generated_constant_value(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                        const CgAotStdlibConst *c) {
    if (!ctx || !out || !v || !c)
        return false;

    if (c->kind == CG_AOT_STDLIB_CONST_I64) {
        const char *conv_suffix =
            emit_conversion_prefix(out, v->type, XR_REP_I64, cg_value_plan_storage_rep(ctx, v));
        if (c->i64_value == INT64_MIN)
            fprintf(out, "INT64_MIN");
        else
            fprintf(out, "INT64_C(%" PRId64 ")", c->i64_value);
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    if (c->kind == CG_AOT_STDLIB_CONST_F64 && c->f64_expr && c->f64_expr[0]) {
        const char *conv_suffix =
            emit_conversion_prefix(out, v->type, XR_REP_F64, cg_value_plan_storage_rep(ctx, v));
        fprintf(out, "%s", c->f64_expr);
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    if (c->kind == CG_AOT_STDLIB_CONST_HELPER_VALUE && c->helper && c->helper[0]) {
        const char *conv_suffix =
            emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_value_plan_storage_rep(ctx, v));
        fprintf(out, "%s()", c->helper);
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    return false;
}

static bool cg_emit_aot_stdlib_generated_constant_import_ref(XiCgenCtx *ctx, FILE *out,
                                                             const XiValue *v,
                                                             const XiImportRef *ref) {
    if (!ctx || !out || !v || !ref || !ref->module_path || !ref->member_name)
        return false;
    const CgAotStdlibConst *c =
        cg_aot_stdlib_generated_const_for_member(ref->module_path, ref->member_name);
    return cg_emit_aot_stdlib_generated_constant_value(ctx, out, v, c);
}

static bool cg_emit_aot_stdlib_generated_constant_field(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                        const XiValue *v) {
    if (!ctx || !out || !f || !v || v->nargs < 1 || !v->aux)
        return false;

    const char *field = (const char *) v->aux;
    for (int i = 0; i < CG_AOT_STDLIB_GENERATED_CONST_COUNT; i++) {
        const CgAotStdlibConst *c = cg_aot_stdlib_generated_const_at(i);
        if (!c || strcmp(field, c->name) != 0 ||
            !cg_value_is_module_import_ctx(ctx, f, v->args[0], c->module))
            continue;
        return cg_emit_aot_stdlib_generated_constant_value(ctx, out, v, c);
    }
    return false;
}

static void cg_emit_runtime_info_value(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                       const char *aot_ctx) {
    const XaotValuePlan *plan = cg_value_plan(ctx, v);
    const XrAggregateLayout *layout = cg_value_struct_layout(ctx, f, v);
    if (layout && layout->field_count == 9 && plan && cg_value_rep_is_struct_aggregate(plan->rep) &&
        plan->rep.c_type) {
        char fields[9][128];
        for (uint16_t i = 0; i < 9; i++)
            cg_struct_field_c_name(layout, i, fields[i], sizeof(fields[i]));
        fprintf(out,
                "({ XrAotRuntimeInfo _ri = xr_aot_runtime_info(%s); "
                "(%s){ .%s = _ri.live_bytes, .%s = _ri.live_kb, "
                ".%s = _ri.live_objects, .%s = _ri.cycle_collection_enabled, "
                ".%s = _ri.cycle_collections, .%s = _ri.finalizer_count, "
                ".%s = _ri.blocks, .%s = _ri.free_blocks, .%s = _ri.full_blocks }; })",
                aot_ctx, plan->rep.c_type, fields[0], fields[1], fields[2], fields[3], fields[4],
                fields[5], fields[6], fields[7], fields[8]);
        return;
    }

    if (cg_value_plan_storage_rep(ctx, v) != XR_REP_TAGGED) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: runtime.info has no verified AOT record layout\n");
        emit_codegen_abort_expr(out);
        return;
    }
    fprintf(out,
            "({ static const char *const _rif[] = {\"liveBytes\", \"liveKB\", "
            "\"liveObjects\", \"cycleCollectionEnabled\", \"cycleCollections\", "
            "\"finalizerCount\", \"blocks\", \"freeBlocks\", \"fullBlocks\"}; "
            "XrAotRuntimeInfo _ri = xr_aot_runtime_info(%s); "
            "XrValue _riv = xrt_record_new_named(9, _rif); "
            "xrt_json_set_field(_riv, 0, XR_FROM_INT(_ri.live_bytes)); "
            "xrt_json_set_field(_riv, 1, XR_FROM_FLOAT(_ri.live_kb)); "
            "xrt_json_set_field(_riv, 2, XR_FROM_INT(_ri.live_objects)); "
            "xrt_json_set_field(_riv, 3, XR_FROM_BOOL(_ri.cycle_collection_enabled)); "
            "xrt_json_set_field(_riv, 4, XR_FROM_INT(_ri.cycle_collections)); "
            "xrt_json_set_field(_riv, 5, XR_FROM_INT(_ri.finalizer_count)); "
            "xrt_json_set_field(_riv, 6, XR_FROM_INT(_ri.blocks)); "
            "xrt_json_set_field(_riv, 7, XR_FROM_INT(_ri.free_blocks)); "
            "xrt_json_set_field(_riv, 8, XR_FROM_INT(_ri.full_blocks)); "
            "_riv; })",
            aot_ctx);
}

static bool cg_emit_runtime_control_call(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                         const XiValue *v, const char *method, uint16_t argc) {
    const char *aot_ctx = xicgen_aot_context_expr(ctx, f);
    if (!method || argc != 0) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT runtime control call '%s'\n",
                method ? method : "?");
        emit_codegen_abort_expr(out);
        return true;
    }

    if (strcmp(method, "info") == 0) {
        cg_emit_runtime_info_value(ctx, out, f, v, aot_ctx);
        return true;
    }
    if (strcmp(method, "disableCycleCollection") == 0 ||
        strcmp(method, "enableCycleCollection") == 0) {
        const char *helper = strcmp(method, "disableCycleCollection") == 0
                                 ? "xr_aot_runtime_disable_cycle_collection"
                                 : "xr_aot_runtime_enable_cycle_collection";
        const char *suffix =
            emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_value_plan_storage_rep(ctx, v));
        fprintf(out, "({ %s(%s); XR_NULL_VAL; })", helper, aot_ctx);
        emit_conversion_suffix(out, suffix);
        return true;
    }

    const char *helper = NULL;
    if (strcmp(method, "collectCycles") == 0)
        helper = "xr_aot_runtime_collect_cycles";
    else if (strcmp(method, "liveBytes") == 0)
        helper = "xr_aot_runtime_live_bytes";
    else if (strcmp(method, "liveObjects") == 0)
        helper = "xr_aot_runtime_live_objects";
    if (helper) {
        const char *suffix =
            emit_conversion_prefix(out, v->type, XR_REP_I64, cg_value_plan_storage_rep(ctx, v));
        fprintf(out, "%s(%s)", helper, aot_ctx);
        emit_conversion_suffix(out, suffix);
        return true;
    }
    if (strcmp(method, "isCycleCollectionEnabled") == 0) {
        const char *suffix =
            emit_conversion_prefix(out, v->type, XR_REP_I64, cg_value_plan_storage_rep(ctx, v));
        fprintf(out, "(int64_t)xr_aot_runtime_is_cycle_collection_enabled(%s)", aot_ctx);
        emit_conversion_suffix(out, suffix);
        return true;
    }

    ctx->error = true;
    fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT runtime control call 'runtime.%s'\n", method);
    emit_codegen_abort_expr(out);
    return true;
}

static bool xicgen_emit_runtime_control_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                               const XiValue *v) {
    if (!v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) || v->nargs < 1 ||
        !cg_value_is_module_import_ctx(ctx, f, v->args[0], "runtime"))
        return false;

    return cg_emit_runtime_control_call(ctx, out, f, v, (const char *) v->aux,
                                        (uint16_t) (v->nargs - 1));
}

static const CgAotStdlibMethod *cg_find_aot_stdlib_method(const char *module, const char *method,
                                                          uint16_t argc) {
    if (!module || !method)
        return NULL;
    for (int i = 0; i < cg_aot_stdlib_method_count(); i++) {
        const CgAotStdlibMethod *m = cg_aot_stdlib_method_at(i);
        if (!m)
            continue;
        if ((m->argc == argc || m->argc == CG_AOT_STDLIB_VARIADIC) &&
            strcmp(module, m->module) == 0 && strcmp(method, m->method) == 0)
            return m;
    }
    return NULL;
}

/* Resolve the AOT-managed stdlib module that a method-call receiver names. */
static const char *cg_aot_stdlib_module_of_receiver(const XiCgenCtx *ctx, const XiFunc *f,
                                                    const XiValue *recv) {
    for (int i = 0; i < cg_aot_stdlib_method_count(); i++) {
        const CgAotStdlibMethod *m = cg_aot_stdlib_method_at(i);
        const char *module = m ? m->module : NULL;
        if (module && cg_value_is_module_import_ctx(ctx, f, recv, module))
            return module;
    }
    return NULL;
}

/* Emit the comma-separated shim arguments per the method's arg_spec. String
 * args ('s') are lowered to the specialized (data, length) pair via the AOT
 * string accessors; other args ('v') pass through as tagged values. SSA arg
 * references have no side effects, so emitting one twice (data + length) is
 * safe. */
static void cg_emit_aot_stdlib_args(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                    const CgAotStdlibMethod *m, uint16_t call_argc,
                                    uint16_t arg_base) {
    (void) f;
    for (uint16_t a = 0; a < call_argc; a++) {
        const XiValue *arg = v->args[arg_base + a];
        char spec = m->arg_spec[a];
        if (a > 0)
            fprintf(out, ", ");
        if (spec == 's') {
            fprintf(out, "xr_str_data(");
            emit_value_as_rep_ctx(ctx, out, arg, XR_REP_TAGGED);
            fprintf(out, "), xr_str_len(");
            emit_value_as_rep_ctx(ctx, out, arg, XR_REP_TAGGED);
            fprintf(out, ")");
        } else if (spec == 'p') {
            fprintf(out, "xrt_path_data(");
            emit_value_as_rep_ctx(ctx, out, arg, XR_REP_TAGGED);
            fprintf(out, "), xrt_path_len(");
            emit_value_as_rep_ctx(ctx, out, arg, XR_REP_TAGGED);
            fprintf(out, ")");
        } else {
            emit_value_as_rep_ctx(ctx, out, arg, XR_REP_TAGGED);
        }
    }
}

static bool cg_aot_stdlib_method_is_variadic_strings(const CgAotStdlibMethod *m) {
    return m && m->argc == CG_AOT_STDLIB_VARIADIC && m->arg_spec && strcmp(m->arg_spec, "*") == 0;
}

static bool cg_emit_aot_i64_pair_result(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const XiValue *v, const CgAotStdlibMethod *m,
                                        uint16_t call_argc, uint16_t arg_base) {
    const XrAggregateLayout *layout = cg_value_struct_layout(ctx, f, v);
    if (!layout)
        layout = cg_type_struct_layout(v ? v->type : NULL);
    const XrType *record_type = v ? v->type : NULL;
    bool layout_ok = layout && layout->field_count == 2 &&
                     layout->fields[0].native_type == XR_NATIVE_I64 &&
                     layout->fields[1].native_type == XR_NATIVE_I64;
    bool record_ok = record_type && XR_TYPE_IS_RECORD(record_type) &&
                     record_type->object.field_count == 2 && record_type->object.field_names &&
                     record_type->object.field_types && record_type->object.field_types[0] &&
                     record_type->object.field_types[1] &&
                     XR_TYPE_IS_INT(record_type->object.field_types[0]) &&
                     XR_TYPE_IS_INT(record_type->object.field_types[1]);
    if (!ctx || !out || !v || !m || (!layout_ok && !record_ok) || !m->error_enum_name ||
        !m->error_variant_names || m->error_variant_count == 0) {
        fprintf(stderr,
                "[xi_cgen] ERROR: invalid i64-pair result contract for %s.%s "
                "(layout=%p fields=%u error-enum=%s variants=%u)\n",
                m && m->module ? m->module : "?", m && m->method ? m->method : "?",
                (const void *) layout,
                layout ? (unsigned) layout->field_count
                       : (XR_TYPE_HAS_OBJECT_SHAPE(record_type)
                              ? (unsigned) record_type->object.field_count
                              : 0),
                m && m->error_enum_name ? m->error_enum_name : "?",
                m ? (unsigned) m->error_variant_count : 0);
        if (ctx)
            ctx->error = true;
        emit_codegen_abort_expr(out);
        return true;
    }

    unsigned id = v->id;
    char field0[128];
    char field1[128];
    if (layout) {
        cg_struct_field_c_name(layout, 0, field0, sizeof(field0));
        cg_struct_field_c_name(layout, 1, field1, sizeof(field1));
    }

    fprintf(out, "XrtI64PairResult _arp%u = %s(", id, m->shim);
    cg_emit_aot_stdlib_args(ctx, out, f, v, m, call_argc, arg_base);
    fprintf(out,
            "); if (_arp%u.error_index >= 0) { uint32_t _are%u = "
            "(uint32_t)_arp%u.error_index; if (_are%u >= UINT32_C(%u)) "
            "{ fputs(\"invalid direct stdlib error ordinal\\n\", stderr); abort(); } "
            "xrt_pending_error = "
            "xrt_enum_box_new(UINT32_C(%u), ",
            id, id, id, id, (unsigned) m->error_variant_count, (unsigned) m->error_layout_id);
    emit_c_string_literal(out, m->error_enum_name);
    fprintf(out, ", ");
    fprintf(out, "((const char *const[]){");
    for (uint16_t i = 0; i < m->error_variant_count; i++) {
        if (i > 0)
            fprintf(out, ", ");
        emit_c_string_literal(out, m->error_variant_names[i]);
    }
    fprintf(out, "})[_are%u], _are%u); } ", id, id);

    const XaotValuePlan *plan = cg_value_plan(ctx, v);
    if (layout && plan && cg_value_rep_is_struct_aggregate(plan->rep) && plan->rep.c_type) {
        fprintf(out, "(%s){ .%s = _arp%u.first, .%s = _arp%u.second }; })", plan->rep.c_type,
                field0, id, field1, id);
        return true;
    }

    fprintf(out, "XrValue _arr%u = xrt_record_new_named(2, (const char *const[]){", id);
    const char *name0 =
        layout && layout->field_names ? layout->field_names[0] : record_type->object.field_names[0];
    const char *name1 =
        layout && layout->field_names ? layout->field_names[1] : record_type->object.field_names[1];
    emit_c_string_literal(out, name0 ? name0 : "?");
    fprintf(out, ", ");
    emit_c_string_literal(out, name1 ? name1 : "?");
    fprintf(out,
            "}); xrt_json_set_field(_arr%u, 0, XR_FROM_INT(_arp%u.first)); "
            "xrt_json_set_field(_arr%u, 1, XR_FROM_INT(_arp%u.second)); _arr%u; })",
            id, id, id, id, id);
    return true;
}

static bool cg_emit_aot_stdlib_direct_call(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                           const XiValue *v, const CgAotStdlibMethod *m,
                                           uint16_t call_argc, uint16_t arg_base) {
    if (!ctx || !out || !v || !m)
        return false;

    /* Wrap in a statement-expression so the shim's forward declaration is
     * self-contained at the call site, then convert the result to the value's
     * required storage representation. */
    XrRep target_rep = xicgen_value_c_storage_rep(ctx, f, v);
    fprintf(out, "({ ");
    if (m->extern_decl && m->extern_decl[0])
        fprintf(out, "%s ", m->extern_decl);

    if (m->ret_kind == CG_AOT_RET_I64_PAIR_RESULT)
        return cg_emit_aot_i64_pair_result(ctx, out, f, v, m, call_argc, arg_base);

    if (m->ret_kind == CG_AOT_RET_STR_BORROWED) {
        /* The shim returns a borrowed (data, *out_len) slice; copy it into a
         * fresh AOT string. Temps are id-suffixed to nest safely. */
        unsigned id = v->id;
        fprintf(out, "int64_t _arl%u = 0; const char *_ard%u = %s(", id, id, m->shim);
        cg_emit_aot_stdlib_args(ctx, out, f, v, m, call_argc, arg_base);
        fprintf(out, ", &_arl%u); XrValue _ars%u = xrt_str_alloc((size_t) _arl%u); ", id, id, id);
        fprintf(out, "if (_arl%u) memcpy(xr_str_buf(_ars%u), _ard%u, (size_t) _arl%u); ", id, id,
                id, id);
        const char *suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, target_rep);
        fprintf(out, "_ars%u", id);
        emit_conversion_suffix(out, suffix);
        fprintf(out, "; })");
        return true;
    }

    if (cg_aot_stdlib_method_is_variadic_strings(m)) {
        unsigned id = v->id;
        if (call_argc > 0) {
            fprintf(out, "const char *_asd%u[%u] = {", id, (unsigned) call_argc);
            for (uint16_t a = 0; a < call_argc; a++) {
                if (a > 0)
                    fprintf(out, ", ");
                fprintf(out, "xr_str_data(");
                emit_value_as_rep_ctx(ctx, out, v->args[arg_base + a], XR_REP_TAGGED);
                fprintf(out, ")");
            }
            fprintf(out, "}; size_t _asl%u[%u] = {", id, (unsigned) call_argc);
            for (uint16_t a = 0; a < call_argc; a++) {
                if (a > 0)
                    fprintf(out, ", ");
                fprintf(out, "(size_t) xr_str_len(");
                emit_value_as_rep_ctx(ctx, out, v->args[arg_base + a], XR_REP_TAGGED);
                fprintf(out, ")");
            }
            fprintf(out, "}; ");
            fprintf(out, "XrValue _arv%u = %s((int64_t) %u, _asd%u, _asl%u); ", id, m->shim,
                    (unsigned) call_argc, id, id);
        } else {
            fprintf(out, "XrValue _arv%u = %s(0, NULL, NULL); ", id, m->shim);
        }
        const char *suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, target_rep);
        fprintf(out, "_arv%u", id);
        emit_conversion_suffix(out, suffix);
    } else {
        /* CG_AOT_RET_VALUE: shim returns a tagged XrValue. */
        const char *suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, target_rep);
        fprintf(out, "%s(", m->shim);
        cg_emit_aot_stdlib_args(ctx, out, f, v, m, call_argc, arg_base);
        fprintf(out, ")");
        emit_conversion_suffix(out, suffix);
    }
    fprintf(out, "; })");
    return true;
}

/* Emit a direct AOT call for a stdlib module method.
 * Returns true when the receiver is an AOT-managed stdlib module: supported
 * methods emit a direct shim call, unsupported ones raise a clean codegen
 * error (never a silent dispatch on the module placeholder). */
static bool xicgen_emit_stdlib_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                      const XiValue *v) {
    if (!v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) || v->nargs < 1)
        return false;

    const char *module = cg_aot_stdlib_module_of_receiver(ctx, f, v->args[0]);
    if (!module)
        return false;

    const char *method = (const char *) v->aux;
    uint16_t call_argc = (uint16_t) (v->nargs - 1);
    const CgAotStdlibMethod *m = cg_find_aot_stdlib_method(module, method, call_argc);
    if (!m) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT stdlib call '%s.%s'\n", module,
                method ? method : "?");
        emit_codegen_abort_expr(out);
        return true;
    }

    return cg_emit_aot_stdlib_direct_call(ctx, out, f, v, m, call_argc, 1);
}

/* Emit a direct AOT call for a selected stdlib member imported as a function
 * value, e.g. a helper exposed to stdlib/<module>/<module>.xr. */
static bool xicgen_emit_stdlib_import_call(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                           const XiValue *v) {
    if (!v || v->op != XI_CALL || v->nargs < 1)
        return false;

    const XiValue *callee = cg_unwrap_identity_value(v->args[0]);
    const XiImportRef *ref = (callee && callee->op == XI_IMPORT_REF && callee->aux)
                                 ? (const XiImportRef *) callee->aux
                                 : cg_import_ref_for_value(ctx, f, callee);
    if (!ref || !ref->module_path || !ref->member_name)
        return false;

    uint16_t call_argc = (uint16_t) (v->nargs - 1);
    if (strcmp(ref->module_path, "runtime") == 0)
        return cg_emit_runtime_control_call(ctx, out, f, v, ref->member_name, call_argc);

    const CgAotStdlibMethod *m =
        cg_find_aot_stdlib_method(ref->module_path, ref->member_name, call_argc);
    if (!m) {
        if (!cg_aot_stdlib_has_direct_member(ref->module_path, ref->member_name))
            return false;
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT stdlib call '%s.%s' with %u args\n",
                ref->module_path, ref->member_name, (unsigned) call_argc);
        emit_codegen_abort_expr(out);
        return true;
    }

    return cg_emit_aot_stdlib_direct_call(ctx, out, f, v, m, call_argc, 1);
}
