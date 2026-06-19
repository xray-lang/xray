/*
 * xi_cgen_stmt_dispatch_helpers.inc.c - Generated Xi statement lowering drivers for AOT C
 */

static uint32_t xicgen_stmt_find_try_id(const XiFunc *f) {
    uint32_t try_id = 0;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *tv = blk->values[vi];
            if (tv && tv->op == XI_TRY)
                try_id = tv->id;
        }
    }
    return try_id;
}

static bool xicgen_stmt_try(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                            const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    const XiBlock *catch_blk = (const XiBlock *) v->aux;
    fprintf(out, "    XrtExcFrame _ef%u;\n", v->id);
    fprintf(out, "    _ef%u.prev = xrt_exc_top;\n", v->id);
    fprintf(out, "    xrt_exc_top = &_ef%u;\n", v->id);
    if (catch_blk) {
        fprintf(out,
                "    if (setjmp(_ef%u.buf) != 0) {"
                " xrt_exc_top = _ef%u.prev; goto L%u; }\n",
                v->id, v->id, catch_blk->id);
    } else {
        fprintf(out,
                "    if (setjmp(_ef%u.buf) != 0) {"
                " xrt_exc_top = _ef%u.prev; }\n",
                v->id, v->id);
    }
    return true;
}

static bool xicgen_stmt_end_try(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                const char *prefix) {
    (void) ctx;
    (void) v;
    (void) prefix;
    fprintf(out, "    xrt_exc_top = _ef%u.prev;\n", xicgen_stmt_find_try_id(f));
    return true;
}

static bool xicgen_stmt_catch(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) prefix;
    fprintf(out, "    ");
    if (!ctx->pre_decl_all) {
        fprintf(out, "%s ", ctype_str(cg_rep(v)));
        emit_vref(out, v);
        fprintf(out, " = ");
    } else {
        emit_vref(out, v);
        fprintf(out, " = ");
    }
    fprintf(out, "_ef%u.exception;\n", xicgen_stmt_find_try_id(f));
    return true;
}

static bool xicgen_stmt_defer(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) ctx;
    (void) out;
    (void) f;
    (void) v;
    (void) prefix;
    return true;
}

static bool xicgen_stmt_err_set(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_stmt_err_set: missing error value");
    fprintf(out, "    xrt_pending_error = ");
    emit_vref(out, v->args[0]);
    fprintf(out, ";\n");
    return true;
}

static bool xicgen_stmt_err_return(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                   const char *prefix) {
    XR_DCHECK(v->nargs >= 1, "xicgen_stmt_err_return: missing error value");
    fprintf(out, "    xrt_pending_error = ");
    emit_vref(out, v->args[0]);
    fprintf(out, ";\n");
    emit_class_field_cache_flush(ctx, out);
    emit_deferred_calls(ctx, out, f, prefix);
    emit_cell_var_releases(ctx, out);
    fprintf(out, "    return ");
    emit_default_return_for_abi(ctx, out, f);
    fprintf(out, ";\n");
    return true;
}

static bool xicgen_stmt_err_check(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                  const char *prefix) {
    if (cg_value_type_is_bool(v)) {
        XrRep rep = cg_rep(v);
        fprintf(out, "    ");
        if (!ctx->pre_decl_all) {
            fprintf(out, "%s ", ctype_str(rep));
            emit_vref(out, v);
            fprintf(out, " = ");
        } else {
            emit_vref(out, v);
            fprintf(out, " = ");
        }
        if (rep == XR_REP_TAGGED)
            fprintf(out, "XR_FROM_BOOL(xrt_has_pending_error());\n");
        else
            fprintf(out, "xrt_has_pending_error();\n");
        return true;
    }

    if (cg_array_err_check_after_unchecked_fill_push(ctx, f, v) ||
        cg_array_err_check_after_typed_push(ctx, f, v) ||
        cg_array_err_check_after_inline_hof(ctx, f, prefix, v) ||
        cg_class_native_err_check_after_nothrow_call(ctx, f, v))
        return true;

    fprintf(out, "    if (XR_UNLIKELY(xrt_has_pending_error())) {\n");
    emit_class_field_cache_flush(ctx, out);
    emit_deferred_calls(ctx, out, f, prefix);
    emit_cell_var_releases(ctx, out);
    fprintf(out, "        return ");
    emit_default_return_for_abi(ctx, out, f);
    fprintf(out, ";\n");
    fprintf(out, "    }\n");
    return true;
}

static bool xicgen_stmt_err_catch(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                  const char *prefix) {
    (void) f;
    (void) prefix;
    fprintf(out, "    ");
    if (!ctx->pre_decl_all) {
        fprintf(out, "%s ", ctype_str(cg_rep(v)));
        emit_vref(out, v);
        fprintf(out, " = ");
    } else {
        emit_vref(out, v);
        fprintf(out, " = ");
    }
    fprintf(out, "xrt_pending_error;\n");
    fprintf(out, "    xrt_pending_error = XR_NULL_VAL;\n");
    return true;
}

static bool xi_to_c_emit_stmt_generated(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const XiValue *v, const char *prefix) {
    switch (v->op) {
#define XICGEN_STMT_GENERATED_CASE(op, name, driver)                                               \
    case XI_##op:                                                                                  \
        (void) name;                                                                               \
        return driver(ctx, out, f, v, prefix);
        XI_TO_C_STMT_LOWERING_DRIVERS(XICGEN_STMT_GENERATED_CASE)
#undef XICGEN_STMT_GENERATED_CASE
        case XI_OP_COUNT:
            break;
    }
    return false;
}
