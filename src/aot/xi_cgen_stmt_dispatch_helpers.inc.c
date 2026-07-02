/*
 * xi_cgen_stmt_dispatch_helpers.inc.c - Generated Xi statement lowering drivers for AOT C
 */

static uint32_t xicgen_stmt_bound_try_id(const XiFunc *f, const XiValue *v) {
    if (v && v->aux) {
        const XiValue *try_op = (const XiValue *) v->aux;
        if (try_op->op == XI_TRY)
            return try_op->id;
    }

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
    /* Record both the active defer scope and its count at try entry. A caught
     * panic unwinds skipped functions and then runs this same scope back to the
     * count mark, so block-scoped defers inside the try do not leak to the
     * enclosing block. */
    fprintf(out, "    _ef%u.defer_scope_mark = xrt_defer_top;\n", v->id);
    fprintf(out, "    _ef%u.defer_count_mark = xrt_defer_top ? xrt_defer_top->count : 0;\n", v->id);
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
    fprintf(out, "    xrt_exc_top = _ef%u.prev;\n", xicgen_stmt_bound_try_id(f, v));
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
    fprintf(out, "_ef%u.exception;\n", xicgen_stmt_bound_try_id(f, v));
    return true;
}

static bool xicgen_stmt_defer(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    /* Register the deferred closure onto this function's defer scope. The IR
     * desugars every `defer` into a zero-arg closure that eagerly captures its
     * operands, so registration is a single push; the scope runs it LIFO at
     * exit (emit_deferred_calls) or on panic unwind (xrt_throw_exc). XI_DEFER
     * consumes the closure, so the scope owns this reference. */
    if (!v || v->nargs < 1)
        return true;
    fprintf(out, "    xrt_defer_push(&_xrt_ds, ");
    emit_value_as_rep(out, cg_unwrap_identity_value(v->args[0]), XR_REP_TAGGED);
    fprintf(out, ");\n");
    return true;
}

static bool xicgen_stmt_defer_run_to(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                     const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    if (!v || v->nargs < 1)
        return true;
    fprintf(out, "    xrt_defer_run_to(&_xrt_ds, (int)");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, ");\n");
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
    emit_default_return_stmt_for_abi(ctx, out, f);
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

    if (xicgen_err_check_after_proven_nothrow(ctx, f, v) ||
        cg_array_err_check_after_unchecked_fill_push(ctx, f, v) ||
        cg_array_err_check_after_unchecked_bytes_trusted(ctx, f, v) ||
        cg_array_err_check_after_typed_push(ctx, f, v) ||
        cg_array_err_check_after_inline_hof(ctx, f, prefix, v) ||
        xicgen_atomic_err_check_after_direct_nothrow(v) ||
        cg_class_native_err_check_after_nothrow_call(ctx, f, v))
        return true;

    fprintf(out, "    if (XR_UNLIKELY(xrt_has_pending_error())) {\n");
    emit_class_field_cache_flush(ctx, out);
    emit_deferred_calls(ctx, out, f, prefix);
    emit_cell_var_releases(ctx, out);
    if (cg_func_return_abi_rep(ctx, f) == XR_REP_VOID) {
        fprintf(out, "        return;\n");
    } else {
        fprintf(out, "        return ");
        emit_default_return_for_abi(ctx, out, f);
        fprintf(out, ";\n");
    }
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
