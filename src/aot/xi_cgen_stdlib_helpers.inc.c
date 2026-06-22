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

#define CG_AOT_STDLIB_VARIADIC UINT16_MAX

typedef struct CgAotStdlibMethod {
    const char *module; /* stdlib module identifier (e.g. "path") */
    const char *method; /* method name (e.g. "isAbsolute") */
    uint16_t argc;      /* argument count excluding the receiver */
    const char *shim;   /* xr_aot_<module>_<method> runtime symbol */
    /* One character per argument describing how it is passed to the shim:
     *   's' = string, lowered to specialized (const char *data, int64_t len)
     *   'v' = tagged XrValue passed as-is
     *   '*' = variadic strings, lowered to (argc, data[], len[]) */
    const char *arg_spec;
    CgAotRetKind ret_kind;
    const char *extern_decl; /* forward declaration emitted into generated C */
} CgAotStdlibMethod;

#include "xstdlib_aot_methods_generated.inc.c"

static const CgAotStdlibMethod g_aot_stdlib_manual_methods[] = {
    {"io", "appendFile", 2, "xrt_io_append_file", "ss", CG_AOT_RET_VALUE, NULL},
    {"io", "chmod", 2, "xrt_io_chmod_value", "sv", CG_AOT_RET_VALUE, NULL},
    {"io", "chdir", 1, "xrt_io_chdir", "s", CG_AOT_RET_VALUE, NULL},
    {"io", "copyFile", 2, "xrt_io_copy_file", "ss", CG_AOT_RET_VALUE, NULL},
    {"io", "cwd", 0, "xrt_io_cwd", "", CG_AOT_RET_VALUE, NULL},
    {"io", "exists", 1, "xrt_io_exists", "s", CG_AOT_RET_VALUE, NULL},
    {"io", "fileSize", 1, "xrt_io_file_size", "s", CG_AOT_RET_VALUE, NULL},
    {"io", "isDir", 1, "xrt_io_is_dir", "s", CG_AOT_RET_VALUE, NULL},
    {"io", "isFile", 1, "xrt_io_is_file", "s", CG_AOT_RET_VALUE, NULL},
    {"io", "isSymlink", 1, "xrt_io_is_symlink", "s", CG_AOT_RET_VALUE, NULL},
    {"io", "mkdir", 1, "xrt_io_mkdir", "s", CG_AOT_RET_VALUE, NULL},
    {"io", "mkdirp", 1, "xrt_io_mkdirp", "s", CG_AOT_RET_VALUE, NULL},
    {"io", "readDir", 1, "xrt_io_read_dir", "s", CG_AOT_RET_VALUE, NULL},
    {"io", "readDirRecursive", 1, "xrt_io_read_dir_recursive", "s", CG_AOT_RET_VALUE, NULL},
    {"io", "readFile", 1, "xrt_io_read_file", "s", CG_AOT_RET_VALUE, NULL},
    {"io", "readFileBytes", 1, "xrt_io_read_file_bytes", "s", CG_AOT_RET_VALUE, NULL},
    {"io", "readLines", 1, "xrt_io_read_lines", "s", CG_AOT_RET_VALUE, NULL},
    {"io", "readStdin", 0, "xrt_io_read_stdin", "", CG_AOT_RET_VALUE, NULL},
    {"io", "readlink", 1, "xrt_io_readlink", "s", CG_AOT_RET_VALUE, NULL},
    {"io", "realpath", 1, "xrt_io_realpath", "s", CG_AOT_RET_VALUE, NULL},
    {"io", "remove", 1, "xrt_io_remove", "s", CG_AOT_RET_VALUE, NULL},
    {"io", "removeAll", 1, "xrt_io_remove_all", "s", CG_AOT_RET_VALUE, NULL},
    {"io", "rename", 2, "xrt_io_rename", "ss", CG_AOT_RET_VALUE, NULL},
    {"io", "stat", 1, "xrt_io_stat", "s", CG_AOT_RET_VALUE, NULL},
    {"io", "symlink", 2, "xrt_io_symlink", "ss", CG_AOT_RET_VALUE, NULL},
    {"io", "tempDir", 0, "xrt_io_temp_dir", "", CG_AOT_RET_VALUE, NULL},
    {"io", "tempFile", 0, "xrt_io_temp_file", "", CG_AOT_RET_VALUE, NULL},
    {"io", "touch", 1, "xrt_io_touch", "s", CG_AOT_RET_VALUE, NULL},
    {"io", "writeFile", 2, "xrt_io_write_file", "ss", CG_AOT_RET_VALUE, NULL},
    {"io", "writeFileBytes", 2, "xrt_io_write_file_bytes", "sv", CG_AOT_RET_VALUE, NULL},
};

#define CG_AOT_STDLIB_MANUAL_METHOD_COUNT                                                          \
    ((int) (sizeof(g_aot_stdlib_manual_methods) / sizeof(g_aot_stdlib_manual_methods[0])))

static const CgAotStdlibMethod *cg_aot_stdlib_generated_method_at(int index) {
    if (index < 0 || index >= CG_AOT_STDLIB_GENERATED_METHOD_COUNT)
        return NULL;
    return &g_aot_stdlib_generated_methods[index];
}

static const CgAotStdlibMethod *cg_aot_stdlib_manual_method_at(int index) {
    if (index < 0 || index >= CG_AOT_STDLIB_MANUAL_METHOD_COUNT)
        return NULL;
    return &g_aot_stdlib_manual_methods[index];
}

static const CgAotStdlibMethod *cg_aot_stdlib_method_at(int index) {
    const CgAotStdlibMethod *m = cg_aot_stdlib_generated_method_at(index);
    if (m)
        return m;
    return cg_aot_stdlib_manual_method_at(index - CG_AOT_STDLIB_GENERATED_METHOD_COUNT);
}

static int cg_aot_stdlib_method_count(void) {
    return CG_AOT_STDLIB_GENERATED_METHOD_COUNT + CG_AOT_STDLIB_MANUAL_METHOD_COUNT;
}

/* Whether `module` is a stdlib module with AOT direct-call support. The
 * import-ref emitter uses this to materialize the module object as an
 * XR_NULL_VAL placeholder (the real work happens at the call sites). */
static bool cg_module_has_aot_direct_calls(const char *module) {
    if (!module)
        return false;
    for (int i = 0; i < cg_aot_stdlib_method_count(); i++) {
        const CgAotStdlibMethod *m = cg_aot_stdlib_method_at(i);
        if (m && strcmp(module, m->module) == 0)
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
        fprintf(out, "INT64_C(%" PRId64 ")", c->i64_value);
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

static bool cg_aot_stdlib_method_is_variadic_strings(const CgAotStdlibMethod *m) {
    return m && m->argc == CG_AOT_STDLIB_VARIADIC && m->arg_spec && strcmp(m->arg_spec, "*") == 0;
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

    if (cg_aot_stdlib_method_is_variadic_strings(m)) {
        unsigned id = v->id;
        if (call_argc > 0) {
            fprintf(out, "const char *_asd%u[%u] = {", id, (unsigned) call_argc);
            for (uint16_t a = 0; a < call_argc; a++) {
                if (a > 0)
                    fprintf(out, ", ");
                fprintf(out, "xr_str_data(");
                emit_value_as_rep_ctx(ctx, out, v->args[1 + a], XR_REP_TAGGED);
                fprintf(out, ")");
            }
            fprintf(out, "}; size_t _asl%u[%u] = {", id, (unsigned) call_argc);
            for (uint16_t a = 0; a < call_argc; a++) {
                if (a > 0)
                    fprintf(out, ", ");
                fprintf(out, "(size_t) xr_str_len(");
                emit_value_as_rep_ctx(ctx, out, v->args[1 + a], XR_REP_TAGGED);
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
        cg_emit_aot_stdlib_args(ctx, out, f, v, m, call_argc);
        fprintf(out, ")");
        emit_conversion_suffix(out, suffix);
    }
    fprintf(out, "; })");
    return true;
}
