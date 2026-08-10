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
    CG_AOT_RET_I64,             /* unboxed int64_t, converted to the call's rep */
    CG_AOT_RET_ENUM_I64,        /* enum ordinal; boxed only at a tagged boundary */
    CG_AOT_RET_STR_BORROWED,    /* (const char *data, int64_t *out_len) slice into an
                                 * input/static buffer; copied into an AOT string */
    CG_AOT_RET_I64_PAIR_RESULT, /* XrtI64PairResult, materialized as a typed
                                 * two-int structural object; error_index names a generated
                                 * native enum variant */
} CgAotRetKind;

#define CG_AOT_STDLIB_VARIADIC UINT16_MAX

typedef struct CgAotStdlibMethod {
    /* All const char* rows below are owned: static string literals from the
     * generated xstdlib_aot_methods_generated.inc.c table (program lifetime). */
    const char *module; /* owned: static literal; stdlib module identifier (e.g. "path") */
    const char *method; /* owned: static literal; method name (e.g. "isAbsolute") */
    uint16_t argc;      /* argument count excluding the receiver */
    const char *shim;   /* owned: static literal; xr_aot_<module>_<method> runtime symbol */
    /* One character per argument describing how it is passed to the shim:
     *   's' = string, lowered to specialized (const char *data, int64_t len)
     *   'p' = Path owner, lowered to specialized (const char *data, int64_t len)
     *   'v' = tagged XrValue passed as-is
     *   'i' = opaque int handle, passed as a tagged XrValue like 'v'
     *   '*' = variadic strings, lowered to (argc, data[], len[])
     * stdlibgen rejects rows whose spec length differs from the fixed argc, so
     * a fixed-arity spec always covers every argument position. */
    const char *arg_spec; /* owned: static literal (generated table) */
    CgAotRetKind ret_kind;
    const char *extern_decl; /* owned: static literal; forward decl emitted into generated C */
    /* `enum_*` describes either the enum returned by CG_AOT_RET_ENUM_I64 or
     * the error enum carried by CG_AOT_RET_I64_PAIR_RESULT. */
    uint32_t enum_layout_id;
    const char *enum_name;            /* owned: static literal; generated enum name */
    const char *const *variant_names; /* owned: static generated literal table */
    uint16_t variant_count;
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

static int cg_aot_stdlib_method_index(const CgAotStdlibMethod *method) {
    if (!method)
        return -1;
    for (int i = 0; i < cg_aot_stdlib_method_count(); i++) {
        if (cg_aot_stdlib_method_at(i) == method)
            return i;
    }
    return -1;
}

static bool cg_mark_aot_stdlib_enum_scalar_sidecar(XiCgenCtx *ctx, const CgAotStdlibMethod *method,
                                                   uint32_t *out_index) {
    int index = cg_aot_stdlib_method_index(method);
    int count = cg_aot_stdlib_method_count();
    if (!ctx || index < 0 || method->ret_kind != CG_AOT_RET_ENUM_I64 || !method->enum_name ||
        !method->variant_names || method->variant_count == 0) {
        if (ctx)
            ctx->error = true;
        return false;
    }
    if ((uint32_t) count > ctx->stdlib_enum_scalar_sidecar_cap) {
        uint32_t old_cap = ctx->stdlib_enum_scalar_sidecar_cap;
        uint8_t *used = (uint8_t *) xr_realloc(ctx->stdlib_enum_scalar_sidecar_used,
                                               (size_t) count * sizeof(uint8_t));
        if (!used) {
            ctx->error = true;
            return false;
        }
        memset(used + old_cap, 0, (size_t) count - old_cap);
        ctx->stdlib_enum_scalar_sidecar_used = used;
        ctx->stdlib_enum_scalar_sidecar_cap = (uint32_t) count;
    }
    ctx->stdlib_enum_scalar_sidecar_used[index] = 1;
    if (out_index)
        *out_index = (uint32_t) index;
    return true;
}

static void cg_reset_aot_stdlib_enum_scalar_sidecars(XiCgenCtx *ctx) {
    if (ctx && ctx->stdlib_enum_scalar_sidecar_used && ctx->stdlib_enum_scalar_sidecar_cap > 0)
        memset(ctx->stdlib_enum_scalar_sidecar_used, 0,
               ctx->stdlib_enum_scalar_sidecar_cap * sizeof(uint8_t));
}

static void cg_emit_aot_stdlib_enum_scalar_sidecar_defs(XiCgenCtx *ctx, FILE *out) {
    if (!ctx || !out || !ctx->stdlib_enum_scalar_sidecar_used)
        return;
    const char *module_prefix =
        ctx->module && ctx->module->name && ctx->module->name[0] ? ctx->module->name : "mod";
    int count = cg_aot_stdlib_method_count();
    for (int i = 0; i < count; i++) {
        if ((uint32_t) i >= ctx->stdlib_enum_scalar_sidecar_cap ||
            !ctx->stdlib_enum_scalar_sidecar_used[i])
            continue;
        const CgAotStdlibMethod *method = cg_aot_stdlib_method_at(i);
        if (!method || method->ret_kind != CG_AOT_RET_ENUM_I64 || !method->enum_name ||
            !method->variant_names || method->variant_count == 0) {
            ctx->error = true;
            return;
        }
        fprintf(out, "static const char *const _xaot_stdlib_enum_names_%s_%d[%u] = {",
                module_prefix, i, (unsigned) method->variant_count);
        for (uint16_t variant = 0; variant < method->variant_count; variant++) {
            if (variant > 0)
                fprintf(out, ",");
            emit_c_string_literal(out, method->variant_names[variant]);
        }
        fprintf(out, "};\n");
        fprintf(out,
                "static const XrAotEnumScalarLayout _xaot_stdlib_enum_layout_%s_%d = "
                "{{XR_TENUM_SCALAR_LAYOUT, XR_OBJ_IMMORTAL, XR_RC_STICKY, 0, 0}, ",
                module_prefix, i);
        emit_c_string_literal(out, method->enum_name);
        fprintf(out, ", _xaot_stdlib_enum_names_%s_%d, %u, %u};\n", module_prefix, i,
                (unsigned) method->variant_count, (unsigned) method->enum_layout_id);
        fprintf(out,
                "static XrValue _xaot_stdlib_enum_box_%s_%d(int64_t ordinal) { "
                "if ((uint64_t) ordinal >= UINT64_C(%u)) { "
                "fputs(\"invalid direct stdlib enum ordinal\\n\", stderr); abort(); } "
                "return xrt_enum_scalar_box(&_xaot_stdlib_enum_layout_%s_%d, ordinal); }\n\n",
                module_prefix, i, (unsigned) method->variant_count, module_prefix, i);
    }
}

/* Whether `module` is a stdlib module with AOT direct-call support. The
 * import-ref emitter uses this to materialize the module object as an
 * XR_NULL_VAL placeholder (the real work happens at the call sites). */
static bool cg_module_has_aot_direct_calls(const char *module) {
    if (!module)
        return false;
    if (strcmp(module, "runtime") == 0 || strcmp(module, "test_yield") == 0)
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
        (strcmp(member, "liveBytes") == 0 || strcmp(member, "liveObjects") == 0 ||
         strcmp(member, "sharedBytes") == 0 || strcmp(member, "staticBytes") == 0 ||
         strcmp(member, "info") == 0))
        return true;
    if (strcmp(module, "test_yield") == 0 &&
        (strcmp(member, "simple") == 0 || strcmp(member, "add") == 0 ||
         strcmp(member, "sync") == 0 || strcmp(member, "blocking_sleep") == 0 ||
         strcmp(member, "counter_inc") == 0 || strcmp(member, "counter_get") == 0 ||
         strcmp(member, "counter_reset") == 0))
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

/* Validate import resolution independently of value emission.  Release CGen
 * deliberately erases dead import tokens, but link resolution is a program
 * contract and must therefore fail closed even when a token has no emitted C
 * use.  Keep this predicate aligned with xicgen_import_ref's supported
 * resolution classes. */
static bool cg_import_ref_has_aot_resolution(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v,
                                             const XiImportRef *ref) {
    if (!ctx || !f || !v || !ref || !ref->module_path || !ref->module_path[0])
        return false;

    if (ref->resolved_mod_index >= 0 && ref->resolved_mod_index < ctx->all_nmodules &&
        ctx->all_modules && ctx->all_modules[ref->resolved_mod_index]) {
        const XiModule *target = ctx->all_modules[ref->resolved_mod_index];
        if (!ref->member_name)
            return true;
        if (ref->resolved_shared_slot >= 0 && ref->resolved_shared_slot < target->nslots)
            return true;
    }

    for (int i = 0; i < ctx->nimports; i++) {
        const CgImportEntry *binding = &ctx->imports[i];
        if (!binding->module_path || strcmp(binding->module_path, ref->module_path) != 0)
            continue;
        if (!ref->member_name)
            return true;
        if (binding->member_name && strcmp(binding->member_name, ref->member_name) == 0)
            return true;
    }

    if (!ref->member_name &&
        (strcmp(ref->module_path, "time") == 0 || strcmp(ref->module_path, "math") == 0 ||
         strcmp(ref->module_path, "log") == 0 || strcmp(ref->module_path, "parallel") == 0 ||
         cg_module_has_aot_direct_calls(ref->module_path)))
        return true;
    if (xicgen_import_ref_is_core_math_member(ref))
        return true;
    if (ref->member_name &&
        (xa_builtin_get_object_shape(ref->module_path, ref->member_name) ||
         xa_builtin_get_enum_type(ref->module_path, ref->member_name) ||
         cg_aot_stdlib_has_direct_member(ref->module_path, ref->member_name) ||
         cg_aot_stdlib_generated_const_for_member(ref->module_path, ref->member_name)))
        return true;
    return cg_import_ref_has_verified_link_dependency(ctx, ref) &&
           cg_import_ref_value_is_dead_for_aot(ctx, f, v);
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

static const char *const cg_runtime_info_object_fields[] = {
    "liveBytes", "liveKB", "liveObjects", "finalizerCount", "blocks", "freeBlocks", "fullBlocks"};

static void cg_emit_runtime_info_value(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                       const char *aot_ctx) {
    const XaotValuePlan *plan = cg_value_plan(ctx, v);
    const XrAggregateLayout *layout = cg_value_struct_layout(ctx, f, v);
    if (layout && layout->field_count == 7 && plan && cg_value_rep_is_struct_aggregate(plan->rep) &&
        plan->rep.c_type) {
        char fields[7][128];
        for (uint16_t i = 0; i < 7; i++)
            cg_struct_field_c_name(layout, i, fields[i], sizeof(fields[i]));
        fprintf(out,
                "({ XrAotRuntimeInfo _ri = xr_aot_runtime_info(%s); "
                "(%s){ .%s = _ri.live_bytes, .%s = _ri.live_kb, "
                ".%s = _ri.live_objects, .%s = _ri.finalizer_count, "
                ".%s = _ri.blocks, .%s = _ri.free_blocks, .%s = _ri.full_blocks }; })",
                aot_ctx, plan->rep.c_type, fields[0], fields[1], fields[2], fields[3], fields[4],
                fields[5], fields[6]);
        return;
    }

    if (cg_value_plan_storage_rep(ctx, v) != XR_REP_TAGGED) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: runtime.info has no verified AOT object layout\n");
        emit_codegen_abort_expr(out);
        return;
    }
    const XrType *shape_type =
        v && XR_TYPE_HAS_OBJECT_SHAPE(v->type) && v->type->object.field_count == 7 ? v->type : NULL;
    int shape_id = cg_intern_object_shape_parts(ctx, 7, cg_runtime_info_object_fields, shape_type,
                                                XR_OBJECT_DOMAIN_STRUCT);
    if (shape_id < 0) {
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }
    fprintf(out,
            "({ XrAotRuntimeInfo _ri = xr_aot_runtime_info(%s); "
            "XrValue _riv = xrt_object_new_shape(&_xobj_shape_%d); "
            "xrt_object_set_field(_riv, 0, XR_FROM_INT(_ri.live_bytes)); "
            "xrt_object_set_field(_riv, 1, XR_FROM_FLOAT(_ri.live_kb)); "
            "xrt_object_set_field(_riv, 2, XR_FROM_INT(_ri.live_objects)); "
            "xrt_object_set_field(_riv, 3, XR_FROM_INT(_ri.finalizer_count)); "
            "xrt_object_set_field(_riv, 4, XR_FROM_INT(_ri.blocks)); "
            "xrt_object_set_field(_riv, 5, XR_FROM_INT(_ri.free_blocks)); "
            "xrt_object_set_field(_riv, 6, XR_FROM_INT(_ri.full_blocks)); "
            "_riv; })",
            aot_ctx, shape_id);
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
    const char *helper = NULL;
    if (strcmp(method, "liveBytes") == 0)
        helper = "xr_aot_runtime_live_bytes";
    else if (strcmp(method, "liveObjects") == 0)
        helper = "xr_aot_runtime_live_objects";
    else if (strcmp(method, "sharedBytes") == 0)
        helper = "xr_aot_runtime_shared_bytes";
    else if (strcmp(method, "staticBytes") == 0)
        helper = "xr_aot_runtime_static_bytes";
    if (helper) {
        const char *suffix =
            emit_conversion_prefix(out, v->type, XR_REP_I64, cg_value_plan_storage_rep(ctx, v));
        fprintf(out, "%s(%s)", helper, aot_ctx);
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

static bool cg_emit_test_yield_sync_call(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                         const XiValue *v, const char *method, uint16_t argc,
                                         uint16_t arg_base) {
    (void) f;
    if (!method)
        return false;

    const char *helper = NULL;
    uint16_t expected_argc = 0;
    if (strcmp(method, "sync") == 0)
        helper = "xr_aot_test_yield_sync";
    else if (strcmp(method, "blocking_sleep") == 0) {
        helper = "xr_aot_test_yield_blocking_sleep";
        expected_argc = 1;
    } else if (strcmp(method, "counter_get") == 0)
        helper = "xr_aot_test_yield_counter_get";
    else if (strcmp(method, "counter_reset") == 0)
        helper = "xr_aot_test_yield_counter_reset";
    else if (strcmp(method, "simple") == 0 || strcmp(method, "add") == 0 ||
             strcmp(method, "counter_inc") == 0) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: yieldable AOT test_yield call '%s' escaped coroutine lowering\n",
                method);
        emit_codegen_abort_expr(out);
        return true;
    } else {
        return false;
    }

    if (argc != expected_argc) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT test_yield call '%s' with %u args\n",
                method, (unsigned) argc);
        emit_codegen_abort_expr(out);
        return true;
    }
    const char *suffix =
        emit_conversion_prefix(out, v->type, XR_REP_I64, cg_value_plan_storage_rep(ctx, v));
    fprintf(out, "%s(", helper);
    if (expected_argc == 1)
        emit_value_as_rep_ctx(ctx, out, v->args[arg_base], XR_REP_I64);
    fprintf(out, ")");
    emit_conversion_suffix(out, suffix);
    return true;
}

static bool xicgen_emit_test_yield_method(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const XiValue *v) {
    if (!v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) || v->nargs < 1 ||
        !cg_value_is_module_import_ctx(ctx, f, v->args[0], "test_yield"))
        return false;
    return cg_emit_test_yield_sync_call(ctx, out, f, v, (const char *) v->aux,
                                        (uint16_t) (v->nargs - 1), 1);
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
 * string accessors; other args ('v'/'i') pass through as tagged values. SSA
 * arg references have no side effects, so emitting one twice (data + length)
 * is safe. stdlibgen guarantees the spec covers every fixed argument; a NULL
 * or exhausted spec (variadic rows, or call_argc beyond the spec length) must
 * still never be indexed past its NUL, so those positions take the default
 * tagged form. */
static void cg_emit_aot_stdlib_args(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                    const CgAotStdlibMethod *m, uint16_t call_argc,
                                    uint16_t arg_base) {
    (void) f;
    const char *spec_cursor = m->arg_spec ? m->arg_spec : "";
    for (uint16_t a = 0; a < call_argc; a++) {
        const XiValue *arg = v->args[arg_base + a];
        char spec = *spec_cursor;
        if (spec != '\0')
            spec_cursor++;
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
    const XrType *object_type = v ? v->type : NULL;
    bool layout_ok = layout && layout->field_count == 2 &&
                     layout->fields[0].native_type == XR_NATIVE_I64 &&
                     layout->fields[1].native_type == XR_NATIVE_I64;
    bool object_ok = object_type && XR_TYPE_IS_STRUCT_OBJECT(object_type) &&
                     object_type->object.field_count == 2 && object_type->object.field_names &&
                     object_type->object.field_types && object_type->object.field_types[0] &&
                     object_type->object.field_types[1] &&
                     XR_TYPE_IS_INT(object_type->object.field_types[0]) &&
                     XR_TYPE_IS_INT(object_type->object.field_types[1]);
    if (!ctx || !out || !v || !m || (!layout_ok && !object_ok) || !m->enum_name ||
        !m->variant_names || m->variant_count == 0) {
        fprintf(stderr,
                "[xi_cgen] ERROR: invalid i64-pair result contract for %s.%s "
                "(layout=%p fields=%u error-enum=%s variants=%u)\n",
                m && m->module ? m->module : "?", m && m->method ? m->method : "?",
                (const void *) layout,
                layout ? (unsigned) layout->field_count
                       : (XR_TYPE_HAS_OBJECT_SHAPE(object_type)
                              ? (unsigned) object_type->object.field_count
                              : 0),
                m && m->enum_name ? m->enum_name : "?", m ? (unsigned) m->variant_count : 0);
        if (ctx)
            ctx->error = true;
        emit_codegen_abort_expr(out);
        return true;
    }

    unsigned id = v->id;
    int first_layout_ordinal = 0;
    int second_layout_ordinal = 1;
    const char *first_name = object_ok ? object_type->object.field_names[0] : NULL;
    const char *second_name = object_ok ? object_type->object.field_names[1] : NULL;
    if (layout && layout->field_names && first_name && second_name) {
        first_layout_ordinal = -1;
        second_layout_ordinal = -1;
        for (uint16_t i = 0; i < layout->field_count; i++) {
            if (layout->field_names[i] && strcmp(layout->field_names[i], first_name) == 0)
                first_layout_ordinal = (int) i;
            if (layout->field_names[i] && strcmp(layout->field_names[i], second_name) == 0)
                second_layout_ordinal = (int) i;
        }
        if (first_layout_ordinal < 0 || second_layout_ordinal < 0) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: i64-pair layout lost semantic field names\n");
            emit_codegen_abort_expr(out);
            return true;
        }
    }
    char field0[128];
    char field1[128];
    if (layout) {
        cg_struct_field_c_name(layout, first_layout_ordinal, field0, sizeof(field0));
        cg_struct_field_c_name(layout, second_layout_ordinal, field1, sizeof(field1));
    }

    fprintf(out, "XrtI64PairResult _arp%u = %s(", id, m->shim);
    cg_emit_aot_stdlib_args(ctx, out, f, v, m, call_argc, arg_base);
    fprintf(out,
            "); if (_arp%u.error_index >= 0) { uint32_t _are%u = "
            "(uint32_t)_arp%u.error_index; if (_are%u >= UINT32_C(%u)) "
            "{ fputs(\"invalid direct stdlib error ordinal\\n\", stderr); abort(); } "
            "xrt_pending_error = "
            "xrt_enum_box_new(UINT32_C(%u), ",
            id, id, id, id, (unsigned) m->variant_count, (unsigned) m->enum_layout_id);
    emit_c_string_literal(out, m->enum_name);
    fprintf(out, ", ");
    fprintf(out, "((const char *const[]){");
    for (uint16_t i = 0; i < m->variant_count; i++) {
        if (i > 0)
            fprintf(out, ", ");
        emit_c_string_literal(out, m->variant_names[i]);
    }
    fprintf(out, "})[_are%u], _are%u); } ", id, id);

    const XaotValuePlan *plan = cg_value_plan(ctx, v);
    if (layout && plan && cg_value_rep_is_struct_aggregate(plan->rep) && plan->rep.c_type) {
        fprintf(out, "(%s){ .%s = _arp%u.first, .%s = _arp%u.second }; })", plan->rep.c_type,
                field0, id, field1, id);
        return true;
    }

    fprintf(out, "XrValue _arr%u = ", id);
    if (object_ok) {
        /* Store each pair half into the slot its field name sorts to under the
         * canonical order the evidence table and the structural field-table
         * verifier use, so a verified field read on the result lands on the
         * value it named. The declared order is (first, second); the shape
         * interner sorts the names, which may swap the two slots. */
        const char *const *decl_names = (const char *const *) object_type->object.field_names;
        uint64_t key_first = xg_object_stable_name_key(decl_names[0]);
        uint64_t key_second = xg_object_stable_name_key(decl_names[1]);
        bool first_leads =
            key_first < key_second ||
            (key_first == key_second && xg_name_id(decl_names[0]) <= xg_name_id(decl_names[1]));
        int slot_first = first_leads ? 0 : 1;
        int slot_second = first_leads ? 1 : 0;
        int shape_id =
            cg_intern_object_shape_type_domain(ctx, object_type, XR_OBJECT_DOMAIN_STRUCT);
        if (shape_id < 0) {
            ctx->error = true;
            emit_codegen_abort_expr(out);
            return true;
        }
        fprintf(out,
                "xrt_object_new_shape(&_xobj_shape_%d); "
                "xrt_object_set_field(_arr%u, %d, XR_FROM_INT(_arp%u.first)); "
                "xrt_object_set_field(_arr%u, %d, XR_FROM_INT(_arp%u.second)); _arr%u; })",
                shape_id, id, slot_first, id, id, slot_second, id, id);
        return true;
    }
    int shape_id = cg_intern_object_shape_parts(ctx, 2, (const char *const *) layout->field_names,
                                                NULL, XR_OBJECT_DOMAIN_STRUCT);
    if (shape_id < 0) {
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return true;
    }
    int first_shape_ordinal = 0;
    int second_shape_ordinal = 1;
    if (object_ok) {
        const CgObjectShape *shape = &ctx->object_shapes[shape_id];
        first_shape_ordinal = -1;
        second_shape_ordinal = -1;
        for (int64_t i = 0; i < shape->field_count; i++) {
            if (strcmp(shape->field_names[i], first_name) == 0)
                first_shape_ordinal = (int) i;
            if (strcmp(shape->field_names[i], second_name) == 0)
                second_shape_ordinal = (int) i;
        }
        if (first_shape_ordinal < 0 || second_shape_ordinal < 0) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: i64-pair shape lost semantic field names\n");
            emit_codegen_abort_expr(out);
            return true;
        }
    }
    fprintf(out,
            "xrt_object_new_shape(&_xobj_shape_%d); "
            "xrt_object_set_field(_arr%u, %d, XR_FROM_INT(_arp%u.first)); "
            "xrt_object_set_field(_arr%u, %d, XR_FROM_INT(_arp%u.second)); _arr%u; })",
            shape_id, id, first_shape_ordinal, id, id, second_shape_ordinal, id, id);
    return true;
}

static bool cg_emit_aot_stdlib_direct_call(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                           const XiValue *v, const CgAotStdlibMethod *m,
                                           uint16_t call_argc, uint16_t arg_base) {
    if (!ctx || !out || !v || !m)
        return false;

    XrRep target_rep = xicgen_value_c_storage_rep(ctx, f, v);

    if (m->ret_kind == CG_AOT_RET_I64) {
        const char *suffix = emit_conversion_prefix_ctx(ctx, out, v->type, XR_REP_I64, target_rep);
        fprintf(out, "%s(", m->shim);
        cg_emit_aot_stdlib_args(ctx, out, f, v, m, call_argc, arg_base);
        fprintf(out, ")");
        emit_conversion_suffix(out, suffix);
        return true;
    }

    if (m->ret_kind == CG_AOT_RET_ENUM_I64) {
        if (target_rep == XR_REP_I64) {
            fprintf(out, "%s(", m->shim);
            cg_emit_aot_stdlib_args(ctx, out, f, v, m, call_argc, arg_base);
            fprintf(out, ")");
            return true;
        }
        uint32_t index = 0;
        if (!cg_mark_aot_stdlib_enum_scalar_sidecar(ctx, m, &index)) {
            emit_codegen_abort_expr(out);
            return true;
        }
        const char *module_prefix =
            ctx->module && ctx->module->name && ctx->module->name[0] ? ctx->module->name : "mod";
        const char *suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, target_rep);
        fprintf(out, "_xaot_stdlib_enum_box_%s_%u(%s(", module_prefix, (unsigned) index, m->shim);
        cg_emit_aot_stdlib_args(ctx, out, f, v, m, call_argc, arg_base);
        fprintf(out, "))");
        emit_conversion_suffix(out, suffix);
        return true;
    }

    /* The common value-returning ABI is already declared by the layered xrt
     * headers.  Emit it as an ordinary C expression so hosted fragments remain
     * valid ISO C11 and compile with MSVC as well as GCC/Clang. */
    if (m->ret_kind == CG_AOT_RET_VALUE && !cg_aot_stdlib_method_is_variadic_strings(m) &&
        (!m->extern_decl || !m->extern_decl[0])) {
        const char *suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, target_rep);
        fprintf(out, "%s(", m->shim);
        cg_emit_aot_stdlib_args(ctx, out, f, v, m, call_argc, arg_base);
        fprintf(out, ")");
        emit_conversion_suffix(out, suffix);
        return true;
    }

    /* Complex result adapters still require statement-local temporaries. They
     * are rejected by the hosted-fragment residue gate until their portable
     * statement forms are lowered explicitly. */
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
    if (strcmp(ref->module_path, "test_yield") == 0)
        return cg_emit_test_yield_sync_call(ctx, out, f, v, ref->member_name, call_argc, 1);

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
