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
    CG_AOT_RET_VALUE,        /* tagged XrValue, converted to the call's rep */
    CG_AOT_RET_STR_BORROWED, /* (const char *data, int64_t *out_len) slice into an
                              * input/static buffer; copied into an AOT string */
} CgAotRetKind;

typedef struct CgAotStdlibMethod {
    const char *module; /* stdlib module identifier (e.g. "path") */
    const char *method; /* method name (e.g. "isAbsolute") */
    uint16_t argc;      /* argument count excluding the receiver */
    const char *shim;   /* xr_aot_<module>_<method> runtime symbol */
    /* One character per argument describing how it is passed to the shim:
     *   's' = string, lowered to specialized (const char *data, int64_t len)
     *   'v' = tagged XrValue passed as-is */
    const char *arg_spec;
    CgAotRetKind ret_kind;
    const char *extern_decl; /* forward declaration emitted into generated C */
} CgAotStdlibMethod;

static const CgAotStdlibMethod g_aot_stdlib_methods[] = {
    {"path", "isAbsolute", 1, "xrt_path_is_absolute", "s", CG_AOT_RET_VALUE, NULL},
    {"path", "dirname", 1, "xrt_path_dirname", "s", CG_AOT_RET_STR_BORROWED, NULL},
    {"path", "basename", 1, "xrt_path_basename", "s", CG_AOT_RET_STR_BORROWED, NULL},
    {"path", "extname", 1, "xrt_path_extname", "s", CG_AOT_RET_STR_BORROWED, NULL},
};

#define CG_AOT_STDLIB_METHOD_COUNT                                                                 \
    ((int) (sizeof(g_aot_stdlib_methods) / sizeof(g_aot_stdlib_methods[0])))

/* Whether `module` is a stdlib module with AOT direct-call support. The
 * import-ref emitter uses this to materialize the module object as an
 * XR_NULL_VAL placeholder (the real work happens at the call sites). */
static bool cg_module_has_aot_direct_calls(const char *module) {
    if (!module)
        return false;
    for (int i = 0; i < CG_AOT_STDLIB_METHOD_COUNT; i++) {
        if (strcmp(module, g_aot_stdlib_methods[i].module) == 0)
            return true;
    }
    return false;
}

static const CgAotStdlibMethod *cg_find_aot_stdlib_method(const char *module, const char *method,
                                                          uint16_t argc) {
    if (!module || !method)
        return NULL;
    for (int i = 0; i < CG_AOT_STDLIB_METHOD_COUNT; i++) {
        const CgAotStdlibMethod *m = &g_aot_stdlib_methods[i];
        if (m->argc == argc && strcmp(module, m->module) == 0 && strcmp(method, m->method) == 0)
            return m;
    }
    return NULL;
}

/* Resolve the AOT-managed stdlib module that a method-call receiver names. */
static const char *cg_aot_stdlib_module_of_receiver(const XiCgenCtx *ctx, const XiFunc *f,
                                                    const XiValue *recv) {
    for (int i = 0; i < CG_AOT_STDLIB_METHOD_COUNT; i++) {
        const char *module = g_aot_stdlib_methods[i].module;
        if (cg_value_is_module_import_ctx(ctx, f, recv, module))
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
                                    const CgAotStdlibMethod *m, uint16_t call_argc) {
    (void) f;
    for (uint16_t a = 0; a < call_argc; a++) {
        const XiValue *arg = v->args[1 + a];
        char spec = m->arg_spec[a];
        if (a > 0)
            fprintf(out, ", ");
        if (spec == 's') {
            fprintf(out, "xr_str_data(");
            emit_value_as_rep_ctx(ctx, out, arg, XR_REP_TAGGED);
            fprintf(out, "), xr_str_len(");
            emit_value_as_rep_ctx(ctx, out, arg, XR_REP_TAGGED);
            fprintf(out, ")");
        } else {
            emit_value_as_rep_ctx(ctx, out, arg, XR_REP_TAGGED);
        }
    }
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

    /* Wrap in a statement-expression so the shim's forward declaration is
     * self-contained at the call site, then convert the result to the value's
     * required storage representation. */
    XrRep target_rep = xicgen_value_c_storage_rep(ctx, f, v);
    fprintf(out, "({ ");
    if (m->extern_decl && m->extern_decl[0])
        fprintf(out, "%s ", m->extern_decl);

    if (m->ret_kind == CG_AOT_RET_STR_BORROWED) {
        /* The shim returns a borrowed (data, *out_len) slice; copy it into a
         * fresh AOT string. Temps are id-suffixed to nest safely. */
        unsigned id = v->id;
        fprintf(out, "int64_t _arl%u = 0; const char *_ard%u = %s(", id, id, m->shim);
        cg_emit_aot_stdlib_args(ctx, out, f, v, m, call_argc);
        fprintf(out, ", &_arl%u); XrValue _ars%u = xrt_str_alloc((size_t) _arl%u); ", id, id, id);
        fprintf(out, "if (_arl%u) memcpy(xr_str_buf(_ars%u), _ard%u, (size_t) _arl%u); ", id, id,
                id, id);
        const char *suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, target_rep);
        fprintf(out, "_ars%u", id);
        emit_conversion_suffix(out, suffix);
        fprintf(out, "; })");
        return true;
    }

    /* CG_AOT_RET_VALUE: shim returns a tagged XrValue. */
    const char *suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, target_rep);
    fprintf(out, "%s(", m->shim);
    cg_emit_aot_stdlib_args(ctx, out, f, v, m, call_argc);
    fprintf(out, ")");
    emit_conversion_suffix(out, suffix);
    fprintf(out, "; })");
    return true;
}
